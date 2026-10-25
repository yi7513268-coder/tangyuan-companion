#include "port.h"
#include "wire.h"
#include "robot.h"
#include "board_page.h"
#include "computer_io.h"
#include "agent_extra.h"
#include "relative_timer.h"
#include "turn_trace.h"
#include "local_dialogue.h"
#include "voice_control.h"
#include "memory/session_mgr.h"
#include "interaction.h"
#include "interaction_intent.h"
#include "agent/agent_loop.h"
#include "tools/tool_registry.h"
#include "bus/message_bus.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static nvs_handle_t data_nvs,secret_nvs;
static atomic_bool cancelled=false,busy=false,online=false;
static atomic_llong task_deadline;
static atomic_bool timed_out=false;
static atomic_bool listening_style=false;
static char api_key[257],model[97],token[65],final_text[4096],ip[24];
static atomic_uint events;
static uint32_t boot_id,task_id;
static unsigned tool_ok,tool_failed,llm_calls;
static bool current_voice_task,current_scheduled_task;
static char last_heard[1000];static const char *voice_admission="none";
static const char *preset="找3篇2025至2026年视觉手势识别论文，实际检索、核对DOI和年份，以元数据为依据比较适用性。不要编造全文实验结果。保存中文report并用read_report回读核验。";
extern void ty_screen_init(void);
const char *ty_runtime_name(void){return "ESP32-S3 robot firmware; LLM is a remote cloud API";}
bool ty_listening_style(void){return atomic_load(&listening_style);}
static void set_listening_style(bool enabled){atomic_store(&listening_style,enabled);ty_setting_put("listen_style",enabled);ty_event("listening_style","hri",true,enabled?1:0);}
bool ty_cancelled(void){
    long long deadline=atomic_load(&task_deadline);
    if(atomic_load(&busy)&&deadline&&esp_timer_get_time()>deadline)atomic_store(&timed_out,true);
    return atomic_load(&cancelled)||atomic_load(&timed_out);
}
void ty_board_cancel(void){atomic_store(&cancelled,true);ty_voice_control_clear();ty_interaction_clear();ty_audio_cancel();ty_robot_stop();}
void ty_board_quiet(void){atomic_store(&cancelled,true);ty_voice_control_clear();ty_interaction_clear();ty_robot_suspend();}
uint32_t ty_board_boot_id(void){return boot_id;}
unsigned ty_current_task_id(void){return task_id;}
bool ty_setting_get(const char *key,bool fallback){uint8_t v=0;return nvs_get_u8(data_nvs,key,&v)==ESP_OK?v!=0:fallback;}
void ty_setting_put(const char *key,bool value){if(nvs_set_u8(data_nvs,key,value?1:0)==ESP_OK)(void)nvs_commit(data_nvs);}
unsigned ty_volume_setting(unsigned value,bool write){uint8_t v=(uint8_t)value;if(write){if(nvs_set_u8(data_nvs,"volume",v)==ESP_OK)(void)nvs_commit(data_nvs);}else if(nvs_get_u8(data_nvs,"volume",&v)!=ESP_OK)v=25;return v>100?25:v;}
bool ty_board_busy(void){return atomic_load(&busy);}
const char *ty_board_ip(void){return ip;}
const char *ty_board_result(void){return final_text;}

esp_err_t ty_store_write(const char *key,const char *value){
    if((ty_board_busy()&&ty_cancelled())||strlen(key)>14||strlen(value)>=TY_STORE_MAX)return ESP_FAIL;
    if(strcmp(key,"hri")&&strcmp(key,"agent_files")&&strcmp(key,"agent_jobs")&&strcmp(key,"sources")&&strcmp(key,"skill")&&strcmp(key,"report")&&strcmp(key,"report2")&&strcmp(key,"report3"))return ESP_FAIL;
    esp_err_t ret=nvs_set_blob(data_nvs,key,value,strlen(value)+1);
    return ret==ESP_OK?nvs_commit(data_nvs):ret;
}
esp_err_t ty_store_read(const char *key,char *value,size_t size){
    size_t needed=0;esp_err_t ret=nvs_get_blob(data_nvs,key,NULL,&needed);
    if(ret!=ESP_OK||needed>size||needed<1)return ESP_FAIL;
    ret=nvs_get_blob(data_nvs,key,value,&needed);
    return ret==ESP_OK&&value[needed-1]==0?ESP_OK:ESP_FAIL;
}
void ty_event(const char *event,const char *tool,bool ok,size_t bytes){
    if(!strcmp(event,"tool_result")){if(ok)tool_ok++;else tool_failed++;}
    /* Only technical fields. No prompts, report text, API keys or HTTP payloads. */
    unsigned sequence=atomic_fetch_add(&events,1)+1;
    char record[256];snprintf(record,sizeof(record),"{\"boot_id\":%u,\"sequence\":%u,\"uptime_ms\":%lld,\"event\":\"%.32s\",\"tool\":\"%.32s\",\"ok\":%s,\"bytes\":%u,\"free_psram\":%u}",
      (unsigned)boot_id,sequence,(long long)(esp_timer_get_time()/1000),event,tool,ok?"true":"false",(unsigned)bytes,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    char key[12];snprintf(key,sizeof(key),"event%u",sequence%8);
    nvs_set_str(data_nvs,key,record);nvs_set_str(data_nvs,"evidence",record);nvs_commit(data_nvs);
    printf("TY_MIMI_EVIDENCE %s\n",record);
}
typedef struct {char *data;size_t used,limit;bool overflow;} buffer_t;
/* The single Agent task owns this cloud client. Reuse TLS during a short
 * conversation; no request replay on ambiguous network failure. */
static esp_http_client_handle_t cloud_client;
static int64_t cloud_used_at;
static esp_err_t on_http(esp_http_client_event_t *e){
    buffer_t *b=e->user_data;
    if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0){
        if(b->used+(size_t)e->data_len>b->limit){b->overflow=true;return ESP_FAIL;}
        memcpy(b->data+b->used,e->data,e->data_len);b->used+=e->data_len;b->data[b->used]=0;
    }return ESP_OK;
}
static esp_err_t http(const char *url,const char *payload,const char *auth,char **out,size_t limit){
    if(!atomic_load(&online)||ty_cancelled()){ty_event("network_or_task_unavailable","transport",false,0);return ESP_FAIL;}
    /* Certificate validity needs real wall-clock time; do not bypass TLS checks. */
    if(time(NULL)<1735689600&&esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000))!=ESP_OK){ty_event("clock_not_ready","TLS",false,0);return ESP_FAIL;}
    buffer_t b={.data=heap_caps_calloc(1,limit+1,MALLOC_CAP_SPIRAM),.limit=limit};
    if(!b.data)return ESP_ERR_NO_MEM;
    esp_http_client_config_t conf={.url=url,.timeout_ms=45000,.crt_bundle_attach=esp_crt_bundle_attach,.keep_alive_enable=true,
      .event_handler=on_http,.user_data=&b,.buffer_size=2048,.buffer_size_tx=2048,.disable_auto_redirect=true};
    if(auth&&cloud_client&&esp_timer_get_time()-cloud_used_at>20000000LL){esp_http_client_cleanup(cloud_client);cloud_client=NULL;}
    esp_http_client_handle_t h=auth&&cloud_client?cloud_client:esp_http_client_init(&conf);if(!h){free(b.data);return ESP_ERR_NO_MEM;}
    (void)esp_http_client_set_user_data(h,&b);
    esp_http_client_set_header(h,"User-Agent","Tangyuan-MimiClaw-Research/0.1");
    if(auth)esp_http_client_set_header(h,"Authorization",auth);
    if(payload){esp_http_client_set_method(h,HTTP_METHOD_POST);esp_http_client_set_header(h,"Content-Type","application/json");
        esp_http_client_set_post_field(h,payload,strlen(payload));}
    esp_err_t ret=esp_http_client_perform(h);int status=esp_http_client_get_status_code(h);
    (void)esp_http_client_set_post_field(h,NULL,0);(void)esp_http_client_set_user_data(h,NULL);
    if(auth&&ret==ESP_OK&&status==200&&!b.overflow&&!ty_cancelled()){cloud_client=h;cloud_used_at=esp_timer_get_time();}
    else{esp_http_client_cleanup(h);if(auth)cloud_client=NULL;}
    if(ret!=ESP_OK||status!=200||b.overflow||ty_cancelled()){
        ty_event(b.overflow?"http_response_limit":"http_request_failed",auth?"cloud_model":"crossref",false,status>0?(size_t)status:0);
        free(b.data);return ESP_FAIL;}
    *out=b.data;return ESP_OK;
}
esp_err_t ty_http_get(const char *url,char **out,size_t limit){
    const char *prefix="https://api.crossref.org/works";size_t n=strlen(prefix);
    if(strncmp(url,prefix,n)||(url[n]&&url[n]!='/'&&url[n]!='?'))return ESP_FAIL;
    return http(url,NULL,NULL,out,limit);
}
esp_err_t ty_llm_request(const cJSON *request,cJSON **response){
    if(!*api_key||!*model)return ESP_FAIL;
    llm_calls++;
    cJSON *payload=ty_openai_payload(request,model);if(!payload)return ESP_FAIL;
    char *text=cJSON_PrintUnformatted(payload),*body=NULL;cJSON_Delete(payload);
    if(!text)return ESP_ERR_NO_MEM;
    if(strlen(text)>96*1024){free(text);return ESP_FAIL;}
    char auth[280];snprintf(auth,sizeof(auth),"Bearer %s",api_key);
    int64_t model_start=esp_timer_get_time();
    esp_err_t ret=http("https://api.deepseek.com/chat/completions",text,auth,&body,128*1024);free(text);
    ty_trace_model(task_id,(unsigned)((esp_timer_get_time()-model_start)/1000));
    memset(auth,0,sizeof(auth));if(ret!=ESP_OK)return ret;
    cJSON *raw=cJSON_Parse(body);free(body);*response=raw?ty_openai_response(raw):NULL;cJSON_Delete(raw);
    if(!*response)ty_event("model_response_invalid","",false,0);
    return *response?ESP_OK:ESP_FAIL;
}
static void wifi_event(void *unused,esp_event_base_t base,int32_t id,void *event){
    (void)unused;static unsigned retries;
    if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_START)esp_wifi_connect();
    else if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){
        atomic_store(&online,false);if(retries++<5)esp_wifi_connect();else ty_set_status("wifi_unavailable");
    }else if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *got=event;snprintf(ip,sizeof(ip),IPSTR,IP2STR(&got->ip_info.ip));
        retries=0;atomic_store(&online,true);ty_set_status("ready");
    }
}
static esp_err_t submit(const char *text,bool voice,bool scheduled){
    if(!text||!*text||strlen(text)>4000||!*api_key||!*model||!atomic_load(&online))return ESP_FAIL;
    bool expected=false;if(!atomic_compare_exchange_strong(&busy,&expected,true))return ESP_FAIL;
    ty_interaction_clear();
    atomic_store(&cancelled,false);atomic_store(&timed_out,false);
    atomic_store(&task_deadline,esp_timer_get_time()+360LL*1000000);
    final_text[0]=0;current_voice_task=voice;current_scheduled_task=scheduled;ty_set_status("queued");ty_expression("thinking",360000);
    tool_ok=0;tool_failed=0;llm_calls=0;task_id++;
    nvs_set_u32(data_nvs,"task_id",task_id);nvs_set_u8(data_nvs,"active",1);nvs_commit(data_nvs);
    mimi_msg_t message={0};strcpy(message.channel,"system");strcpy(message.chat_id,voice?"voice-dialog":"onboard-task");message.content=strdup(text);
    if(!message.content||message_bus_push_inbound(&message)!=ESP_OK){free(message.content);atomic_store(&busy,false);return ESP_FAIL;}
    return ESP_OK;
}
esp_err_t ty_submit_task(const char *text){return submit(text,false,false);}
esp_err_t ty_submit_scheduled_task(const char *text){return ty_audio_muted()?ESP_FAIL:submit(text,false,true);}
bool ty_direct_command(const char *text){
 const char *action=NULL,*reply=NULL;
 if(strstr(text,"恢复说话")||strstr(text,"取消静音")||strstr(text,"解除静音")||strstr(text,"可以说话")){
  ty_audio_mute(false);ty_say("好，我可以说话了。");return true;}
 if(strstr(text,"安静")||strstr(text,"闭嘴")||strstr(text,"静音")){ty_audio_mute(true);return true;}
 if(strstr(text,"停下")||strstr(text,"别动")||(strstr(text,"停止")&&!strstr(text,"断句")&&!strstr(text,"倾听")&&!strstr(text,"观察")&&!strstr(text,"汇报"))||!strcmp(text,"停")){
  ty_board_cancel();ty_say("好，停下了。");return true;}
 if(ty_audio_muted())return true;
 if(strstr(text,"你刚才听到什么")||strstr(text,"你刚才听成什么")||strstr(text,"复述我刚才说的")){
  char reply[1050];snprintf(reply,sizeof(reply),"我刚才听到的是：%s",*last_heard?last_heard:"还没有可复述的话。");ty_say(reply);return true;
 }
 if(ty_voice_controls(text)){ty_local_dialogue_clear();return true;}
 const char *local_reply=ty_local_answer(text);if(local_reply){
  if(!ty_board_busy()){(void)session_append("voice-dialog","user",text);(void)session_append("voice-dialog","assistant",local_reply);}
  ty_say(local_reply);return true;
 }
 if(strstr(text,"关闭倾听模式")||strstr(text,"退出倾听模式")||strstr(text,"普通聊天模式")){set_listening_style(false);ty_say("好，回到普通聊天模式。");return true;}
 if(strstr(text,"倾听模式")||strstr(text,"听我说说")){set_listening_style(true);ty_say("好，我在听，你慢慢说。");return true;}
 /* Research requests about gestures/following must reach the Agent, not be
  * swallowed by a keyword intended for a short hardware command. */
 const char *tasks[]={"论文","检索","搜索","资料","帮我找","浏览器","网页","文件","表格","分析","报告","任务","比较","电脑","上网"};
 bool task_intent=false;for(unsigned i=0;i<sizeof(tasks)/sizeof(tasks[0]);i++)if(strstr(text,tasks[i]))task_intent=true;
 if(ty_interaction_user(text)){
  ty_hri_intent_t intent=ty_hri_intent(text,true);
  return intent==TY_HRI_AVAILABLE||intent==TY_HRI_CLARIFY||!task_intent;
 }
 if(task_intent)return false;
 if(strstr(text,"音量")){
  unsigned v=ty_volume_setting(25,false);const char *p=text;while(*p&&(*p<'0'||*p>'9'))p++;
  if(*p)v=(unsigned)atoi(p);else if(strstr(text,"大")||strstr(text,"高"))v=v+10>100?100:v+10;else if(strstr(text,"小")||strstr(text,"低"))v=v<10?0:v-10;
  ty_audio_volume(v>100?100:v);ty_say("好，音量调好了。");return true;}
 if(strstr(text,"陪伴模式")){action="companion";reply="好，我试着开启陪伴模式。";}
 else if((strstr(text,"关闭")||strstr(text,"取消")||strstr(text,"不要")||strstr(text,"别再"))&&strstr(text,"跟随")){action="follow_off";reply="好，不跟着转了。";}
 else if((strstr(text,"关闭")||strstr(text,"取消")||strstr(text,"不要"))&&strstr(text,"手势")){action="gestures_off";reply="好，手势观察关掉了。";}
 else if(strstr(text,"跟随")||strstr(text,"看着我")||strstr(text,"看我")){action="follow";reply="好，我试着跟着你的脸。";}
 else if(strstr(text,"手势")){action="gestures";reply="好，我试着开启手势观察。";}
 else if(strstr(text,"点头")){action="nod";reply="好，我试着点一下头。";}
 else if(strstr(text,"摇头")){action="shake";reply="好，我试着摇一下头。";}
 else if(strstr(text,"向左")||strstr(text,"往左")){action="left";reply="好，往左看一点。";}
 else if(strstr(text,"向右")||strstr(text,"往右")){action="right";reply="好，往右看一点。";}
 else if(strstr(text,"抬头")||strstr(text,"向上")){action="up";reply="好，我试着抬一点头。";}
 else if(strstr(text,"低头")||strstr(text,"向下")){action="down";reply="好，我试着低一点头。";}
 else if(strstr(text,"回正")){action="center";reply="好，我试着回到中间。";}
 else if(strstr(text,"笑一个")||strstr(text,"开心表情")){action="happy";reply="嘿嘿。";}
 if(!action)return false;
 cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"action",action);cJSON *r=ty_robot_command(a);bool ok=cJSON_IsTrue(cJSON_GetObjectItem(r,"ok"));cJSON_Delete(r);cJSON_Delete(a);
 ty_say(ok?reply:"这次没有接收成功，你可以看一下状态。");return true;
}
void ty_voice_text(const char *text){
 /* Only wake-word/dialog text reaches the agent. Generic nearby speech is ignored. */
 if(strstr(text,"安静")||strstr(text,"闭嘴")||strstr(text,"恢复说话")||strstr(text,"取消静音")){(void)ty_direct_command(text);return;}
 cJSON *s=ty_audio_status();bool open=cJSON_IsTrue(cJSON_GetObjectItem(s,"reply_window_open"))||cJSON_IsTrue(cJSON_GetObjectItem(s,"utterance_dialog_admitted"));
 const char *wake=strstr(text,"汤圆");if(!wake)wake=strstr(text,"汤园");
 /* Busy still blocks ambient chat. An explicit delivery/defer answer inside
  * the actual reply window must remain usable after our clarification. */
 ty_hri_intent_t followup_intent=ty_hri_intent(text,false);
 bool followup_control=open&&(followup_intent==TY_HRI_AVAILABLE||followup_intent==TY_HRI_BUSY);
 if(!wake&&ty_interaction_busy()&&!followup_control){voice_admission="ignored_user_busy";cJSON_Delete(s);return;}
 if(!wake&&!open){voice_admission="ignored_not_awake_or_waiting_reply";cJSON_Delete(s);return;}
 voice_admission=wake?"wake_word":"followup_window";
 ty_trace_begin(s);cJSON_Delete(s);
 if(wake){text=wake+strlen("汤圆");
  const char *punct[]={" ",",","，",":","：",".","。","！","？"};bool removed;
  do{removed=false;for(unsigned i=0;i<sizeof(punct)/sizeof(punct[0]);i++)if(!strncmp(text,punct[i],strlen(punct[i]))){text+=strlen(punct[i]);removed=true;break;}}while(removed);
  if(!open&&!ty_board_busy()){ty_dialog_clear();ty_local_dialogue_clear();}
  ty_dialog_begin();}
 if(ty_direct_command(text)){if(!strstr(text,"刚才听")&&!strstr(text,"复述我"))snprintf(last_heard,sizeof(last_heard),"%s",text);cJSON *a=ty_audio_status();cJSON *id=cJSON_GetObjectItem(a,"say_id");if(cJSON_IsNumber(id))ty_trace_reply(0,(unsigned)id->valuedouble);cJSON_Delete(a);return;}
 snprintf(last_heard,sizeof(last_heard),"%s",text);
 if(ty_board_busy()){voice_admission="ignored_agent_busy";return;}
 if(!*text||!strcmp(text,"你好")||!strcmp(text,"你好。")){ty_say("我在，你说。");cJSON *a=ty_audio_status();ty_trace_reply(0,(unsigned)cJSON_GetObjectItem(a,"say_id")->valuedouble);cJSON_Delete(a);return;}
 ty_dialog_begin();
 if(submit(text,true,false)!=ESP_OK)ty_say("这次任务没有启动，稍后再试一下。");
 else {ty_trace_task(task_id);if(strstr(text,"帮我")||strstr(text,"上网")||strstr(text,"论文")||strstr(text,"分析")||strstr(text,"整理")||strstr(text,"报告"))ty_say("好，我去看看，处理好告诉你。");}
}
void ty_board_preset(void){(void)ty_submit_task(preset);}
static void outbound(void *arg){
    (void)arg;mimi_msg_t msg;
    for(;;){if(message_bus_pop_outbound(&msg,UINT32_MAX)!=ESP_OK)continue;
        snprintf(final_text,sizeof(final_text),"%s",msg.content?msg.content:"");
        bool failed=msg.content&&!strcmp(msg.content,"Sorry, I encountered an error.");
        free(msg.content);ty_set_status(atomic_load(&timed_out)?"time_budget_exhausted":ty_cancelled()?"cancelled":failed?"failed":"finished");
        nvs_set_u8(data_nvs,"active",0);nvs_commit(data_nvs);ty_event("task_return","",!failed&&!ty_cancelled(),strlen(final_text));
        atomic_store(&busy,false);
        if(!ty_cancelled()){
            ty_expression(failed?"sad":"happy",2000);
            if(!failed)ty_interaction_result(*ty_task_artifact_name()?"结果文件已经保存好了，详细内容可以在电脑上查看。":final_text,current_voice_task||current_scheduled_task);
            else if(current_voice_task||current_scheduled_task)ty_interaction_result("这次没有完成，你可以看看任务状态。",true);
        }
    }
}
static bool authorized(httpd_req_t *r){
    char header[90],expected[90];snprintf(expected,sizeof(expected),"Bearer %s",token);
    return *token&&httpd_req_get_hdr_value_str(r,"Authorization",header,sizeof(header))==ESP_OK&&!strcmp(header,expected);
}
static esp_err_t api(httpd_req_t *r){
    if(!authorized(r)){httpd_resp_set_status(r,"401 Unauthorized");return httpd_resp_sendstr(r,"{\"error\":\"pairing_token_required\"}");}
    httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    if(!strcmp(r->uri,"/computer"))return ty_computer_api(r);
    if(!strcmp(r->uri,"/voice_control")){
        cJSON *out=NULL;
        if(r->method==HTTP_GET)out=ty_voice_control_pending();
        else{
            char buf[1401]={0};int used=0,n;if(r->content_len<1||r->content_len>1400)return ESP_ERR_INVALID_ARG;
            while(used<r->content_len&&(n=httpd_req_recv(r,buf+used,r->content_len-used))>0)used+=n;
            cJSON *body=used==r->content_len?cJSON_Parse(buf):NULL;bool ok=ty_voice_control_ack(body);cJSON_Delete(body);
            out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"accepted",ok);if(!ok)httpd_resp_set_status(r,"409 Conflict");
        }char *v=cJSON_PrintUnformatted(out);cJSON_Delete(out);esp_err_t e=httpd_resp_sendstr(r,v?v:"{}");free(v);return e;
    }
    if(!strcmp(r->uri,"/agent")){
        if(r->content_len<1||r->content_len>10000)return ESP_ERR_INVALID_ARG;
        char *buf=calloc(1,r->content_len+1);if(!buf)return ESP_ERR_NO_MEM;int used=0,n;
        while(used<r->content_len&&(n=httpd_req_recv(r,buf+used,r->content_len-used))>0)used+=n;
        cJSON *body=used==r->content_len?cJSON_Parse(buf):NULL;free(buf);
        cJSON *tool=cJSON_GetObjectItem(body,"tool"),*args=cJSON_GetObjectItem(body,"arguments");
        cJSON *result=cJSON_IsString(tool)&&cJSON_IsObject(args)?ty_agent_extra(tool->valuestring,args):NULL;cJSON_Delete(body);
        if(!result){httpd_resp_set_status(r,"400 Bad Request");return httpd_resp_sendstr(r,"{\"ok\":false}");}
        char *encoded=cJSON_PrintUnformatted(result);cJSON_Delete(result);esp_err_t e=httpd_resp_sendstr(r,encoded?encoded:"{}");free(encoded);return e;
    }
    if(!strcmp(r->uri,"/cancel")){ty_board_cancel();return httpd_resp_sendstr(r,"{\"cancel_requested\":true}");}
    if(!strncmp(r->uri,"/audio",6))return ty_audio_api(r,"/audio");
    if(!strcmp(r->uri,"/play")||!strcmp(r->uri,"/voice")||!strcmp(r->uri,"/hri_audio")||!strcmp(r->uri,"/hri_prediction"))return ty_audio_api(r,r->uri);
    if(!strcmp(r->uri,"/preview")){
        cJSON *j=ty_robot_preview();char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);esp_err_t e=httpd_resp_sendstr(r,text?text:"{}");free(text);return e;}
    if(!strcmp(r->uri,"/control")||!strcmp(r->uri,"/gesture")){
        if(r->content_len<1||r->content_len>2000)return ESP_ERR_INVALID_ARG;
        char buf[2001]={0};int used=0,n;while(used<r->content_len&&(n=httpd_req_recv(r,buf+used,r->content_len-used))>0)used+=n;
        cJSON *a=used==r->content_len?cJSON_Parse(buf):NULL,*v=cJSON_GetObjectItem(a,"action"),*out=NULL;
        const char *action=cJSON_IsString(v)?v->valuestring:"";
        if(!strcmp(r->uri,"/gesture")){
            cJSON *score=cJSON_GetObjectItem(a,"score");bool ok=ty_robot_gesture(action,cJSON_IsNumber(score)?score->valuedouble:0);out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",ok);
        }else if(!strcmp(action,"speech_failed")){
            cJSON *id=cJSON_GetObjectItem(a,"say_id"),*generation=cJSON_GetObjectItem(a,"speech_epoch"),*boot=cJSON_GetObjectItem(a,"boot_id");
            bool valid=cJSON_IsNumber(boot)&&(uint32_t)boot->valuedouble==ty_board_boot_id()&&cJSON_IsNumber(id)&&cJSON_IsNumber(generation);
            bool retired=valid&&ty_audio_fail((unsigned)id->valuedouble,(unsigned)generation->valuedouble);
            out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",valid);cJSON_AddBoolToObject(out,"retired",retired);
            if(!valid)httpd_resp_set_status(r,"409 Conflict");
        }else if(!strcmp(action,"sleep")){ty_dialog_close();ty_local_dialogue_clear();out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);}
        else if(!strcmp(action,"mute")||!strcmp(action,"resume")){ty_audio_mute(!strcmp(action,"mute"));out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);}
        else if(!strcmp(action,"voice_on")||!strcmp(action,"voice_off")){ty_audio_enable(!strcmp(action,"voice_on"));out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);}
        else if(!strcmp(action,"listen_on")||!strcmp(action,"listen_off")){set_listening_style(!strcmp(action,"listen_on"));out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);}
        else if(!strcmp(action,"volume")){cJSON *n=cJSON_GetObjectItem(a,"value");out=cJSON_CreateObject();bool ok=cJSON_IsNumber(n)&&n->valueint>=0&&n->valueint<=100;if(ok)ty_audio_volume(n->valueint);cJSON_AddBoolToObject(out,"ok",ok);}
        else if(!strcmp(action,"stop")){ty_board_cancel();out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);}
        else if(ty_interaction_control(action)){out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);}
        else out=ty_robot_command(a);
        cJSON_Delete(a);char *text=cJSON_PrintUnformatted(out);cJSON_Delete(out);esp_err_t ret=httpd_resp_sendstr(r,text?text:"{}");free(text);return ret;
    }
    if(!strcmp(r->uri,"/task")){
        if(r->content_len<1||r->content_len>6000){httpd_resp_set_status(r,"400 Bad Request");return httpd_resp_sendstr(r,"{\"ok\":false}");}
        char *buf=calloc(1,r->content_len+1);if(!buf)return ESP_ERR_NO_MEM;
        size_t used=0;int n;while(used<r->content_len&&(n=httpd_req_recv(r,buf+used,r->content_len-used))>0)used+=n;
        cJSON *root=used==r->content_len?cJSON_Parse(buf):NULL;free(buf);
        cJSON *task=cJSON_GetObjectItem(root,"task");esp_err_t ret=cJSON_IsString(task)?(ty_direct_command(task->valuestring)?ESP_OK:ty_submit_task(task->valuestring)):ESP_FAIL;cJSON_Delete(root);
        if(ret!=ESP_OK)httpd_resp_set_status(r,"409 Conflict");
        return httpd_resp_sendstr(r,ret==ESP_OK?"{\"accepted\":true}":"{\"accepted\":false,\"reason\":\"busy_or_not_ready\"}");
    }
    cJSON *out=cJSON_CreateObject();cJSON_AddStringToObject(out,"phase",ty_status());
    cJSON_AddNumberToObject(out,"boot_id",boot_id);cJSON_AddNumberToObject(out,"task_id",task_id);
    cJSON_AddBoolToObject(out,"active_listening_style",ty_listening_style());
    cJSON_AddNumberToObject(out,"tool_ok",tool_ok);cJSON_AddNumberToObject(out,"tool_failed",tool_failed);
    cJSON_AddNumberToObject(out,"llm_calls",llm_calls);
    cJSON_AddNumberToObject(out,"free_psram",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(out,"free_internal",heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(out,"reset_reason",esp_reset_reason());
    cJSON_AddBoolToObject(out,"clock_ready",time(NULL)>=1735689600);
    char checkpoint[256];size_t checkpoint_size=sizeof(checkpoint);
    if(nvs_get_str(data_nvs,"evidence",checkpoint,&checkpoint_size)==ESP_OK){
        cJSON *value=cJSON_Parse(checkpoint);if(value)cJSON_AddItemToObject(out,"last_evidence",value);
    }
    cJSON *recent=cJSON_AddArrayToObject(out,"recent_evidence");
    for(unsigned i=0;i<8;i++){char key[12];snprintf(key,sizeof(key),"event%u",i);checkpoint_size=sizeof(checkpoint);
        if(nvs_get_str(data_nvs,key,checkpoint,&checkpoint_size)==ESP_OK){cJSON *value=cJSON_Parse(checkpoint);if(value)cJSON_AddItemToArray(recent,value);}}
    cJSON_AddBoolToObject(out,"busy",atomic_load(&busy));cJSON_AddBoolToObject(out,"onboard_agent",true);cJSON_AddStringToObject(out,"last_voice_admission",voice_admission);
    cJSON_AddBoolToObject(out,"local_llm",false);cJSON_AddBoolToObject(out,"cancel_requested",ty_cancelled());
    cJSON_AddStringToObject(out,"answer",final_text);char *report=malloc(TY_STORE_MAX);
    cJSON_AddItemToObject(out,"robot",ty_robot_status());cJSON_AddItemToObject(out,"audio",ty_audio_status());cJSON_AddItemToObject(out,"voice_traces",ty_trace_status());cJSON_AddItemToObject(out,"local_dialogue",ty_local_dialogue_status());
    cJSON_AddItemToObject(out,"computer",ty_computer_status());cJSON_AddItemToObject(out,"interaction",ty_interaction_status());cJSON_AddItemToObject(out,"voice_control",ty_voice_control_status());
    nvs_stats_t nvs_stats={0};if(nvs_get_stats("agentcfg",&nvs_stats)==ESP_OK)cJSON_AddNumberToObject(out,"nvs_free_entries",nvs_stats.free_entries);
    if(report&&ty_store_read("report",report,TY_STORE_MAX)==ESP_OK)cJSON_AddStringToObject(out,"report",report);
    if(report&&ty_store_read("skill",report,TY_STORE_MAX)==ESP_OK)cJSON_AddBoolToObject(out,"saved_skill_available",true);
    cJSON_AddItemToObject(out,"agent_storage",ty_agent_extra_status());
    free(report);char *text=cJSON_PrintUnformatted(out);cJSON_Delete(out);
    esp_err_t ret=httpd_resp_sendstr(r,text?text:"{}");free(text);return ret;
}
static esp_err_t index_page(httpd_req_t *r){
    httpd_resp_set_type(r,"text/html; charset=utf-8");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,board_page);
}
static void start_server(void){
    /* Derive capacity from the complete route table. Silently dropping the
     * final routes leaves audio online while the PC executor stays offline. */
    httpd_uri_t routes[]={
      {.uri="/",.method=HTTP_GET,.handler=index_page},
      {.uri="/task",.method=HTTP_POST,.handler=api},
      {.uri="/cancel",.method=HTTP_POST,.handler=api},
      {.uri="/control",.method=HTTP_POST,.handler=api},
      {.uri="/gesture",.method=HTTP_POST,.handler=api},
      {.uri="/status",.method=HTTP_GET,.handler=api},
      {.uri="/preview",.method=HTTP_GET,.handler=api},
      {.uri="/agent",.method=HTTP_POST,.handler=api},
      {.uri="/voice_control",.method=HTTP_GET,.handler=api},
      {.uri="/voice_control",.method=HTTP_POST,.handler=api},
      {.uri="/computer",.method=HTTP_GET,.handler=api},
      {.uri="/computer",.method=HTTP_POST,.handler=api}
    };
    const size_t route_count=sizeof(routes)/sizeof(routes[0]);
    httpd_handle_t h=NULL;httpd_config_t c=HTTPD_DEFAULT_CONFIG();c.stack_size=10000;c.max_uri_handlers=route_count;
    /* Two servers reserve internal sockets as well as accepted TCP clients.
     * Leave room for both, concurrent helpers and outbound model HTTPS. */
    c.max_open_sockets=8;c.lru_purge_enable=true;
    if(httpd_start(&h,&c)!=ESP_OK)return;
    for(size_t i=0;i<route_count;i++)ESP_ERROR_CHECK(httpd_register_uri_handler(h,&routes[i]));
    httpd_handle_t audio=NULL;httpd_config_t ac=HTTPD_DEFAULT_CONFIG();ac.server_port=81;ac.ctrl_port=32769;ac.stack_size=10000;ac.max_open_sockets=8;ac.lru_purge_enable=true;
    if(httpd_start(&audio,&ac)==ESP_OK){const char *p[]={"/voice","/play","/audio","/hri_audio","/hri_prediction"};for(int i=0;i<5;i++){httpd_uri_t u={.uri=p[i],.method=i<2||i==4?HTTP_POST:HTTP_GET,.handler=api};httpd_register_uri_handler(audio,&u);}}
}
static bool get(nvs_handle_t h,const char *key,char *buf,size_t cap){size_t n=cap;return nvs_get_str(h,key,buf,&n)==ESP_OK;}
static bool set_config(cJSON *j){
    const char *keys[]={"ssid","password","api_key","model","token"};const size_t limits[]={32,64,256,96,64};
    for(int i=0;i<5;i++){
        cJSON *v=cJSON_GetObjectItem(j,keys[i]);
        if(!cJSON_IsString(v)||strlen(v->valuestring)>limits[i]||(i!=1&&!*v->valuestring))return false;
        if(i==4&&strlen(v->valuestring)<32)return false;
    }
    for(int i=0;i<5;i++)if(nvs_set_str(secret_nvs,keys[i],cJSON_GetObjectItem(j,keys[i])->valuestring)!=ESP_OK)return false;
    return nvs_commit(secret_nvs)==ESP_OK;
}
static void config_serial(void *unused){
    (void)unused;char line[2048];size_t used=0;bool overflow=false;
    for(;;){int c=getchar();if(c==EOF){clearerr(stdin);vTaskDelay(pdMS_TO_TICKS(30));continue;}
        if(c!='\n'){if(used<sizeof(line)-1)line[used++]=c;else overflow=true;continue;}
        line[used]=0;
        if(!overflow&&!strncmp(line,"CONFIG ",7)){
            cJSON *j=cJSON_Parse(line+7);bool ok=j&&set_config(j);cJSON_Delete(j);
            printf("TY_MIMI_CONFIG %s\n",ok?"SAVED_RESTART_REQUIRED":"REJECTED");
        }else if(!strcmp(line,"CANCEL")){ty_board_cancel();printf("TY_MIMI_CANCEL REQUESTED\n");}
        else printf("TY_MIMI_COMMAND REJECTED\n");
        memset(line,0,sizeof(line));used=0;overflow=false;
    }
}
static void *json_allocate(size_t bytes){return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
void app_main(void){
    cJSON_Hooks json_hooks={.malloc_fn=json_allocate,.free_fn=free};
    cJSON_InitHooks(&json_hooks);
    boot_id=esp_random();
    /* Never format/erase existing NVS on initialization failure. */
    if(nvs_flash_init_partition("agentcfg")!=ESP_OK||
       nvs_open_from_partition("agentcfg","ty_mimi_data",NVS_READWRITE,&data_nvs)!=ESP_OK||
       nvs_open_from_partition("agentcfg","ty_mimi_sec",NVS_READWRITE,&secret_nvs)!=ESP_OK)return;
    uint8_t previous_active=0;nvs_get_u8(data_nvs,"active",&previous_active);
    nvs_get_u32(data_nvs,"task_id",&task_id);
    ty_set_status(previous_active?"previous_task_interrupted":"setup_required");
    atomic_store(&listening_style,ty_setting_get("listen_style",false));
    ty_voice_control_init();ty_event("boot","",true,0);ty_screen_init();ty_robot_init();ty_audio_init();ty_computer_init();ty_interaction_init();ty_agent_extra_init();
    xTaskCreate(config_serial,"mimi_config",6144,NULL,3,NULL);
    char ssid[33],password[65];
    if(!get(secret_nvs,"ssid",ssid,sizeof(ssid))||!get(secret_nvs,"password",password,sizeof(password))||
       !get(secret_nvs,"api_key",api_key,sizeof(api_key))||!get(secret_nvs,"model",model,sizeof(model))||!get(secret_nvs,"token",token,sizeof(token)))return;
    if(esp_netif_init()!=ESP_OK||esp_event_loop_create_default()!=ESP_OK)return;
    esp_netif_create_default_wifi_sta();wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable=0; /* Keep Wi-Fi driver out of the original default NVS. */
    if(esp_wifi_init(&cfg)!=ESP_OK)return;
    esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL);
    esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL);
    esp_sntp_config_t sntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if(esp_netif_sntp_init(&sntp)!=ESP_OK)return;
    wifi_config_t w={0};memcpy(w.sta.ssid,ssid,strlen(ssid));memcpy(w.sta.password,password,strlen(password));memset(password,0,sizeof(password));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);esp_wifi_set_mode(WIFI_MODE_STA);esp_wifi_set_config(WIFI_IF_STA,&w);memset(&w,0,sizeof(w));
    if(message_bus_init()!=ESP_OK||tool_registry_init()!=ESP_OK||agent_loop_init()!=ESP_OK||agent_loop_start()!=ESP_OK)return;
    xTaskCreate(outbound,"mimi_result",6144,NULL,3,NULL);start_server();esp_wifi_start();
}

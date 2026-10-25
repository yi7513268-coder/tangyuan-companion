/* Onboard task notes and schedules. No raw GPIO or external-account sending.
 * Persistence is confined to our existing experimental NVS namespace.
 */
#include "agent_extra.h"
#include "interaction.h"
#include "relative_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
static SemaphoreHandle_t mutex;
static cJSON *files,*jobs;
static bool storage_ready;
static const char *str(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static double num(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsNumber(v)?v->valuedouble:0;}
static void text(cJSON *j,const char *k,const char *v){cJSON_DeleteItemFromObject(j,k);cJSON_AddStringToObject(j,k,v);}
static void number(cJSON *j,const char *k,double v){cJSON_DeleteItemFromObject(j,k);cJSON_AddNumberToObject(j,k,v);}
static void boolean(cJSON *j,const char *k,bool v){cJSON_DeleteItemFromObject(j,k);cJSON_AddBoolToObject(j,k,v);}
static cJSON *fail(const char *s){cJSON *r=cJSON_CreateObject();cJSON_AddBoolToObject(r,"ok",false);cJSON_AddStringToObject(r,"error",s);return r;}
static cJSON *ok(void){cJSON *r=cJSON_CreateObject();cJSON_AddBoolToObject(r,"ok",true);return r;}
static bool persist(const char *key,cJSON *j){
 char *s=cJSON_PrintUnformatted(j);if(!s)return false;size_t n=strlen(s);char *check=malloc(n+1);
 bool good=n<12000&&check&&ty_store_write(key,s)==ESP_OK&&ty_store_read(key,check,n+1)==ESP_OK&&!strcmp(s,check);
 free(check);free(s);return good;
}
static cJSON *load(const char *key,bool array){
 char *s=malloc(TY_STORE_MAX);cJSON *j=s&&ty_store_read(key,s,TY_STORE_MAX)==ESP_OK?cJSON_Parse(s):NULL;free(s);
 if(!j)j=array?cJSON_CreateArray():cJSON_CreateObject();
 if((array&&!cJSON_IsArray(j))||(!array&&!cJSON_IsObject(j))){cJSON_Delete(j);return NULL;}return j;
}
static bool filename(const char *p){
 if(strncmp(p,"/agent/",7)||strlen(p)>70||strstr(p,"..")||strchr(p+7,'/')||strchr(p,'\\')||strchr(p,':'))return false;
 const char *ext=strrchr(p,'.');return strlen(p)>10&&ext&&(!strcmp(ext,".md")||!strcmp(ext,".txt")||!strcmp(ext,".json")||!strcmp(ext,".csv"));
}
static cJSON *file_tool(const cJSON *a){
 const char *action=str(a,"action"),*path=str(a,"path");cJSON *r=ok();
 if(!strcmp(action,"list")){
  cJSON *list=cJSON_AddArrayToObject(r,"files"),*v;cJSON_ArrayForEach(v,files){cJSON *item=cJSON_CreateObject();cJSON_AddStringToObject(item,"path",v->string);cJSON_AddNumberToObject(item,"bytes",strlen(v->valuestring));cJSON_AddItemToArray(list,item);}return r;
 }
 if(!filename(path)){cJSON_Delete(r);return fail("path_must_be_flat_agent_md_txt_json_csv");}
 cJSON *old=cJSON_GetObjectItemCaseSensitive(files,path);
 if(!strcmp(action,"read")){
  if(!cJSON_IsString(old)){cJSON_Delete(r);return fail("file_not_found");}
  cJSON_AddStringToObject(r,"path",path);cJSON_AddStringToObject(r,"content",old->valuestring);return r;
 }
 cJSON *next=cJSON_Duplicate(files,true);const char *content=str(a,"content");char *edited=NULL;
 if(!strcmp(action,"write")){
  if(cJSON_IsString(old)&&!cJSON_IsTrue(cJSON_GetObjectItem(a,"overwrite"))){cJSON_Delete(next);cJSON_Delete(r);return fail("file_exists_explicit_overwrite_required");}
  if(strlen(content)>3000||(!old&&cJSON_GetArraySize(files)>=12)){cJSON_Delete(next);cJSON_Delete(r);return fail("file_limit_12_entries_3000_bytes_each");}
  text(next,path,content);
 }else if(!strcmp(action,"edit")){
  const char *find=str(a,"old_string"),*replacement=str(a,"new_string"),*at=cJSON_IsString(old)&&*find?strstr(old->valuestring,find):NULL;
  if(!at){cJSON_Delete(next);cJSON_Delete(r);return fail("old_string_not_found");}
  size_t n=strlen(old->valuestring)-strlen(find)+strlen(replacement);
  if(n>3000){cJSON_Delete(next);cJSON_Delete(r);return fail("file_exceeds_3000_bytes");}
  edited=malloc(n+1);if(!edited){cJSON_Delete(next);cJSON_Delete(r);return fail("no_memory");}
  size_t prefix=(size_t)(at-old->valuestring);memcpy(edited,old->valuestring,prefix);strcpy(edited+prefix,replacement);strcpy(edited+prefix+strlen(replacement),at+strlen(find));text(next,path,edited);free(edited);
 }else if(!strcmp(action,"delete")){
  if(!old){cJSON_Delete(next);cJSON_Delete(r);return fail("file_not_found");}cJSON_DeleteItemFromObjectCaseSensitive(next,path);
 }else{cJSON_Delete(next);cJSON_Delete(r);return fail("unknown_file_action");}
 if(!persist("agent_files",next)){cJSON_Delete(next);cJSON_Delete(r);return fail("storage_or_exact_readback_failed");}
 cJSON_Delete(files);files=next;cJSON_AddBoolToObject(r,"verified",true);cJSON_AddStringToObject(r,"path",path);cJSON_AddStringToObject(r,"storage","robot experimental NVS; explicit task notes, not original private memory");return r;
}
static cJSON *clock_tool(void){
 time_t now=time(NULL);if(now<1735689600)return fail("clock_not_ready_wait_for_NTP_or_paired_PC");
 cJSON *r=ok();struct tm local;time_t adjusted=now+8*3600;gmtime_r(&adjusted,&local);char stamp[32];strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",&local);
 cJSON_AddNumberToObject(r,"unix_time",now);cJSON_AddStringToObject(r,"local_time",stamp);cJSON_AddStringToObject(r,"timezone","Asia/Shanghai");cJSON_AddNumberToObject(r,"utc_offset_seconds",28800);return r;
}
/* Explicit local calendar conversion, independent of process-global TZ. */
static int64_t local_epoch(const char *s){
 if(strlen(s)!=19||s[4]!='-'||s[7]!='-'||s[10]!=' '||s[13]!=':'||s[16]!=':')return 0;
 for(int i=0;i<19;i++)if(i!=4&&i!=7&&i!=10&&i!=13&&i!=16&&(s[i]<'0'||s[i]>'9'))return 0;
 int y,m,d,h,n,z;char tail;if(sscanf(s,"%d-%d-%d %d:%d:%d%c",&y,&m,&d,&h,&n,&z,&tail)!=6||y<2025||y>2099||m<1||m>12||d<1||h<0||h>23||n<0||n>59||z<0||z>59)return 0;
 int days[]={31,28,31,30,31,30,31,31,30,31,30,31};bool leap=y%4==0&&(y%100!=0||y%400==0);if(m==2&&leap)days[1]++;if(d>days[m-1])return 0;
 int64_t total=0;for(int year=1970;year<y;year++)total+=365+(year%4==0&&(year%100!=0||year%400==0));for(int month=1;month<m;month++)total+=days[month-1];
 return (total+d-1)*86400+h*3600+n*60+z-28800;
}
static void advance(cJSON *j,time_t now){
 double interval=num(j,"interval_s"),next=num(j,"next_epoch");
 if(interval>=60){next+=(floor((now-next)/interval)+1)*interval;number(j,"next_epoch",next);}
 else boolean(j,"enabled",false);
}
static cJSON *schedule_tool(const cJSON *a){
 const char *action=str(a,"action");time_t now=time(NULL);
 if(!strcmp(action,"list")){cJSON *r=ok();cJSON_AddItemToObject(r,"jobs",cJSON_Duplicate(jobs,true));return r;}
 if(now<1735689600)return fail("clock_not_ready");
 cJSON *next=cJSON_Duplicate(jobs,true),*j=NULL,*v;const char *id=str(a,"id");
 cJSON_ArrayForEach(v,next)if(!strcmp(str(v,"id"),id)){j=v;break;}
 if(!strcmp(action,"add")){
  const char *name=str(a,"name"),*message=str(a,"message"),*type=str(a,"schedule_type");
  if(j||cJSON_GetArraySize(next)>=6||!*name||strlen(name)>60||!*message||strlen(message)>600){cJSON_Delete(next);return fail("job_limit_6_name_60_message_600_bytes");}
  cJSON_ArrayForEach(v,next)if(!strcmp(str(v,"name"),name)){cJSON_Delete(next);return fail("job_name_already_exists_list_or_cancel_first");}
  double delay=num(a,"delay_s"),interval=num(a,"interval_s"),at=num(a,"at_epoch");
  if(*str(a,"local_time"))at=local_epoch(str(a,"local_time"));
  if(!strcmp(type,"after")){if(delay<5||delay>604800||floor(delay)!=delay){cJSON_Delete(next);return fail("delay_must_be_5_to_604800_seconds");}at=now+delay;interval=0;}
  else if(!strcmp(type,"every")){if(interval<60||interval>604800||floor(interval)!=interval){cJSON_Delete(next);return fail("interval_must_be_60_to_604800_seconds");}if(!at)at=now+interval;}
  else if(!strcmp(type,"at"))interval=0;
  else{cJSON_Delete(next);return fail("schedule_type_after_at_or_every_required");}
  if(!isfinite(at)||at<=now||at>now+366LL*86400||floor(at)!=at){cJSON_Delete(next);return fail("future_time_within_one_year_required");}
  const char *kind=str(a,"task_kind");if(*kind&&strcmp(kind,"reminder")&&strcmp(kind,"agent")){cJSON_Delete(next);return fail("task_kind_reminder_or_agent_required");}
  j=cJSON_CreateObject();char identifier[16];snprintf(identifier,sizeof(identifier),"%08lx",(unsigned long)esp_random());text(j,"id",identifier);text(j,"name",name);text(j,"message",message);text(j,"task_kind",*kind?kind:"agent");text(j,"schedule_type",type);text(j,"phase","scheduled");number(j,"next_epoch",at);number(j,"interval_s",interval);boolean(j,"enabled",true);cJSON_AddItemToArray(next,j);
 }else if(!j){cJSON_Delete(next);return fail("job_not_found");}
 else if(!strcmp(action,"cancel")){boolean(j,"enabled",false);text(j,"phase","cancelled_future_runs");}
 else if(!strcmp(action,"retry")){if(ty_board_busy()){cJSON_Delete(next);return fail("robot_busy_retry_later");}boolean(j,"enabled",true);number(j,"next_epoch",now+5);number(j,"last_task_id",0);text(j,"phase","scheduled");}
 else if(!strcmp(action,"remove")){
  for(int i=0;i<cJSON_GetArraySize(next);i++)if(cJSON_GetArrayItem(next,i)==j){cJSON_DeleteItemFromArray(next,i);j=NULL;break;}
 }else{cJSON_Delete(next);return fail("unknown_schedule_action");}
 if(!persist("agent_jobs",next)){cJSON_Delete(next);return fail("schedule_storage_or_readback_failed");}
 cJSON *r=ok();if(j)cJSON_AddItemToObject(r,"job",cJSON_Duplicate(j,true));cJSON_AddBoolToObject(r,"verified",true);cJSON_AddStringToObject(r,"cancel_scope","future runs only; already dispatched task uses separate stop control");
 cJSON_Delete(jobs);jobs=next;return r;
}
cJSON *ty_agent_extra(const char *tool,const cJSON *args){
 if(!strcmp(tool,"preview_voice_schedule")){
  ty_voice_schedule_t spec;int parsed=ty_parse_voice_schedule(str(args,"text"),time(NULL),&spec);cJSON *r=cJSON_CreateObject();
  cJSON_AddNumberToObject(r,"parsed",parsed);cJSON_AddBoolToObject(r,"ok",parsed==1);
  if(parsed==1){cJSON_AddNumberToObject(r,"seconds",spec.seconds);cJSON_AddNumberToObject(r,"at_epoch",spec.at_epoch);cJSON_AddBoolToObject(r,"repeating",spec.repeating);cJSON_AddStringToObject(r,"message",spec.message);}return r;
 }
 if(!mutex)return fail("agent_storage_unavailable");
 if(!storage_ready&&strcmp(tool,"get_current_time"))return fail("agent_storage_invalid_no_automatic_erase");
 xSemaphoreTake(mutex,portMAX_DELAY);cJSON *r;
 if(!strcmp(tool,"get_current_time"))r=clock_tool();
 else if(!strcmp(tool,"agent_files"))r=file_tool(args);
 else if(!strcmp(tool,"agent_schedule"))r=schedule_tool(args);
 else{r=fail("unknown_agent_tool");}
 xSemaphoreGive(mutex);return r;
}
static void tick(void *unused){
 (void)unused;for(;;){vTaskDelay(pdMS_TO_TICKS(1000));time_t now=time(NULL);if(now<1735689600)continue;
  cJSON *audio=ty_audio_status(),*delivery=ty_interaction_status();bool quiet=cJSON_IsTrue(cJSON_GetObjectItem(audio,"muted"));
  xSemaphoreTake(mutex,portMAX_DELAY);bool dirty=false;cJSON *j;
  cJSON_ArrayForEach(j,jobs){
   const char *phase=str(j,"phase");unsigned task=(unsigned)num(j,"last_task_id");
   if(!strcmp(phase,"speech_submitted")){
    if((unsigned)num(j,"run_boot_id")!=ty_board_boot_id()){text(j,"phase","interrupted_by_restart");dirty=true;}
    else if((unsigned)num(j,"speech_epoch")!=(unsigned)num(audio,"speech_epoch")||quiet){text(j,"phase","interrupted_by_quiet");dirty=true;}
    else if((unsigned)num(j,"say_id")== (unsigned)num(audio,"played_id")){text(j,"phase","delivered");number(j,"delivered_epoch",now);dirty=true;}
    else if((unsigned)num(j,"say_id")== (unsigned)num(audio,"failed_id")){text(j,"phase","speech_generation_or_delivery_failed");dirty=true;}
    else if((unsigned)num(j,"say_id")== (unsigned)num(audio,"interrupted_id")||(unsigned)num(j,"say_id")!=(unsigned)num(audio,"say_id")){text(j,"phase","speech_interrupted");dirty=true;}
    else if(now-num(j,"run_epoch")>900){text(j,"phase","delivery_unconfirmed");dirty=true;}
    continue;
   }
   bool inflight=!strcmp(phase,"dispatched")||!strcmp(phase,"result_ready_not_delivered");
   if(inflight){
    if((unsigned)num(j,"run_boot_id")!=ty_board_boot_id()){text(j,"phase","interrupted_by_restart");dirty=true;}
    else if((unsigned)num(delivery,"last_delivered_task_id")==task){text(j,"phase","delivered");number(j,"delivered_epoch",now);dirty=true;}
    else if(quiet){text(j,"phase","interrupted_by_quiet");dirty=true;}
    else if(ty_current_task_id()!=task){text(j,"phase","interrupted_by_new_task");dirty=true;}
    else if(!ty_board_busy()&&!strcmp(phase,"dispatched")){
     if(!strcmp(ty_status(),"finished")){text(j,"phase","result_ready_not_delivered");}
     else{text(j,"phase","task_failed_or_cancelled");}dirty=true;
    }else if(now-num(j,"run_epoch")>900){text(j,"phase","delivery_unconfirmed");dirty=true;}
    continue;
   }
   if(!cJSON_IsTrue(cJSON_GetObjectItem(j,"enabled"))||num(j,"next_epoch")>now)continue;
   if(quiet){text(j,"phase","missed_while_quiet_no_replay");advance(j,now);dirty=true;continue;}
   if(now-num(j,"next_epoch")>900){text(j,"phase","expired_no_replay");advance(j,now);dirty=true;continue;}
   if(ty_board_busy()||cJSON_IsTrue(cJSON_GetObjectItem(delivery,"pending_result"))||cJSON_IsTrue(cJSON_GetObjectItem(audio,"recording"))||cJSON_IsTrue(cJSON_GetObjectItem(audio,"asr_pending"))||cJSON_IsTrue(cJSON_GetObjectItem(audio,"playing"))||cJSON_GetObjectItem(audio,"say_text"))continue;
   if(!strcmp(str(j,"task_kind"),"reminder")){
    unsigned submitted=ty_say_if_idle(str(j,"message"));cJSON *after=ty_audio_status();
    if(submitted&&!cJSON_IsTrue(cJSON_GetObjectItem(after,"muted"))){text(j,"phase","speech_submitted");number(j,"say_id",submitted);number(j,"speech_epoch",num(after,"speech_epoch"));number(j,"run_boot_id",ty_board_boot_id());number(j,"run_epoch",now);number(j,"runs",num(j,"runs")+1);advance(j,now);dirty=true;}
    cJSON_Delete(after);break;
   }
   char message[1000];snprintf(message,sizeof(message),"[用户预先设置的定时任务，不是刚听到的对话] %s。按真实工具结果执行；只需提醒时简短说出提醒内容。",str(j,"message"));
   if(ty_submit_scheduled_task(message)==ESP_OK){text(j,"phase","dispatched");number(j,"last_task_id",ty_current_task_id());number(j,"run_boot_id",ty_board_boot_id());number(j,"run_epoch",now);number(j,"runs",num(j,"runs")+1);advance(j,now);dirty=true;break;}
  }
  if(dirty&&!persist("agent_jobs",jobs))ty_event("schedule_persist_failed","schedule",false,0);
  xSemaphoreGive(mutex);cJSON_Delete(audio);cJSON_Delete(delivery);
 }
}
cJSON *ty_agent_extra_status(void){
 cJSON *r=cJSON_CreateObject();if(!mutex)return r;xSemaphoreTake(mutex,portMAX_DELAY);cJSON_AddNumberToObject(r,"files",cJSON_GetArraySize(files));cJSON_AddNumberToObject(r,"jobs",cJSON_GetArraySize(jobs));cJSON_AddBoolToObject(r,"onboard_persistence",storage_ready);cJSON_AddBoolToObject(r,"clock_ready",time(NULL)>=1735689600);xSemaphoreGive(mutex);return r;
}
void ty_agent_extra_init(void){
 mutex=xSemaphoreCreateMutex();files=load("agent_files",false);jobs=load("agent_jobs",true);
 storage_ready=mutex&&files&&jobs;
 cJSON *v;if(storage_ready)cJSON_ArrayForEach(v,files)if(!cJSON_IsString(v)||!filename(v->string)||strlen(v->valuestring)>3000)storage_ready=false;
 if(storage_ready)xTaskCreate(tick,"agent_schedule",6144,NULL,2,NULL);
}

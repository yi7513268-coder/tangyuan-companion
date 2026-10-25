/* Voice actions independent of web UI. Heavy research stays in the PC service. */
#include "voice_control.h"
#include "agent_extra.h"
#include "relative_timer.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static StaticSemaphore_t mutex_storage;static SemaphoreHandle_t mutex;
void ty_voice_control_init(void){mutex=xSemaphoreCreateRecursiveMutexStatic(&mutex_storage);}
static void enter(void){if(mutex)xSemaphoreTakeRecursive(mutex,portMAX_DELAY);}
static void leave(void){if(mutex)xSemaphoreGiveRecursive(mutex);}
static char pending_action[32],pending_goal[901],last_result[40]="none";
static char ack_speech[901];static int pending_value;static unsigned request_id,acknowledged;static int64_t expires;
static const char *str(const cJSON *j,const char *k){cJSON *v=cJSON_GetObjectItem(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static bool has(const char *t,const char *s){return strstr(t,s)!=NULL;}
void ty_voice_control_clear(void){enter();pending_action[0]=pending_goal[0]=ack_speech[0]=0;leave();}
static bool request(const char *action,int value,const char *goal){
 enter();
 snprintf(pending_action,sizeof(pending_action),"%s",action);snprintf(pending_goal,sizeof(pending_goal),"%s",goal?goal:"");pending_value=value;request_id++;expires=esp_timer_get_time()+60000000LL;ack_speech[0]=0;
 snprintf(last_result,sizeof(last_result),"waiting_for_PC_research_service");
 leave();
 if(!strcmp(action,"proactive_goal"))ty_say("好，我先想想，等我一下。");
 else if(strcmp(action,"proactive_accept")&&strcmp(action,"proactive_reject"))ty_say("好，我试着切换，完成后告诉你。");
 return true;
}
cJSON *ty_voice_control_status(void){
 enter();
 if(*pending_action&&esp_timer_get_time()>expires){ty_voice_control_clear();snprintf(last_result,sizeof(last_result),"PC_service_timeout");snprintf(ack_speech,sizeof(ack_speech),"研究服务暂时没连上，请稍后再试。");}
 if(*ack_speech){if(ty_audio_muted())ack_speech[0]=0;else if(ty_say_if_idle(ack_speech))ack_speech[0]=0;}
 cJSON *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"request_id",request_id);cJSON_AddNumberToObject(j,"acknowledged_id",acknowledged);cJSON_AddBoolToObject(j,"pending",*pending_action!=0);cJSON_AddStringToObject(j,"result",last_result);leave();return j;
}
cJSON *ty_voice_control_pending(void){
 enter();
 cJSON *j=ty_voice_control_status();cJSON_AddNumberToObject(j,"boot_id",ty_board_boot_id());
 if(*pending_action){cJSON_AddStringToObject(j,"action",pending_action);cJSON_AddNumberToObject(j,"value",pending_value);if(*pending_goal)cJSON_AddStringToObject(j,"goal",pending_goal);}leave();return j;
}
bool ty_voice_control_ack(const cJSON *j){
 enter();
 cJSON *boot=cJSON_GetObjectItem(j,"boot_id"),*id=cJSON_GetObjectItem(j,"request_id"),*ok=cJSON_GetObjectItem(j,"ok");
 if(!cJSON_IsNumber(boot)||(uint32_t)boot->valuedouble!=ty_board_boot_id()||!cJSON_IsNumber(id)||(unsigned)id->valuedouble!=request_id||!*pending_action||!cJSON_IsBool(ok)||esp_timer_get_time()>expires||ty_audio_muted()){leave();return false;}
 acknowledged=request_id;bool success=cJSON_IsTrue(ok);snprintf(last_result,sizeof(last_result),"%s",success?"applied_by_PC_research_service":"PC_research_action_failed");ty_voice_control_clear();
 const char *spoken=str(j,"spoken_reply");if(strlen(spoken)>850){leave();return true;}
 snprintf(ack_speech,sizeof(ack_speech),"%s",*spoken?spoken:success?"好，处理好了。":"这次没有完成，研究服务还没准备好。");leave();return true;
}
static bool schedule(const char *text){
 ty_voice_schedule_t spec;int parsed=ty_parse_voice_schedule(text,time(NULL),&spec);if(!parsed)return false;
 if(parsed<0){ty_say("这个时间我没确认清楚，请再说一次时间和要提醒的事情。");return true;}
 if(!strcmp(spec.message,"时间到了。")&&has(text,"叫醒"))snprintf(spec.message,sizeof(spec.message),"该起床了。");
 cJSON *a=cJSON_CreateObject();char name[45];snprintf(name,sizeof(name),"voice_%08x",(unsigned)esp_random());
 cJSON_AddStringToObject(a,"action","add");cJSON_AddStringToObject(a,"name",name);cJSON_AddStringToObject(a,"task_kind","reminder");cJSON_AddStringToObject(a,"message",spec.message);
 cJSON_AddStringToObject(a,"schedule_type",spec.at_epoch?"at":spec.repeating?"every":"after");
 cJSON_AddNumberToObject(a,spec.at_epoch?"at_epoch":spec.repeating?"interval_s":"delay_s",spec.at_epoch?(double)spec.at_epoch:spec.seconds);
 cJSON *r=ty_agent_extra("agent_schedule",a);bool ok=cJSON_IsTrue(cJSON_GetObjectItem(r,"ok"));cJSON_Delete(a);cJSON_Delete(r);
 ty_event("voice_schedule_registered","local_grammar",ok,spec.at_epoch?0:spec.seconds);ty_say(ok?"好，提醒设好了。":"这次提醒没有设好，请检查是否已经有太多计划。");return true;
}
static bool manage(const char *text){
 bool cancel=has(text,"取消")||has(text,"删除"),list=has(text,"有哪些")||has(text,"查看")||has(text,"列出");
 if(!(has(text,"提醒")||has(text,"闹钟")||has(text,"计时"))||(!cancel&&!list))return false;
 cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"action","list");cJSON *r=ty_agent_extra("agent_schedule",a);cJSON_Delete(a);
 cJSON *all=cJSON_GetObjectItem(r,"jobs"),*job;unsigned count=0;char last_id[20]="";
 cJSON_ArrayForEach(job,all){if(!strcmp(str(job,"task_kind"),"reminder")&&cJSON_IsTrue(cJSON_GetObjectItem(job,"enabled"))){count++;snprintf(last_id,sizeof(last_id),"%s",str(job,"id"));}}
 if(list){char reply[180];snprintf(reply,sizeof(reply),"现在有%u个开启的提醒。你可以说取消刚才的提醒，或者取消所有提醒。",count);ty_say(reply);cJSON_Delete(r);return true;}
 bool all_requested=has(text,"所有")||has(text,"全部"),latest=has(text,"刚才")||has(text,"上一个")||has(text,"最近");
 if(!all_requested&&!latest){ty_say("要取消刚才的提醒，还是所有提醒？请说完整一点。");cJSON_Delete(r);return true;}
 unsigned cancelled=0;cJSON_ArrayForEach(job,all){if(strcmp(str(job,"task_kind"),"reminder")||!cJSON_IsTrue(cJSON_GetObjectItem(job,"enabled"))||(!all_requested&&strcmp(last_id,str(job,"id"))))continue;
  a=cJSON_CreateObject();cJSON_AddStringToObject(a,"action","cancel");cJSON_AddStringToObject(a,"id",str(job,"id"));cJSON *v=ty_agent_extra("agent_schedule",a);if(cJSON_IsTrue(cJSON_GetObjectItem(v,"ok")))cancelled++;cJSON_Delete(a);cJSON_Delete(v);
 }cJSON_Delete(r);ty_say(cancelled?"好，提醒取消了。":"没有找到开启的提醒。");return true;
}
bool ty_voice_controls(const char *text){
 /* An explicit goal owns its entire body: a paper topic such as VAP must
  * never be reinterpreted as a mode switch or a timer embedded in the goal. */
 const char *goal=strstr(text,"帮我想个建议");if(!goal)goal=strstr(text,"帮我考虑建议");
 if(goal){const char *p=goal+strlen(strstr(goal,"帮我考虑建议")==goal?"帮我考虑建议":"帮我想个建议");while(*p==' '||*p==':'||*p==',')p++;if(!strncmp(p,"：",3)||!strncmp(p,"，",3))p+=3;if(*p)return request("proactive_goal",0,p);ty_say("可以，请说帮我考虑建议，接着告诉我你的目标。");return true;}
 if(schedule(text)||manage(text))return true;
 if(has(text,"现在几点")||has(text,"今天几号")){
  cJSON *a=cJSON_CreateObject(),*r=ty_agent_extra("get_current_time",a);char reply[100];const char *stamp=str(r,"local_time");int y,m,d,h,n,s;
  if(sscanf(stamp,"%d-%d-%d %d:%d:%d",&y,&m,&d,&h,&n,&s)==6)snprintf(reply,sizeof(reply),"今天是%d月%d日，现在是%d点%d分。",m,d,h,n);else snprintf(reply,sizeof(reply),"时钟还没同步好，我先不猜时间。");
  cJSON_Delete(a);cJSON_Delete(r);ty_say(reply);return true;
 }
 bool off=has(text,"关闭")||has(text,"退出")||has(text,"不要")||has(text,"取消")||has(text,"停止")||has(text,"停用");
 if(has(text,"智能断句")||has(text,"VAP")||has(text,"原版断句"))return request("vap_mode",off||has(text,"原版")?0:has(text,"观察")?1:2,NULL);
 if(has(text,"倾听点头")||has(text,"倾听动作")||has(text,"应答机会"))return request("listener_mode",off?0:has(text,"观察")?1:2,NULL);
 if(has(text,"参与状态观察")||has(text,"参与度观察"))return request("engagement",off?0:1,NULL);
 if(has(text,"平衡交互")||has(text,"朝向汇报"))return request("balanced",off?0:1,NULL);
 if(has(text,"接受建议")||has(text,"按这个建议做"))return request("proactive_accept",1,NULL);
 if(has(text,"不用这个建议")||has(text,"拒绝建议")||has(text,"不要这个建议"))return request("proactive_reject",0,NULL);
 return false;
}

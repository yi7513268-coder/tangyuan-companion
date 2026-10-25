/* Live, explicit rule baseline. Not VAP or a reproduction of CACAS weights.
 * Gates actual task-result speech; speech completion is confirmed by played_id.
 */
#include "interaction.h"
#include "interaction_intent.h"
#include "robot.h"
#include "turn_trace.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static SemaphoreHandle_t mutex;
static char pending[1024];
static const char *reason="no_pending_result",*state="unknown";
static int64_t expires,busy_until,last_complete,visible_since;
static unsigned pending_epoch,submitted,completed_count,wait_count,feedback_count,spacing=30;
static unsigned pending_task_id,last_delivered_task_id;
static bool direct_reply,explicit_ready,feedback_used,availability_clarification;
static double num(cJSON *j,const char *k){cJSON *v=cJSON_GetObjectItem(j,k);return cJSON_IsNumber(v)?v->valuedouble:0;}
static bool yes(cJSON *j,const char *k){return cJSON_IsTrue(cJSON_GetObjectItem(j,k));}
bool ty_interaction_busy(void){
 if(!mutex)return false;
 xSemaphoreTake(mutex,portMAX_DELAY);bool held=esp_timer_get_time()<busy_until;xSemaphoreGive(mutex);return held;
}
void ty_interaction_clear(void){
 if(!mutex)return;
 xSemaphoreTake(mutex,portMAX_DELAY);pending[0]=0;submitted=0;availability_clarification=false;reason="cancelled_or_replaced";xSemaphoreGive(mutex);
}
void ty_interaction_result(const char *text,bool voice_task){
 if(!mutex||!text||!*text)return;
 cJSON *a=ty_audio_status();unsigned epoch=(unsigned)num(a,"speech_epoch");bool quiet=yes(a,"muted");cJSON_Delete(a);
 xSemaphoreTake(mutex,portMAX_DELAY);
 if(!quiet){snprintf(pending,sizeof(pending),"%s",text);pending_task_id=ty_current_task_id();expires=esp_timer_get_time()+900000000LL;
  pending_epoch=epoch;submitted=0;direct_reply=voice_task;reason="awaiting_delivery_decision";}
 else reason="quiet_result_available_in_task_record";
 xSemaphoreGive(mutex);
}
static void tick(void *unused){
 (void)unused;
 for(;;){
  vTaskDelay(pdMS_TO_TICKS(250));cJSON *a=ty_audio_status(),*r=ty_robot_status();int64_t now=esp_timer_get_time();
  bool present=yes(r,"fresh")&&num(r,"faces")>0&&num(r,"frame_age_ms")<1000;
  xSemaphoreTake(mutex,portMAX_DELAY);
  if(present){if(!visible_since)visible_since=now;}else visible_since=0;
  state=yes(a,"muted")?"quiet":now<busy_until?"user_requested_busy":
    (yes(a,"recording")||yes(a,"asr_pending")||yes(a,"playing")||ty_board_busy())?"engaged":
    !yes(r,"fresh")?"unknown":present?"present":"absent";
  if(*pending){
   if(now>expires||yes(a,"muted")||(unsigned)num(a,"speech_epoch")!=pending_epoch){
    pending[0]=0;submitted=0;availability_clarification=false;reason="expired_or_cancelled_no_replay";
   }else if(submitted){
    if((unsigned)num(a,"played_id")==submitted){
     pending[0]=0;submitted=0;last_complete=now;feedback_used=false;completed_count++;last_delivered_task_id=pending_task_id;reason="playback_completed";
    }else if((unsigned)num(a,"interrupted_id")==submitted){submitted=0;pending[0]=0;reason="interrupted_result_kept_in_task_record";}
    else if(!yes(a,"playing")&&!cJSON_GetObjectItem(a,"say_text"))reason="speech_failed_result_kept_until_expiry";
   }else{
    const char *wait=NULL;
    if(!yes(a,"ready"))wait="audio_unavailable";
    else if(yes(a,"recording")||yes(a,"asr_pending")||yes(a,"playing")||ty_board_busy()||cJSON_GetObjectItem(a,"say_text"))wait="conversation_or_speech_in_progress";
    else if(availability_clarification&&!explicit_ready)wait="awaiting_availability_clarification";
    else if(now<busy_until&&!explicit_ready)wait="user_requested_wait";
    else if(num(a,"silence_ms")<800)wait="waiting_for_audio_pause";
    else if(!direct_reply&&!explicit_ready&&(!present||now-visible_since<1500000))wait="waiting_for_fresh_presence";
    else if(!direct_reply&&!explicit_ready&&yes(a,"dialog_open"))wait="waiting_for_dialogue_window";
    else if(!direct_reply&&!explicit_ready&&last_complete&&now-last_complete<(int64_t)spacing*1000000)wait="explicit_feedback_spacing";
    if(wait){if(strcmp(reason,wait))wait_count++;reason=wait;}
    else{
     submitted=ty_say_if_idle(pending);
     if(submitted){if(direct_reply)ty_trace_reply(pending_task_id,submitted);explicit_ready=false;reason="speech_submitted_not_yet_delivered";}
     else reason="speech_channel_reserved_by_other_reply";
    }
   }
  }
  xSemaphoreGive(mutex);cJSON_Delete(a);cJSON_Delete(r);
 }
}
bool ty_interaction_control(const char *action){
 if(!mutex)return false;
 bool recognized=true;const char *reply=NULL;int64_t now=esp_timer_get_time();
 xSemaphoreTake(mutex,portMAX_DELAY);
 if(!strcmp(action,"busy")){busy_until=now+900000000LL;explicit_ready=false;availability_clarification=false;reply="好，我先等着。有结果会留好，要听时说汇报结果。";}
 else if(!strcmp(action,"available")){busy_until=0;explicit_ready=true;availability_clarification=false;
  if(!strcmp(reason,"speech_failed_result_kept_until_expiry"))submitted=0;
  reply=*pending?NULL:"好，我在。暂时没有等着播报的结果。";}
 else if(!strcmp(action,"feedback_intrusive")||!strcmp(action,"feedback_helpful")){
  if(last_complete&&!feedback_used&&now-last_complete<600000000LL){
   if(!strcmp(action,"feedback_intrusive")){spacing=spacing>=120?120:spacing*2;reply="记下了，接下来主动汇报的间隔会更长。";}
   else{spacing=spacing<=15?15:spacing/2;reply="收到，我会保留这样的汇报方式。";}
   feedback_count++;feedback_used=true;
   char prefs[120];snprintf(prefs,sizeof(prefs),"{\"spacing\":%u,\"feedback_count\":%u}",spacing,feedback_count);(void)ty_store_write("hri",prefs);
  }else reply="这次没有能对应的近期汇报，我先不猜是哪一次。";
 }else recognized=false;
 xSemaphoreGive(mutex);if(reply&&!ty_audio_muted())ty_say(reply);return recognized;
}
bool ty_interaction_user(const char *text){
 if(!mutex)return false;
 xSemaphoreTake(mutex,portMAX_DELAY);bool has_pending=*pending!=0;xSemaphoreGive(mutex);
 ty_hri_intent_t intent=ty_hri_intent(text,has_pending);
 if(intent==TY_HRI_BUSY)return ty_interaction_control("busy");
 if(intent==TY_HRI_AVAILABLE)return ty_interaction_control("available");
 if(intent==TY_HRI_CLARIFY){
  xSemaphoreTake(mutex,portMAX_DELAY);
  if(*pending){availability_clarification=true;explicit_ready=false;submitted=0;reason="awaiting_availability_clarification";}
  xSemaphoreGive(mutex);
  ty_say("你是要休息，还是听结果？要听结果请说汇报结果。");
  ty_event("availability_clarification","homophone",true,0);return true;
 }
 if(intent==TY_HRI_INTRUSIVE)return ty_interaction_control("feedback_intrusive");
 if(intent==TY_HRI_HELPFUL)return ty_interaction_control("feedback_helpful");
 return false;
}
cJSON *ty_interaction_status(void){
 cJSON *j=cJSON_CreateObject();if(!mutex)return j;xSemaphoreTake(mutex,portMAX_DELAY);
 cJSON_AddStringToObject(j,"state",state);cJSON_AddStringToObject(j,"reason",reason);
 cJSON_AddBoolToObject(j,"pending_result",*pending!=0);cJSON_AddNumberToObject(j,"submitted_say_id",submitted);
 cJSON_AddBoolToObject(j,"availability_clarification",availability_clarification);
 cJSON_AddNumberToObject(j,"delivered",completed_count);cJSON_AddNumberToObject(j,"wait_transitions",wait_count);
 cJSON_AddNumberToObject(j,"pending_task_id",pending_task_id);cJSON_AddNumberToObject(j,"last_delivered_task_id",last_delivered_task_id);
 cJSON_AddNumberToObject(j,"feedback_count",feedback_count);cJSON_AddNumberToObject(j,"proactive_spacing_seconds",spacing);
 cJSON_AddBoolToObject(j,"applied_to_live_result_delivery",true);cJSON_AddBoolToObject(j,"vap_controls_endpointing",false);
 cJSON_AddStringToObject(j,"method","live freshness-aware rules with explicit preference adaptation; not CACAS reproduction");
 cJSON_AddBoolToObject(j,"gaze_or_emotion_measured",false);xSemaphoreGive(mutex);return j;
}
void ty_interaction_init(void){
 mutex=xSemaphoreCreateMutex();char prefs[120];
 if(ty_store_read("hri",prefs,sizeof(prefs))==ESP_OK){cJSON *j=cJSON_Parse(prefs);unsigned n=(unsigned)num(j,"spacing");if(n>=15&&n<=120)spacing=n;feedback_count=(unsigned)num(j,"feedback_count");cJSON_Delete(j);}
 if(mutex)xTaskCreate(tick,"interaction_gate",4096,NULL,2,NULL);
}

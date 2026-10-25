/* Numeric RAM-only turn correlation. Never prompts, transcripts or media. */
#include "turn_trace.h"
#include "robot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>
typedef struct {unsigned uid,task,say,model_calls,model_ms;int64_t voice_end,capture_end,accepted,task_begin,model_end;} trace_t;
static trace_t rows[8];static unsigned cursor=0;static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static int64_t num(const cJSON *j,const char *k){cJSON *v=cJSON_GetObjectItem(j,k);return cJSON_IsNumber(v)?(int64_t)v->valuedouble:0;}
void ty_trace_begin(const cJSON *a){
 trace_t t={.uid=(unsigned)num(a,"utterance_id"),.voice_end=num(a,"utterance_voiced_end_ms"),.capture_end=num(a,"utterance_complete_ms"),.accepted=esp_timer_get_time()/1000};
 portENTER_CRITICAL(&mux);cursor=(cursor+1)%8;rows[cursor]=t;portEXIT_CRITICAL(&mux);
}
void ty_trace_task(unsigned task){portENTER_CRITICAL(&mux);rows[cursor].task=task;rows[cursor].task_begin=esp_timer_get_time()/1000;portEXIT_CRITICAL(&mux);}
void ty_trace_model(unsigned task,unsigned elapsed){portENTER_CRITICAL(&mux);for(unsigned i=0;i<8;i++){trace_t *t=&rows[i];if(task&&t->task==task){t->model_calls++;t->model_ms+=elapsed;t->model_end=esp_timer_get_time()/1000;}}portEXIT_CRITICAL(&mux);}
void ty_trace_reply(unsigned task,unsigned say){portENTER_CRITICAL(&mux);if(!task&&rows[cursor].task==0)rows[cursor].say=say;else for(unsigned i=0;i<8;i++)if(task&&rows[i].task==task)rows[i].say=say;portEXIT_CRITICAL(&mux);}
cJSON *ty_trace_status(void){
 trace_t copy[8];unsigned head;portENTER_CRITICAL(&mux);for(unsigned i=0;i<8;i++)copy[i]=rows[i];head=cursor;portEXIT_CRITICAL(&mux);
 cJSON *a=ty_audio_status(),*out=cJSON_CreateArray();
 for(unsigned i=0;i<8;i++){trace_t t=copy[(head+1+i)%8];if(!t.uid)continue;cJSON *j=cJSON_CreateObject();
  cJSON_AddNumberToObject(j,"utterance_id",t.uid);cJSON_AddNumberToObject(j,"task_id",t.task);cJSON_AddNumberToObject(j,"say_id",t.say);
  cJSON_AddNumberToObject(j,"voiced_end_ms",t.voice_end);cJSON_AddNumberToObject(j,"capture_end_ms",t.capture_end);cJSON_AddNumberToObject(j,"accepted_ms",t.accepted);
  cJSON_AddNumberToObject(j,"endpoint_wait_ms",t.capture_end-t.voice_end);cJSON_AddNumberToObject(j,"recognition_bridge_ms",t.accepted-t.capture_end);
  cJSON_AddNumberToObject(j,"model_calls",t.model_calls);cJSON_AddNumberToObject(j,"cloud_model_ms",t.model_ms);
  if(t.say&&(unsigned)num(a,"say_id")==t.say){
   int64_t requested=num(a,"speech_requested_ms");cJSON_AddNumberToObject(j,"reply_queued_ms",requested);cJSON_AddNumberToObject(j,"accepted_to_reply_ms",requested-t.accepted);
   if((unsigned)num(a,"playing_id")==t.say&&num(a,"playback_started_ms")>=t.accepted){
    int64_t start=num(a,"playback_started_ms");cJSON_AddNumberToObject(j,"play_started_ms",start);cJSON_AddNumberToObject(j,"speech_preparation_ms",start-requested);
    cJSON_AddNumberToObject(j,"voiced_end_to_play_ms",start-t.voice_end);cJSON_AddBoolToObject(j,"playback_completed",(unsigned)num(a,"played_id")==t.say);
   }
  }cJSON_AddItemToArray(out,j);
 }cJSON_Delete(a);return out;
}

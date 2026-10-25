#include "robot.h"
#include "cached_audio.h"
#include "watche_hw_audio.h"
#include "sensecap-watcher.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mbedtls/base64.h"

#define CAPTURE_BYTES 640000
#define PLAY_BYTES (2*1024*1024)
static SemaphoreHandle_t lock;
static atomic_bool muted=false,enabled=true,audio_ready=false,playing=false;
static atomic_uint epoch=1,volume=25;
static unsigned utterance_id=0,utterance_bytes=0,accepted_utterance=0,say_id=0;
static unsigned utterance_epoch=0;
static bool utterance_admitted=false;
static int64_t utterance_voiced_end=0;
static unsigned playing_id=0,played_id=0,interrupted_id=0,level=0;
static unsigned failed_id=0,local_audio_id=0;
static int64_t speech_requested_at=0;
static int64_t playback_started_at=0,playback_finished_at=0;
#define GENERATION_DEADLINE_US 60000000LL
static uint8_t *utterance,*playback;
static size_t playback_size=0;
static unsigned playback_epoch,playback_say;
static char say_text[1024];
static atomic_llong listen_until=0;
static atomic_llong last_speech_us=0,last_utterance_us=0;
static atomic_bool capturing=false;
static int audio_error;
static const char *audio_phase="starting";
/* RAM-only stream from the SAME microphone owner; no second codec lease.
 * Mode 0 keeps the old endpoint, 1 observes, 2 uses fresh mono VAP evidence.
 * No model availability => original 560ms endpoint. Never block on PC I/O. */
#define HRI_FRAMES 50
static uint8_t *hri_ring;
static unsigned hri_sequence,hri_frames,hri_epoch,hri_mode=0,hri_prediction_sequence;
static int64_t hri_prediction_at;
static float hri_now=1,hri_future=1,hri_vad=1;
static bool hri_stream_valid;
static unsigned hri_close_vap,hri_close_baseline,hri_close_timeout;
static unsigned hri_endpoint_silence_ms;
static const char *hri_last_endpoint="not_recorded";
static void hri_capture(const int16_t *frame,bool valid){
 if(!lock||!hri_ring)return;
 xSemaphoreTake(lock,portMAX_DELAY);
 unsigned current=atomic_load(&epoch);
 valid=valid&&hri_mode!=0;
 if(!valid||current!=hri_epoch){hri_frames=0;hri_prediction_at=0;hri_epoch=current;}
 hri_stream_valid=valid;
 if(valid){memcpy(hri_ring+(hri_sequence%HRI_FRAMES)*640,frame,640);hri_sequence++;if(hri_frames<HRI_FRAMES)hri_frames++;}
 xSemaphoreGive(lock);
}
static bool hri_endpoint(unsigned quiet){
 unsigned mode,lag;int64_t age;float n,f,v;
 xSemaphoreTake(lock,portMAX_DELAY);mode=hri_mode;lag=hri_sequence-hri_prediction_sequence;
 age=esp_timer_get_time()-hri_prediction_at;n=hri_now;f=hri_future;v=hri_vad;xSemaphoreGive(lock);
 bool fresh=age>=0&&age<400000&&lag<=20;
 if(mode!=2||!fresh){if(quiet>=28){hri_close_baseline++;hri_endpoint_silence_ms=quiet*20;hri_last_endpoint=mode==2?"baseline_model_unavailable":"baseline_560ms";return true;}return false;}
 if(quiet>=25&&n<.35f&&f<.35f&&v<.35f){hri_close_vap++;hri_endpoint_silence_ms=quiet*20;hri_last_endpoint="VAP_mono_candidate_500ms";return true;}
 if(quiet>=70){hri_close_timeout++;hri_endpoint_silence_ms=quiet*20;hri_last_endpoint="VAP_bounded_timeout_1400ms";return true;}
 return false;
}
bool ty_audio_muted(void){return atomic_load(&muted);}
void ty_audio_enable(bool on){atomic_store(&enabled,on);ty_setting_put("voice_on",on);}
void ty_audio_volume(unsigned value){if(value==0){ty_audio_mute(true);return;}if(value<=100){atomic_store(&volume,value);(void)ty_volume_setting(value,true);}}
void ty_audio_cancel(void){
 atomic_fetch_add(&epoch,1);atomic_store(&listen_until,0);
 if(lock){xSemaphoreTake(lock,portMAX_DELAY);say_text[0]=0;free(playback);playback=NULL;playback_size=0;xSemaphoreGive(lock);}
}
bool ty_audio_fail(unsigned job,unsigned generation){
 if(!lock)return false;
 xSemaphoreTake(lock,portMAX_DELAY);bool valid=job==say_id&&generation==atomic_load(&epoch)&&*say_text&&!playback&&!atomic_load(&playing);
 if(valid){say_text[0]=0;failed_id=job;audio_phase="voice_generation_failed";atomic_store(&listen_until,0);}xSemaphoreGive(lock);
 if(valid)ty_event("speech_generation_failed","PC_bridge",false,job);
 return valid;
}
void ty_audio_mute(bool value){
 atomic_store(&muted,value);ty_audio_cancel();
 ty_setting_put("muted",value);ty_expression(value?"quiet":"idle",2000);
 if(value){ty_board_quiet();}ty_event(value?"quiet_enabled":"quiet_released","speech",true,0);
}
void ty_dialog_begin(void){atomic_store(&listen_until,INT64_MAX);}
void ty_dialog_close(void){atomic_store(&listen_until,0);}
static unsigned say(const char *text,bool only_idle){
 if(!text||!*text||atomic_load(&muted)||!lock)return 0;
 size_t bytes=0;const unsigned char *cached=ty_cached_audio(text,&bytes);
 uint8_t *copy=cached?heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM):NULL;
 if(copy)memcpy(copy,cached,bytes);
 xSemaphoreTake(lock,portMAX_DELAY);
 if(atomic_load(&muted)||(only_idle&&(*say_text||playback||atomic_load(&playing)||atomic_load(&capturing)))){xSemaphoreGive(lock);free(copy);return 0;}
 snprintf(say_text,sizeof(say_text),"%s",text);say_id++;speech_requested_at=esp_timer_get_time();
 /* A newer reply replaces a buffer that has not started playing. */
 free(playback);playback=NULL;playback_size=0;
 if(copy){playback=copy;playback_size=bytes;playback_epoch=atomic_load(&epoch);playback_say=say_id;local_audio_id=say_id;}
 unsigned id=say_id;atomic_store(&listen_until,INT64_MAX);xSemaphoreGive(lock);return id;
}
void ty_say(const char *text){(void)say(text,false);}
unsigned ty_say_if_idle(const char *text){return say(text,true);}
static void owner(void *unused){
 (void)unused;int16_t frame[320];uint8_t *segment=heap_caps_malloc(CAPTURE_BYTES,MALLOC_CAP_SPIRAM);
 uint8_t *pre=heap_caps_calloc(10,sizeof(frame),MALLOC_CAP_SPIRAM);unsigned prepos=0,used=0,quiet=0,voiced=0,settle=0,last_volume=999;
 float noise=60;bool recording=false,recording_admitted=false;int64_t voiced_at=0;unsigned recording_epoch=atomic_load(&epoch);
 audio_error=watche_hw_audio_init(16000,atomic_load(&volume));atomic_store(&audio_ready,audio_error==ESP_OK);
 if(audio_error==ESP_OK)audio_error=bsp_exp_io_set_level(BSP_PWR_CODEC_PA,1);
 if(audio_error==ESP_OK)audio_error=bsp_codec_mute_set(atomic_load(&muted));
 atomic_store(&audio_ready,audio_error==ESP_OK);
 ty_event("audio_init","codec",audio_error==ESP_OK,(size_t)audio_error);
 if(audio_error!=ESP_OK||!segment||!pre){audio_phase="audio_init_failed";vTaskDelete(NULL);return;}
 audio_phase="listening";
 for(;;){
  unsigned target_volume=atomic_load(&muted)?0:atomic_load(&volume);
  if(last_volume!=target_volume){audio_error=watche_hw_audio_volume(target_volume);if(audio_error==ESP_OK)audio_error=bsp_codec_mute_set(target_volume==0);last_volume=target_volume;}
  uint8_t *pcm=NULL;size_t bytes=0;unsigned generation=0,job=0;
  xSemaphoreTake(lock,portMAX_DELAY);
  bool expired=*say_text&&!playback&&!atomic_load(&playing)&&esp_timer_get_time()-speech_requested_at>GENERATION_DEADLINE_US;
  unsigned expired_id=say_id;
  if(expired){failed_id=say_id;say_text[0]=0;atomic_store(&listen_until,0);}
  if(playback){pcm=playback;bytes=playback_size;generation=playback_epoch;job=playback_say;playback=NULL;playback_size=0;}
  if(pcm){atomic_store(&playing,true);playing_id=job;playback_started_at=esp_timer_get_time();}
  xSemaphoreGive(lock);
  if(expired)ty_event("speech_generation_timeout","local_watchdog",false,expired_id);
  if(pcm){
   hri_capture(frame,false);
   /* Codec reopen during lease configuration can leave the DAC muted.
    * This task owns the lease; explicitly prepare the physical output. */
   audio_error=bsp_exp_io_set_level(BSP_PWR_CODEC_PA,1);
   if(audio_error==ESP_OK)audio_error=bsp_codec_mute_set(false);
   bool complete=true;atomic_store(&playing,true);playing_id=job;audio_phase="speaking";ty_expression("speaking",60000);
   if(audio_error!=ESP_OK)complete=false;
   for(size_t offset=0;complete&&offset<bytes;){
    if(atomic_load(&muted)||generation!=atomic_load(&epoch)){complete=false;break;}
    size_t n=bytes-offset;if(n>sizeof(frame))n=sizeof(frame);
    audio_error=watche_hw_audio_write(pcm+offset,n);if(audio_error!=ESP_OK){complete=false;break;}offset+=n;
   }
   free(pcm);
   if(!complete)(void)watche_hw_audio_volume(0);
   /* DMA may still contain a short tail. Never begin the reply window early. */
   vTaskDelay(pdMS_TO_TICKS(160));atomic_store(&playing,false);last_volume=999;
   xSemaphoreTake(lock,portMAX_DELAY);
   playback_finished_at=esp_timer_get_time();
   if(complete)played_id=job;else interrupted_id=job;
   /* A newer pending reply owns the dialog window. */
   if(say_id==job){say_text[0]=0;atomic_store(&listen_until,complete?esp_timer_get_time()+10000000LL:0);}
   xSemaphoreGive(lock);
   ty_event(complete?"audio_write_finished":"audio_interrupted","speaker",complete,(unsigned)bytes);
   ty_expression(complete?"listening":"quiet",1500);audio_phase="listening";settle=20;recording=false;used=quiet=voiced=0;
  }
  audio_error=watche_hw_audio_read(frame,sizeof(frame));
  if(audio_error!=ESP_OK){audio_phase="microphone_failed";atomic_store(&audio_ready,false);ty_event("microphone_read_failed","codec",false,(size_t)audio_error);vTaskDelay(pdMS_TO_TICKS(1000));continue;}
  atomic_store(&audio_ready,true);
  unsigned energy=0;for(unsigned i=0;i<320;i++)energy+=(unsigned)abs(frame[i]);energy/=320;level=energy;
  hri_capture(frame,atomic_load(&enabled)&&!atomic_load(&muted)&&!settle);
  if(!atomic_load(&enabled)||settle){if(settle)settle--;recording=false;atomic_store(&capturing,false);used=quiet=voiced=0;continue;}
  unsigned threshold=(unsigned)(noise*3.2f);if(threshold<180)threshold=180;
  bool speech=energy>threshold;
  if(speech){voiced_at=esp_timer_get_time();atomic_store(&last_speech_us,voiced_at);}
  if(recording&&recording_epoch!=atomic_load(&epoch)){recording=false;used=quiet=voiced=0;}
  if(!recording){
   memcpy(pre+prepos*sizeof(frame),frame,sizeof(frame));prepos=(prepos+1)%10;
   if(!speech){atomic_store(&capturing,false);noise=noise*.99f+energy*.01f;continue;}
   for(unsigned i=0;i<10;i++)memcpy(segment+i*sizeof(frame),pre+((prepos+i)%10)*sizeof(frame),sizeof(frame));
   used=10*sizeof(frame);quiet=0;voiced=0;recording=true;recording_epoch=atomic_load(&epoch);int64_t window=atomic_load(&listen_until);recording_admitted=window!=INT64_MAX&&window>esp_timer_get_time();
  }else if(used+sizeof(frame)<=CAPTURE_BYTES){memcpy(segment+used,frame,sizeof(frame));used+=sizeof(frame);}
  if(speech){voiced++;quiet=0;}else quiet++;
  if(hri_endpoint(quiet)||used+sizeof(frame)>CAPTURE_BYTES){
   if(voiced>=7&&used>=12000){
    xSemaphoreTake(lock,portMAX_DELAY);memcpy(utterance,segment,used);utterance_bytes=used;utterance_epoch=recording_epoch;utterance_admitted=recording_admitted;utterance_voiced_end=voiced_at;utterance_id++;atomic_store(&last_utterance_us,esp_timer_get_time());xSemaphoreGive(lock);
   }
   used=quiet=voiced=0;recording=false;
  }
  atomic_store(&capturing,recording);
 }
}
void ty_audio_init(void){
 lock=xSemaphoreCreateMutex();utterance=heap_caps_malloc(CAPTURE_BYTES,MALLOC_CAP_SPIRAM);
 hri_ring=heap_caps_calloc(HRI_FRAMES,640,MALLOC_CAP_SPIRAM);
 atomic_store(&muted,ty_setting_get("muted",false));atomic_store(&enabled,ty_setting_get("voice_on",true));atomic_store(&volume,ty_volume_setting(25,false));
 if(lock&&utterance)xTaskCreate(owner,"audio_owner",6144,NULL,4,NULL);
}
cJSON *ty_audio_status(void){
 cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ready",atomic_load(&audio_ready));cJSON_AddStringToObject(j,"phase",audio_phase);
 if(!lock)return j;
 cJSON_AddBoolToObject(j,"enabled",atomic_load(&enabled));cJSON_AddBoolToObject(j,"muted",atomic_load(&muted));cJSON_AddBoolToObject(j,"playing",atomic_load(&playing));
 cJSON_AddStringToObject(j,"duplex","half_duplex; use screen/web quiet during playback");
 cJSON_AddNumberToObject(j,"volume",atomic_load(&volume));cJSON_AddNumberToObject(j,"speech_epoch",atomic_load(&epoch));
 cJSON_AddNumberToObject(j,"played_id",played_id);cJSON_AddNumberToObject(j,"playing_id",playing_id);cJSON_AddNumberToObject(j,"interrupted_id",interrupted_id);
 cJSON_AddNumberToObject(j,"level",level);cJSON_AddNumberToObject(j,"error",audio_error);
 cJSON_AddBoolToObject(j,"recording",atomic_load(&capturing));
 cJSON_AddNumberToObject(j,"silence_ms",(esp_timer_get_time()-atomic_load(&last_speech_us))/1000);
 int64_t until=atomic_load(&listen_until);cJSON_AddBoolToObject(j,"dialog_open",until>esp_timer_get_time());
 cJSON_AddBoolToObject(j,"reply_window_open",until!=INT64_MAX&&until>esp_timer_get_time());
 xSemaphoreTake(lock,portMAX_DELAY);cJSON_AddNumberToObject(j,"utterance_id",utterance_id);cJSON_AddNumberToObject(j,"utterance_bytes",utterance_bytes);
 cJSON_AddNumberToObject(j,"accepted_utterance_id",accepted_utterance);
 cJSON_AddNumberToObject(j,"failed_id",failed_id);cJSON_AddNumberToObject(j,"local_audio_id",local_audio_id);
 cJSON_AddBoolToObject(j,"local_audio_pending",local_audio_id==say_id&&(playback||atomic_load(&playing)));
 cJSON_AddNumberToObject(j,"generation_deadline_ms",GENERATION_DEADLINE_US/1000);
 cJSON_AddNumberToObject(j,"pending_age_ms",*say_text?(esp_timer_get_time()-speech_requested_at)/1000:0);
 cJSON_AddNumberToObject(j,"speech_requested_ms",speech_requested_at/1000);
 cJSON_AddNumberToObject(j,"playback_started_ms",playback_started_at/1000);
 cJSON_AddNumberToObject(j,"playback_finished_ms",playback_finished_at/1000);
 cJSON *hri=cJSON_AddObjectToObject(j,"turn_taking");
 cJSON_AddStringToObject(hri,"mode",hri_mode==2?"vap_assisted":hri_mode==1?"shadow":"baseline");
 cJSON_AddStringToObject(hri,"last_endpoint",hri_last_endpoint);
 cJSON_AddNumberToObject(hri,"last_endpoint_silence_ms",hri_endpoint_silence_ms);
 cJSON_AddNumberToObject(hri,"vap_endpoints",hri_close_vap);cJSON_AddNumberToObject(hri,"fallback_endpoints",hri_close_baseline);
 cJSON_AddNumberToObject(hri,"timeout_endpoints",hri_close_timeout);
 cJSON_AddBoolToObject(hri,"model_inference_on_robot",false);
 cJSON_AddBoolToObject(hri,"full_HRI2025_reproduction",false);
 cJSON_AddNumberToObject(j,"utterance_epoch",utterance_epoch);
 cJSON_AddNumberToObject(j,"utterance_complete_ms",atomic_load(&last_utterance_us)/1000);
 cJSON_AddNumberToObject(j,"utterance_voiced_end_ms",utterance_voiced_end/1000);
 cJSON_AddBoolToObject(j,"utterance_dialog_admitted",utterance_admitted&&utterance_epoch==atomic_load(&epoch)&&esp_timer_get_time()-atomic_load(&last_utterance_us)<15000000);
 cJSON_AddBoolToObject(j,"asr_pending",utterance_id!=accepted_utterance&&esp_timer_get_time()-atomic_load(&last_utterance_us)<15000000);
 cJSON_AddNumberToObject(j,"say_id",say_id);if(*say_text)cJSON_AddStringToObject(j,"say_text",say_text);xSemaphoreGive(lock);return j;
}
static bool number_header(httpd_req_t *r,const char *key,unsigned expected){
 char value[32],*end=NULL;if(httpd_req_get_hdr_value_str(r,key,value,sizeof(value))!=ESP_OK)return false;
 unsigned long n=strtoul(value,&end,10);return end!=value&&!*end&&n==expected;
}
esp_err_t ty_audio_api(void *request,const char *route){
 httpd_req_t *r=request;httpd_resp_set_hdr(r,"Cache-Control","no-store");
 if(!strcmp(route,"/hri_audio")){
  /* Up to one second, ordered oldest->newest. Sequence permits exact overlap
   * removal and discontinuity reset. Capture epoch prevents stale predictions. */
  unsigned count,seq,gen,mode;bool valid;char after_header[32],*end=NULL;unsigned after=0;bool has_after=false;
  if(httpd_req_get_hdr_value_str(r,"X-After-Sequence",after_header,sizeof(after_header))==ESP_OK){
   unsigned long n=strtoul(after_header,&end,10);has_after=end!=after_header&&!*end;after=(unsigned)n;
  }
  uint8_t *copy=heap_caps_malloc(HRI_FRAMES*640,MALLOC_CAP_SPIRAM);
  if(!copy)return ESP_ERR_NO_MEM;
  xSemaphoreTake(lock,portMAX_DELAY);seq=hri_sequence;count=hri_frames;gen=hri_epoch;mode=hri_mode;valid=hri_stream_valid;
  if(has_after&&after<=seq){unsigned delta=seq-after;if(delta<count)count=delta;}
  else if(count>5)count=5; /* Start with fresh 100ms, never a slow 1s burst. */
  if(!mode)count=0;
  for(unsigned i=0;i<count;i++)memcpy(copy+i*640,hri_ring+((seq-count+i)%HRI_FRAMES)*640,640);
  xSemaphoreGive(lock);
  size_t encoded=0;char *b64=heap_caps_calloc(1,(count*640+2)/3*4+1,MALLOC_CAP_SPIRAM);
  if(!b64){free(copy);return ESP_ERR_NO_MEM;}
  (void)mbedtls_base64_encode((unsigned char*)b64,(count*640+2)/3*4+1,&encoded,copy,count*640);free(copy);
  cJSON *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"boot_id",ty_board_boot_id());cJSON_AddNumberToObject(j,"speech_epoch",gen);
  cJSON_AddNumberToObject(j,"sequence",seq);cJSON_AddNumberToObject(j,"frames",count);cJSON_AddNumberToObject(j,"mode",mode);
  /* Zero incremental frames means "no new frame", not an invalid stream.
   * Consumers keep context and wait instead of repeatedly resetting models. */
  cJSON_AddBoolToObject(j,"valid",valid&&mode);cJSON_AddStringToObject(j,"pcm",b64);free(b64);
  char *body=cJSON_PrintUnformatted(j);cJSON_Delete(j);esp_err_t ret=httpd_resp_sendstr(r,body?body:"{}");free(body);return ret;
 }
 if(!strcmp(route,"/hri_prediction")){
  if(r->content_len<1||r->content_len>1000)return ESP_ERR_INVALID_ARG;
  char buf[1001]={0};int used=0,n;while(used<r->content_len&&(n=httpd_req_recv(r,buf+used,r->content_len-used))>0)used+=n;
  cJSON *j=used==r->content_len?cJSON_Parse(buf):NULL,*boot=cJSON_GetObjectItem(j,"boot_id"),*mode=cJSON_GetObjectItem(j,"mode");
  bool ok=cJSON_IsNumber(boot)&&(uint32_t)boot->valuedouble==ty_board_boot_id();
  if(ok&&cJSON_IsNumber(mode)){
   ok=mode->valuedouble==mode->valueint&&mode->valueint>=0&&mode->valueint<=2;
   if(ok){xSemaphoreTake(lock,portMAX_DELAY);hri_mode=mode->valueint;hri_prediction_at=0;xSemaphoreGive(lock);}
  }else if(ok){
   cJSON *gen=cJSON_GetObjectItem(j,"speech_epoch"),*seq=cJSON_GetObjectItem(j,"sequence");
   cJSON *now=cJSON_GetObjectItem(j,"p_now"),*future=cJSON_GetObjectItem(j,"p_future"),*vad=cJSON_GetObjectItem(j,"vad");
   ok=cJSON_IsNumber(gen)&&cJSON_IsNumber(seq)&&cJSON_IsNumber(now)&&cJSON_IsNumber(future)&&cJSON_IsNumber(vad)&&
     isfinite(now->valuedouble)&&isfinite(future->valuedouble)&&isfinite(vad->valuedouble)&&
     now->valuedouble>=0&&now->valuedouble<=1&&future->valuedouble>=0&&future->valuedouble<=1&&vad->valuedouble>=0&&vad->valuedouble<=1;
   if(ok){xSemaphoreTake(lock,portMAX_DELAY);
    ok=hri_stream_valid&&(unsigned)gen->valuedouble==hri_epoch&&hri_epoch==atomic_load(&epoch)&&
      (unsigned)seq->valuedouble<=hri_sequence&&hri_sequence-(unsigned)seq->valuedouble<=20&&
      (unsigned)seq->valuedouble>hri_prediction_sequence;
    if(ok){hri_now=now->valuedouble;hri_future=future->valuedouble;hri_vad=vad->valuedouble;hri_prediction_sequence=seq->valuedouble;hri_prediction_at=esp_timer_get_time();}
    xSemaphoreGive(lock);
   }
  }
  cJSON_Delete(j);if(!ok)httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,ok?"{\"accepted\":true}":"{\"accepted\":false}");
 }
 if(!strcmp(route,"/play")){
  if(r->content_len<2||r->content_len>PLAY_BYTES||r->content_len%2||atomic_load(&muted)||
    !number_header(r,"X-Boot-Id",ty_board_boot_id())||!number_header(r,"X-Speech-Epoch",atomic_load(&epoch))){httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,"{\"ok\":false,\"error\":\"stale_or_quiet\"}");}
  unsigned generation=atomic_load(&epoch),job=say_id;
  if(!number_header(r,"X-Say-Id",job)||job==played_id||job==interrupted_id||job==failed_id||atomic_load(&playing)){httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,"{\"ok\":false,\"error\":\"stale_answer\"}");}
  uint8_t *buf=heap_caps_malloc(r->content_len,MALLOC_CAP_SPIRAM);if(!buf)return ESP_ERR_NO_MEM;
  size_t used=0;int n;while(used<r->content_len&&(n=httpd_req_recv(r,(char*)buf+used,r->content_len-used))>0)used+=n;
  xSemaphoreTake(lock,portMAX_DELAY);
  bool ok=used==r->content_len&&generation==atomic_load(&epoch)&&!atomic_load(&muted)&&job==say_id&&job!=failed_id&&*say_text&&!playback&&!atomic_load(&playing);
  if(ok){playback=buf;playback_size=used;playback_epoch=generation;playback_say=job;}else free(buf);
  xSemaphoreGive(lock);if(!ok)httpd_resp_set_status(r,"409 Conflict");httpd_resp_set_type(r,"application/json");
  return httpd_resp_sendstr(r,ok?"{\"accepted\":true,\"physical_playback_verified\":false}":"{\"accepted\":false}");
 }
 if(!strcmp(route,"/audio")){
  char query[80],id[30];unsigned wanted=0;
  if(httpd_req_get_url_query_str(r,query,sizeof(query))==ESP_OK&&httpd_query_key_value(query,"id",id,sizeof(id))==ESP_OK)wanted=(unsigned)strtoul(id,NULL,10);
  xSemaphoreTake(lock,portMAX_DELAY);size_t bytes=utterance_bytes;uint8_t *copy=NULL;
  if(wanted==utterance_id&&bytes){copy=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM);if(copy)memcpy(copy,utterance,bytes);}
  xSemaphoreGive(lock);if(!copy){httpd_resp_set_status(r,"404 Not Found");return httpd_resp_sendstr(r,"{}");}
  httpd_resp_set_type(r,"application/octet-stream");esp_err_t e=httpd_resp_send(r,(char*)copy,bytes);free(copy);return e;
 }
 if(!strcmp(route,"/voice")){
  if(r->content_len<1||r->content_len>5000)return ESP_ERR_INVALID_ARG;
  char *body=calloc(1,r->content_len+1);if(!body)return ESP_ERR_NO_MEM;
  int used=0,n;while(used<r->content_len&&(n=httpd_req_recv(r,body+used,r->content_len-used))>0)used+=n;
  cJSON *j=used==r->content_len?cJSON_Parse(body):NULL;free(body);cJSON *id=cJSON_GetObjectItem(j,"utterance_id"),*text=cJSON_GetObjectItem(j,"text"),*boot=cJSON_GetObjectItem(j,"boot_id"),*generation=cJSON_GetObjectItem(j,"speech_epoch");
  bool valid=cJSON_IsString(text)&&cJSON_IsNumber(id)&&cJSON_IsNumber(boot)&&(uint32_t)boot->valuedouble==ty_board_boot_id()&&
    cJSON_IsNumber(generation)&&(unsigned)generation->valuedouble==atomic_load(&epoch)&&strlen(text->valuestring)<1500;
  bool duplicate=valid&&(unsigned)id->valuedouble==accepted_utterance;
  bool ok=duplicate||(valid&&utterance_epoch==atomic_load(&epoch)&&(unsigned)id->valuedouble==utterance_id);
  if(ok&&!duplicate){accepted_utterance=(unsigned)id->valuedouble;ty_voice_text(text->valuestring);}
  cJSON_Delete(j);if(!ok)httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,duplicate?"{\"accepted\":true,\"duplicate\":true}":ok?"{\"accepted\":true}":"{\"accepted\":false}");
 }
 return ESP_ERR_NOT_FOUND;
}

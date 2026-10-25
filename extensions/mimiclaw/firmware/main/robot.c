#include "robot.h"
#include "watche_hw_body.h"
#include "hal_camera.h"
#include "mcu_link_bootstrap.h"
#include "mcu_link_uart.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {unsigned id,epoch,submitted_ms;char action[20];int degrees;} command_t;
typedef struct {unsigned sequence,time,width,height,count;hal_camera_inference_box_t boxes[8];} frame_t;
typedef struct {
 int body,error,vision_error,original_model;
 bool follow,gestures,camera,pose_valid,fresh;
 unsigned command_id,completed_id,motion_seq,frames,faces,last_frame,pose_time,moves;
 int pan,tilt,command_result,target_pan,target_tilt,raw_motion_result,raw_motion_type;
 unsigned raw_exec_ms;
 bool command_cursor_valid;
 float face_x,face_y;unsigned face_score,face_seen_at;
 char follow_reason[32];
 char mode[20],action[20];
} state_t;
static state_t published;
static SemaphoreHandle_t state_lock,image_lock;
static QueueHandle_t commands,frames;
static atomic_bool follow_request=false,gesture_request=false;
static atomic_uint stop_epoch=0,request_id=0;
static atomic_uint touch_count=0,gesture_count=0;
static char *preview;
static unsigned preview_size,preview_sequence,preview_at;
static char expression[20]="idle";
static atomic_llong expression_until=0;
static unsigned ms(void){return (unsigned)(esp_timer_get_time()/1000);}
static int clamp(int n,int low,int high){return n<low?low:n>high?high:n;}
static void publish(state_t *s){xSemaphoreTake(state_lock,portMAX_DELAY);published=*s;xSemaphoreGive(state_lock);}
void ty_expression(const char *name,unsigned duration){
 if(!name)return;
 if(strcmp(name,"happy")&&strcmp(name,"sad")&&strcmp(name,"listening")&&strcmp(name,"thinking")&&strcmp(name,"speaking")&&strcmp(name,"idle")&&strcmp(name,"quiet")&&strcmp(name,"love")&&strcmp(name,"cheer")&&strcmp(name,"curious"))return;
 snprintf(expression,sizeof(expression),"%s",name);atomic_store(&expression_until,esp_timer_get_time()+duration*1000LL);
}
const char *ty_expression_name(void){return esp_timer_get_time()<atomic_load(&expression_until)?expression:"idle";}
void ty_touch_feedback(void){const char *moods[]={"happy","love","cheer","curious"};unsigned n=atomic_fetch_add(&touch_count,1);ty_expression(moods[n%4],2500);}
void ty_robot_stop(void){
 atomic_store(&follow_request,false);atomic_store(&gesture_request,false);atomic_fetch_add(&stop_epoch,1);
 /* The MCU owner observes this before all queued moves; no callback/network wait. */
}
void ty_robot_suspend(void){atomic_fetch_add(&stop_epoch,1);}
static void inferred(const hal_camera_inference_frame_t *f,void *unused){
 (void)unused;if(!f||!frames)return;
 frame_t copy={.sequence=f->sequence,.time=f->timestamp_ms,.width=f->frame_width,.height=f->frame_height};
 for(unsigned i=0;i<f->box_count&&i<8;i++)if(f->boxes[i].score>=60&&f->boxes[i].width>5&&f->boxes[i].height>5)copy.boxes[copy.count++]=f->boxes[i];
 xQueueOverwrite(frames,&copy);
 if(f->preview_image_base64&&f->preview_image_size<128*1024&&preview&&xSemaphoreTake(image_lock,0)==pdTRUE){
  memcpy(preview,f->preview_image_base64,f->preview_image_size);preview[f->preview_image_size]=0;
  preview_size=f->preview_image_size;preview_sequence=f->sequence;preview_at=ms();xSemaphoreGive(image_lock);
 }
}
static esp_err_t camera_start(state_t *s,bool images){
 esp_err_t e=hal_camera_init();if(e!=ESP_OK)return e;
 if(s->original_model<0){hal_camera_model_info_t current={0};if(hal_camera_get_model_info(&current)==ESP_OK)s->original_model=current.model_id;}
 hal_camera_model_info_t models[8]={0};size_t count=0;e=hal_camera_list_models(models,8,&count);int face=-1;
 if(e==ESP_OK)for(size_t i=0;i<count&&i<8;i++)if(models[i].verified&&models[i].contains_face_class){face=models[i].model_id;break;}
 if(e==ESP_OK&&face<0)e=ESP_ERR_NOT_FOUND;
 if(e==ESP_OK)e=hal_camera_set_active_model(face);
 hal_camera_inference_options_t options={.include_preview_image=images};
 if(e==ESP_OK)e=hal_camera_start_inference_ex(inferred,NULL,&options);
 if(e!=ESP_OK)(void)hal_camera_deinit();
 return e;
}
static void camera_stop(state_t *s){
 if(!s->camera)return;
 esp_err_t e=hal_camera_stop_inference();
 if(e==ESP_OK&&s->original_model>=0)e=hal_camera_set_active_model(s->original_model);
 if(e==ESP_OK)e=hal_camera_deinit();
 s->vision_error=e;s->camera=false;s->fresh=false;s->faces=0;
}
static bool move(state_t *s,int pan,int tilt,unsigned duration,bool manual){
 if(s->body!=WATCHE_HW_READY||!s->pose_valid)return false;
 pan=clamp(pan,30,150);tilt=clamp(tilt,100,130);
 uint32_t seq=0;esp_err_t e=watche_hw_body_move(pan,tilt,duration,&seq);s->error=e;
 if(e!=ESP_OK){if(manual)ty_event("motion_submit_failed","head",false,(size_t)e);return false;}
 s->motion_seq=seq;s->moves++;return true;
}
static bool relative_move(state_t *s,int dx,int dy,unsigned duration,bool manual){
 if(!s->command_cursor_valid)return false;
 return move(s,s->target_pan+dx,s->target_tilt+dy,duration,manual);
}
static void owner(void *unused){
 (void)unused;state_t s={.original_model=-1,.pan=90,.tilt=115};unsigned epoch=atomic_load(&stop_epoch),query_at=0,last_move=0,motion_deadline=0;
 int return_dx=0,return_dy=0;bool return_pending=false,manual_pending=false,images=false;float target_x=.5f,target_y=.5f;
 s.error=watche_hw_body_init();ty_event("body_init","head",s.error==ESP_OK,(size_t)s.error);
 atomic_store(&follow_request,ty_setting_get("follow",false));atomic_store(&gesture_request,ty_setting_get("gestures",false));
 for(;;){
  unsigned now=ms(),new_epoch=atomic_load(&stop_epoch);
  if(epoch!=new_epoch){
   epoch=new_epoch;xQueueReset(commands);(void)watche_hw_body_stop();s.motion_seq=0;s.command_cursor_valid=false;return_pending=manual_pending=false;
   s.completed_id=s.command_id;s.command_result=-1;ty_setting_put("follow",atomic_load(&follow_request));ty_setting_put("gestures",atomic_load(&gesture_request));ty_event("motion_stop","head",true,0);
  }
  watche_hw_status_t body={0};watche_hw_body_status(&body);
  if(s.body!=body.state)ty_event(body.state==WATCHE_HW_READY?"body_ready":"body_state","head",body.state==WATCHE_HW_READY,(size_t)body.last_error);
  s.body=body.state;
  if(body.state!=WATCHE_HW_READY){s.pose_valid=false;s.command_cursor_valid=false;}
  watche_hw_event_t e;
  for(int n=0;n<16&&watche_hw_body_next_event(&e)==ESP_OK;n++){
   if(e.type==WATCHE_HW_EVENT_POSITION&&e.size==sizeof(mcu_motion_servo_feedback_t)){
    mcu_motion_servo_feedback_t p;memcpy(&p,e.payload,sizeof(p));
    if((p.axis_mask&3)==3&&p.x_angle_x10>=0&&p.x_angle_x10<=1800&&p.y_angle_x10>=1000&&p.y_angle_x10<=1400){s.pan=p.x_angle_x10/10;s.tilt=p.y_angle_x10/10;s.pose_time=now;s.pose_valid=true;}
   }else if(e.type==WATCHE_HW_EVENT_MOTION&&e.size==sizeof(mcu_motion_lifecycle_event_t)){
    mcu_motion_lifecycle_event_t m;memcpy(&m,e.payload,sizeof(m));if(m.ref_seq!=s.motion_seq)continue;
    if(m.type==MCU_MOTION_LIFECYCLE_DONE||m.type==MCU_MOTION_LIFECYCLE_REJECTED||m.type==MCU_MOTION_LIFECYCLE_FAULT){
     s.raw_motion_type=m.type;s.raw_motion_result=m.result;s.raw_exec_ms=m.exec_ms;
     bool ok=m.type==MCU_MOTION_LIFECYCLE_DONE&&m.result==MCU_MOTION_RESULT_SUCCESS&&
       m.final_x_deg_x10>=300&&m.final_x_deg_x10<=1500&&m.final_y_deg_x10>=1000&&m.final_y_deg_x10<=1300;
     s.command_cursor_valid=ok;
     if(ok){s.target_pan=m.final_x_deg_x10/10;s.target_tilt=m.final_y_deg_x10/10;}
     else atomic_store(&follow_request,false);
     s.motion_seq=0;
     if(manual_pending&&return_pending&&ok){return_pending=false;if(relative_move(&s,return_dx,return_dy,450,true))motion_deadline=now+2500;else ok=false;}
     if(manual_pending&&!s.motion_seq){s.completed_id=s.command_id;s.command_result=ok?1:-1;manual_pending=false;ty_event("motion_result","head",ok,m.result);}
    }
   }else if(e.type==WATCHE_HW_EVENT_TOUCH){ty_touch_feedback();}
  }
  if(s.motion_seq&&now>motion_deadline){(void)watche_hw_body_stop();s.motion_seq=0;s.pose_valid=false;s.command_cursor_valid=false;atomic_store(&follow_request,false);
   if(manual_pending){s.completed_id=s.command_id;s.command_result=-1;manual_pending=return_pending=false;ty_event("motion_receipt_timeout","head",false,0);}}
  if(s.body==WATCHE_HW_READY&&!s.motion_seq&&now-query_at>1000){(void)watche_hw_body_request_position();query_at=now;}
  bool follow=atomic_load(&follow_request),gestures=atomic_load(&gesture_request);
  if(ty_audio_muted())follow=gestures=false;
  if((follow||gestures)&&(!s.camera||images!=gestures)){
   camera_stop(&s);images=gestures;s.vision_error=camera_start(&s,images);s.camera=s.vision_error==ESP_OK;
   ty_event("vision_start","camera",s.camera,(size_t)s.vision_error);
   if(!s.camera){atomic_store(&follow_request,false);atomic_store(&gesture_request,false);follow=gestures=false;}
  }else if(!(follow||gestures)&&s.camera)camera_stop(&s);
  if(s.follow!=follow){s.follow=follow;ty_setting_put("follow",atomic_load(&follow_request));}
  if(s.gestures!=gestures){s.gestures=gestures;ty_setting_put("gestures",atomic_load(&gesture_request));}
  snprintf(s.mode,sizeof(s.mode),"%s",s.follow?(s.gestures?"companion":"follow"):s.gestures?"gestures":"idle");
  command_t next={0};bool queued=xQueuePeek(commands,&next,0)==pdTRUE;
  if(!s.motion_seq&&!s.command_cursor_valid&&s.body==WATCHE_HW_READY&&s.pose_valid&&
    ((s.follow&&s.fresh&&s.faces>0)||(queued&&strcmp(next.action,"center")))){
   if(move(&s,90,115,2000,false)){motion_deadline=now+4000;ty_event("reference_requested","head",true,0);}
   else atomic_store(&follow_request,false);
  }
  frame_t f;
  if(xQueueReceive(frames,&f,0)==pdTRUE){
   s.frames++;s.last_frame=f.time;s.faces=f.count;s.fresh=now-f.time<700;
   if(s.fresh&&f.count&&f.width&&f.height){
    unsigned chosen=0;float best=1e9f;
    for(unsigned i=0;i<f.count;i++){float x=(float)f.boxes[i].x/f.width,y=(float)f.boxes[i].y/f.height;
     float cost=(x-target_x)*(x-target_x)+(y-target_y)*(y-target_y);if(cost<best){best=cost;chosen=i;}}
    target_x=(float)f.boxes[chosen].x/f.width;target_y=(float)f.boxes[chosen].y/f.height;
    s.face_x=target_x;s.face_y=target_y;s.face_score=f.boxes[chosen].score;s.face_seen_at=now;
    if(s.follow&&!s.motion_seq&&uxQueueMessagesWaiting(commands)==0&&s.pose_valid&&now-s.pose_time<2500&&now-last_move>180){
     float x=target_x-.5f,y=target_y-.5f;
     int dx=fabsf(x)>.07f?clamp((int)lroundf(-x*16),-5,5):0,dy=fabsf(y)>.10f?clamp((int)lroundf(-y*10),-2,2):0;
     if(dx&&clamp(s.target_pan+dx,30,150)==s.target_pan)dx=0;
     if(dy&&clamp(s.target_tilt+dy,100,130)==s.target_tilt)dy=0;
     if((dx||dy)&&relative_move(&s,dx,dy,220,false)){last_move=now;motion_deadline=now+1600;}
    }
   }
  }
  /* A lost frame suppresses new targets. Let an already bounded small move
   * finish so its completion reference is not discarded and replayed. */
  if(now-s.last_frame>900){s.fresh=false;s.faces=0;}
  snprintf(s.follow_reason,sizeof(s.follow_reason),"%s",!s.follow?"disabled":s.body!=WATCHE_HW_READY?"body_unavailable":!s.fresh?"frame_stale":!s.faces?"no_face":!s.pose_valid?"position_unavailable":s.motion_seq?"motion_pending":!s.command_cursor_valid?"reference_pending":now-s.pose_time>=2500?"position_stale":fabsf(s.face_x-.5f)<=.07f&&fabsf(s.face_y-.5f)<=.10f?"centered":"tracking_or_angle_limit");
  command_t cmd;
  if(!s.motion_seq&&s.pose_valid&&now-s.pose_time<2500&&xQueueReceive(commands,&cmd,0)==pdTRUE&&cmd.epoch==epoch){
   s.command_id=cmd.id;s.command_result=0;snprintf(s.action,sizeof(s.action),"%s",cmd.action);
   if(now-cmd.submitted_ms>5000){s.completed_id=cmd.id;s.command_result=-1;ty_event("stale_motion_dropped","head",false,cmd.id);publish(&s);continue;}
   int dx=0,dy=0,d=clamp(cmd.degrees,1,5);return_pending=false;
   if(!strcmp(cmd.action,"left"))dx=d;else if(!strcmp(cmd.action,"right"))dx=-d;
   else if(!strcmp(cmd.action,"up"))dy=d;else if(!strcmp(cmd.action,"down"))dy=-d;
   else if(!strcmp(cmd.action,"nod")){dy=3;return_pending=true;}
   else if(!strcmp(cmd.action,"shake")){dx=5;return_pending=true;}
   return_dx=-dx;return_dy=-dy;
   manual_pending=s.pose_valid&&now-s.pose_time<2500&&(!strcmp(cmd.action,"center")?move(&s,90,115,2000,true):relative_move(&s,dx,dy,450,true));
   if(manual_pending){motion_deadline=now+4000;last_move=now;}
   else{s.completed_id=cmd.id;s.command_result=-1;return_pending=false;}
  }
  publish(&s);vTaskDelay(pdMS_TO_TICKS(20));
 }
}
void ty_robot_init(void){
 state_lock=xSemaphoreCreateMutex();image_lock=xSemaphoreCreateMutex();commands=xQueueCreate(4,sizeof(command_t));frames=xQueueCreate(1,sizeof(frame_t));
 preview=heap_caps_malloc(128*1024+1,MALLOC_CAP_SPIRAM);if(!state_lock||!image_lock||!commands||!frames)return;
 xTaskCreate(owner,"robot_owner",6144,NULL,4,NULL);
}
cJSON *ty_robot_status(void){
 state_t s={0};if(state_lock){xSemaphoreTake(state_lock,portMAX_DELAY);s=published;xSemaphoreGive(state_lock);}
 cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"mode",s.mode);cJSON_AddNumberToObject(j,"body_state",s.body);
 cJSON_AddNumberToObject(j,"error",s.error);cJSON_AddNumberToObject(j,"vision_error",s.vision_error);
 cJSON_AddBoolToObject(j,"follow",s.follow);cJSON_AddBoolToObject(j,"gestures",s.gestures);cJSON_AddBoolToObject(j,"fresh",s.fresh);
 cJSON_AddBoolToObject(j,"camera",s.camera);
 cJSON_AddNumberToObject(j,"faces",s.faces);cJSON_AddNumberToObject(j,"frames",s.frames);cJSON_AddNumberToObject(j,"frame_age_ms",ms()-s.last_frame);
 cJSON_AddBoolToObject(j,"pose_valid",s.pose_valid);cJSON_AddNumberToObject(j,"pan",s.pan);cJSON_AddNumberToObject(j,"tilt",s.tilt);
 cJSON_AddNumberToObject(j,"last_done_target_pan",s.target_pan);cJSON_AddNumberToObject(j,"last_done_target_tilt",s.target_tilt);
 cJSON_AddNumberToObject(j,"raw_motion_type",s.raw_motion_type);cJSON_AddNumberToObject(j,"raw_motion_result",s.raw_motion_result);cJSON_AddNumberToObject(j,"raw_motion_exec_ms",s.raw_exec_ms);
 cJSON_AddBoolToObject(j,"command_cursor_valid",s.command_cursor_valid);
 cJSON_AddStringToObject(j,"follow_reason",s.follow_reason);cJSON_AddNumberToObject(j,"face_x",s.face_x);cJSON_AddNumberToObject(j,"face_y",s.face_y);cJSON_AddNumberToObject(j,"face_score",s.face_score);cJSON_AddNumberToObject(j,"last_face_age_ms",ms()-s.face_seen_at);
 cJSON_AddStringToObject(j,"motion_control","small_absolute_targets_from_completed_reference; measured_feedback_separate");
 cJSON_AddNumberToObject(j,"command_id",s.command_id);cJSON_AddNumberToObject(j,"completed_id",s.completed_id);
 cJSON_AddNumberToObject(j,"command_result",s.command_result);cJSON_AddNumberToObject(j,"moves",s.moves);
 watche_hw_status_t actual={0};watche_hw_body_status(&actual);cJSON_AddNumberToObject(j,"body_error",actual.last_error);
 if(mcu_link_uart_try_lock()){
  mcu_link_t *link=mcu_link_bootstrap_get_link();
  if(link){mcu_link_peer_info_t peer={0};mcu_link_stats_t stats={0};
   (void)mcu_link_copy_peer_info(link,&peer);(void)mcu_link_copy_stats(link,&stats);
   cJSON_AddNumberToObject(j,"link_state",mcu_link_get_state(link));cJSON_AddBoolToObject(j,"peer_version_valid",peer.version_valid);
   cJSON_AddNumberToObject(j,"peer_capabilities",peer.capability_bitmap);cJSON_AddNumberToObject(j,"link_crc_errors",stats.crc_error_count);cJSON_AddNumberToObject(j,"link_ack_timeouts",stats.ack_timeout_count);
  }mcu_link_uart_unlock();
 }
 cJSON_AddNumberToObject(j,"touch_count",atomic_load(&touch_count));cJSON_AddNumberToObject(j,"gesture_actions",atomic_load(&gesture_count));
 cJSON_AddStringToObject(j,"expression",ty_expression_name());cJSON_AddStringToObject(j,"gesture_execution","PC MediaPipe bridge; robot camera");return j;
}
cJSON *ty_robot_preview(void){
 cJSON *j=cJSON_CreateObject();xSemaphoreTake(image_lock,portMAX_DELAY);
 cJSON_AddNumberToObject(j,"sequence",preview_sequence);cJSON_AddNumberToObject(j,"age_ms",preview_size?ms()-preview_at:2147483647);if(preview_size)cJSON_AddStringToObject(j,"jpeg_base64",preview);
 xSemaphoreGive(image_lock);return j;
}
cJSON *ty_robot_command(const cJSON *a){
 const cJSON *v=cJSON_GetObjectItem(a,"action");const char *action=cJSON_IsString(v)?v->valuestring:"";
 cJSON *r=cJSON_CreateObject();bool ok=true;unsigned id=0;
 if(ty_audio_muted()&&strcmp(action,"stop")){cJSON_AddBoolToObject(r,"ok",false);cJSON_AddStringToObject(r,"error","quiet_has_priority; explicitly_resume_first");return r;}
 if(!strcmp(action,"stop")){ty_robot_stop();}
 else if(!strcmp(action,"pause_motion")){atomic_store(&follow_request,false);atomic_fetch_add(&stop_epoch,1);}
 else if(!strcmp(action,"follow")){atomic_store(&follow_request,true);}
 else if(!strcmp(action,"follow_off")){atomic_store(&follow_request,false);}
 else if(!strcmp(action,"companion")){atomic_store(&follow_request,true);atomic_store(&gesture_request,true);}
 else if(!strcmp(action,"gestures")){atomic_store(&gesture_request,true);}
 else if(!strcmp(action,"gestures_off")){atomic_store(&gesture_request,false);}
 else if(!strcmp(action,"happy")||!strcmp(action,"sad")){ty_expression(action,3000);}
 else if(!strcmp(action,"left")||!strcmp(action,"right")||!strcmp(action,"up")||!strcmp(action,"down")||!strcmp(action,"center")||!strcmp(action,"nod")||!strcmp(action,"shake")){
  command_t cmd={.id=atomic_fetch_add(&request_id,1)+1,.epoch=atomic_load(&stop_epoch),.submitted_ms=ms(),.degrees=5};
  cJSON *n=cJSON_GetObjectItem(a,"degrees");if(cJSON_IsNumber(n))cmd.degrees=clamp(n->valueint,1,10);
  snprintf(cmd.action,sizeof(cmd.action),"%s",action);id=cmd.id;ok=commands&&xQueueSend(commands,&cmd,0)==pdTRUE;
 }else ok=false;
 cJSON_AddBoolToObject(r,"ok",ok);cJSON_AddNumberToObject(r,"request_id",id);
 cJSON_AddStringToObject(r,"state",ok?"requested; check hardware receipt and physical behavior":"invalid_action_or_queue_full");return r;
}
bool ty_robot_gesture(const char *name,float score){
 static unsigned last=0;if(score<.7f||ms()-last<3000||!atomic_load(&gesture_request))return false;
 const char *action=!strcmp(name,"Thumb_Up")?"nod":!strcmp(name,"Victory")?"happy":!strcmp(name,"Open_Palm")?"pause_motion":NULL;
 if(!action)return false;
 last=ms();cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"action",action);cJSON *r=ty_robot_command(a);bool ok=cJSON_IsTrue(cJSON_GetObjectItem(r,"ok"));cJSON_Delete(r);cJSON_Delete(a);
 if(ok){atomic_fetch_add(&gesture_count,1);}ty_event("gesture_action",name,ok,0);
 return ok;
}

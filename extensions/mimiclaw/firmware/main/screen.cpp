#include "watche_hw_display.h"
#include "freertos/task.h"
#include <cstring>
extern "C" {
#include "robot.h"
const char *ty_status(void);
const char *ty_board_ip(void);
void ty_board_preset(void);
}
static lv_obj_t *status_label,*face,*quiet_button_label;
static lv_obj_t *face_panel,*eye_left,*eye_right,*cheek_left,*cheek_right;
static void clicked(lv_event_t *e){
 int action=static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
 if(action==1)ty_board_preset();
 else if(action==2)ty_audio_mute(!ty_audio_muted());
 else if(action==3){cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"action","companion");cJSON *r=ty_robot_command(a);cJSON_Delete(r);cJSON_Delete(a);}
 else if(action==4&&!ty_audio_muted()){ty_audio_enable(true);ty_dialog_begin();ty_say("我在，你说。");}
 else ty_touch_feedback();
}
static void refresh(void*){
 unsigned tick=0;
 for(;;){
  const char *emotion=ty_audio_muted()?"quiet":ty_expression_name();
  if(watche_hw_display_lock(150)){
   bool love=!std::strcmp(emotion,"love"),cheer=!std::strcmp(emotion,"cheer"),happy=!std::strcmp(emotion,"happy"),quiet=!std::strcmp(emotion,"quiet"),speaking=!std::strcmp(emotion,"speaking"),thinking=!std::strcmp(emotion,"thinking"),listening=!std::strcmp(emotion,"listening"),curious=!std::strcmp(emotion,"curious");
   bool blink=!speaking&&!listening&&(tick%24==0||tick%24==1);
   int left_h=blink||quiet?5:happy?9:listening?35:30;
   int right_h=cheer?5:curious||thinking?18:left_h;
   lv_obj_set_height(eye_left,left_h);lv_obj_set_height(eye_right,right_h);
   lv_obj_align(eye_left,LV_ALIGN_CENTER,-45,-10);lv_obj_align(eye_right,LV_ALIGN_CENTER,45,-10);
   lv_color_t color=love?lv_color_hex(0xff8fb4):thinking?lv_color_hex(0xffdc90):lv_color_hex(0x88e8cc);
   lv_obj_set_style_bg_color(eye_left,color,0);lv_obj_set_style_bg_color(eye_right,color,0);
   lv_obj_set_style_bg_color(face_panel,quiet?lv_color_hex(0x111a27):love?lv_color_hex(0x372337):lv_color_hex(0x17263c),0);
   if(love||happy||cheer){lv_obj_clear_flag(cheek_left,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(cheek_right,LV_OBJ_FLAG_HIDDEN);}else{lv_obj_add_flag(cheek_left,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(cheek_right,LV_OBJ_FLAG_HIDDEN);}
   lv_label_set_text(face,love?"<3":cheer?"\\___/":happy?"\\___/":speaking?(tick%2?"O":"o"):thinking?"...":quiet?"z z":curious?"?":!std::strcmp(emotion,"sad")?"/---\\":"---");
   lv_label_set_text_fmt(status_label,"%s\nIP: %s\nAgent: robot / Voice: PC bridge",ty_status(),ty_board_ip());
   lv_label_set_text(quiet_button_label,ty_audio_muted()?"Resume":"QUIET / STOP");
   watche_hw_display_unlock();
  }tick++;vTaskDelay(pdMS_TO_TICKS(200));
 }
}
extern "C" void ty_screen_init(void){
 if(watche_hw_display_init()!=ESP_OK||!watche_hw_display_lock(1000))return;
 lv_obj_t *screen=lv_scr_act();lv_obj_set_style_bg_color(screen,lv_color_hex(0x101827),0);
 lv_obj_t *title=lv_label_create(screen);lv_label_set_text(title,"Tangyuan / MimiClaw");
 lv_obj_set_style_text_color(title,lv_color_white(),0);lv_obj_align(title,LV_ALIGN_CENTER,0,-155);
 lv_obj_t *face_pad=lv_btn_create(screen);lv_obj_set_size(face_pad,230,86);lv_obj_align(face_pad,LV_ALIGN_CENTER,0,-105);
 lv_obj_set_style_bg_color(face_pad,lv_color_hex(0x17263c),0);lv_obj_add_event_cb(face_pad,clicked,LV_EVENT_CLICKED,nullptr);
 face_panel=face_pad;
 eye_left=lv_obj_create(face_pad);eye_right=lv_obj_create(face_pad);
 lv_obj_t *eyes[]={eye_left,eye_right};for(int i=0;i<2;i++){lv_obj_t *eye=eyes[i];lv_obj_set_size(eye,30,30);lv_obj_set_style_radius(eye,LV_RADIUS_CIRCLE,0);lv_obj_set_style_border_width(eye,0,0);lv_obj_clear_flag(eye,LV_OBJ_FLAG_SCROLLABLE);lv_obj_clear_flag(eye,LV_OBJ_FLAG_CLICKABLE);}
 cheek_left=lv_obj_create(face_pad);cheek_right=lv_obj_create(face_pad);
 lv_obj_t *cheeks[]={cheek_left,cheek_right};for(int i=0;i<2;i++){lv_obj_t *cheek=cheeks[i];lv_obj_set_size(cheek,18,7);lv_obj_set_style_radius(cheek,LV_RADIUS_CIRCLE,0);lv_obj_set_style_border_width(cheek,0,0);lv_obj_set_style_bg_color(cheek,lv_color_hex(0xf2a3b5),0);lv_obj_clear_flag(cheek,LV_OBJ_FLAG_SCROLLABLE);lv_obj_clear_flag(cheek,LV_OBJ_FLAG_CLICKABLE);}
 lv_obj_align(cheek_left,LV_ALIGN_CENTER,-65,12);lv_obj_align(cheek_right,LV_ALIGN_CENTER,65,12);
 face=lv_label_create(face_pad);lv_label_set_text(face,"---");lv_obj_set_style_text_font(face,&lv_font_montserrat_14,0);lv_obj_set_style_text_color(face,lv_color_hex(0x88e8cc),0);lv_obj_align(face,LV_ALIGN_CENTER,0,24);
 status_label=lv_label_create(screen);lv_obj_set_style_text_color(status_label,lv_color_white(),0);lv_obj_set_style_text_align(status_label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_width(status_label,320);lv_obj_align(status_label,LV_ALIGN_CENTER,0,-30);
 const char *names[]={"Research papers","QUIET / STOP","Companion","Listen"};
 for(int i=0;i<4;i++){lv_obj_t *b=lv_btn_create(screen);lv_obj_set_size(b,145,43);lv_obj_align(b,LV_ALIGN_CENTER,i%2?78:-78,65+(i/2)*53);
  lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,names[i]);lv_obj_center(l);if(i==1)quiet_button_label=l;
  lv_obj_add_event_cb(b,clicked,LV_EVENT_CLICKED,reinterpret_cast<void *>(static_cast<intptr_t>(i+1)));
 }
 watche_hw_display_unlock();xTaskCreate(refresh,"mimi_screen",4096,nullptr,2,nullptr);
}

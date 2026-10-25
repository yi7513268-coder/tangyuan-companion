/* A small explicit public knowledge catalogue, NOT an onboard language model. */
#include "local_dialogue.h"
#include "local_dialogue_data.h"
#include "esp_timer.h"
#include <string.h>
static int64_t sky_until;static const char *last_id="none";static unsigned hits;
static int has(const char *s,const char *q){return strstr(s,q)!=NULL;}
void ty_local_dialogue_clear(void){sky_until=0;}
static const char *answer(const char *id,int sky){
 last_id=id;hits++;sky_until=sky?esp_timer_get_time()+30000000LL:0;
 for(unsigned i=0;i<TY_LOCAL_DIALOGUE_COUNT;i++)if(!strcmp(id,ty_local_dialogue_data[i].id))return ty_local_dialogue_data[i].answer;
 return NULL;
}
const char *ty_local_answer(const char *s){
 if(!s||strlen(s)>180)return NULL;
 /* Longer tool requests, stories and hypothetical claims go to the Agent. */
 const char *excluded[]={"帮我写","故事","论文","搜索","查一下","如果","假设","不对","记住","比较","诗","报告","改","以后","不要","并且","同时","然后","顺便","打开","文件","联网","上网","设置"};
 for(unsigned i=0;i<sizeof(excluded)/sizeof(excluded[0]);i++)if(has(s,excluded[i])){sky_until=0;return NULL;}
 if(has(s,"你叫什么")||has(s,"你的名字"))return answer("name",0);
 if(!strcmp(s,"你是谁")||!strcmp(s,"你是谁？")||!strcmp(s,"你是谁。"))return answer("identity",0);
 int sky=has(s,"天空")||has(s,"天是什么颜色");
 if(sky&&(has(s,"为什么")||has(s,"为何"))&&has(s,"蓝"))return answer("sky_why",1);
 if(sky&&(has(s,"什么颜色")||has(s,"是蓝色吗")||has(s,"是不是蓝色"))){
  if(has(s,"晚上")||has(s,"夜晚"))return answer("sky_night",1);
  if(!has(s,"阴天")&&!has(s,"下雨")&&!has(s,"日落"))return answer("sky_day",1);
 }
 if(esp_timer_get_time()<sky_until&&(has(s,"那晚上")||has(s,"夜晚呢")))return answer("sky_night",1);
 if(has(s,"月亮")&&(has(s,"自己发光")||has(s,"本身发光")))return answer("moon_light",0);
 if(has(s,"太阳")&&has(s,"升起")&&(has(s,"哪")||has(s,"什么方向")))return answer("sun_direction",0);
 if(has(s,"地球")&&has(s,"什么形状"))return answer("earth_shape",0);
 sky_until=0;return NULL;
}
cJSON *ty_local_dialogue_status(void){cJSON *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"public_entries",TY_LOCAL_DIALOGUE_COUNT);cJSON_AddNumberToObject(j,"hits",hits);cJSON_AddStringToObject(j,"last_intent",last_id);cJSON_AddBoolToObject(j,"onboard_llm",false);cJSON_AddStringToObject(j,"method","bounded public catalogue + short context + cached original voice; unmatched requests use Agent");return j;}

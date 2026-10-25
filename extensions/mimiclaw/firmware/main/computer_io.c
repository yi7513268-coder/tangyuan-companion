#include "computer_io.h"
#include "robot.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static SemaphoreHandle_t mutex;
static cJSON *pending,*result;
static unsigned sequence;
static int64_t heartbeat,expires;
static cJSON *failure(const char *code){cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",false);cJSON_AddStringToObject(j,"error",code);return j;}
void ty_computer_init(void){mutex=xSemaphoreCreateMutex();}
cJSON *ty_computer_status(void){
 cJSON *j=cJSON_CreateObject();if(!mutex)return j;
 xSemaphoreTake(mutex,portMAX_DELAY);
 cJSON_AddBoolToObject(j,"connected",heartbeat&&esp_timer_get_time()-heartbeat<5000000);
 cJSON_AddBoolToObject(j,"pending",pending!=NULL);cJSON_AddNumberToObject(j,"request_sequence",sequence);
 cJSON_AddStringToObject(j,"execution_location","computer; requests selected by robot Agent");
 xSemaphoreGive(mutex);return j;
}
cJSON *ty_computer_call(const char *name,const cJSON *args){
 if(!mutex)return failure("computer_bridge_not_initialized");
 xSemaphoreTake(mutex,portMAX_DELAY);
 if(!heartbeat||esp_timer_get_time()-heartbeat>5000000){xSemaphoreGive(mutex);return failure("computer_executor_offline");}
 if(pending){xSemaphoreGive(mutex);return failure("computer_request_already_pending");}
 cJSON_Delete(result);result=NULL;unsigned id=++sequence;
 pending=cJSON_CreateObject();cJSON_AddNumberToObject(pending,"boot_id",ty_board_boot_id());
 cJSON_AddNumberToObject(pending,"task_id",ty_current_task_id());cJSON_AddNumberToObject(pending,"request_id",id);
 cJSON_AddStringToObject(pending,"tool",name);cJSON_AddItemToObject(pending,"arguments",cJSON_Duplicate(args,true));
 expires=esp_timer_get_time()+65000000;
 xSemaphoreGive(mutex);
 for(;;){
  vTaskDelay(pdMS_TO_TICKS(100));xSemaphoreTake(mutex,portMAX_DELAY);
  bool cancelled=ty_cancelled(),timeout=esp_timer_get_time()>expires;
  if(result||cancelled||timeout){
   cJSON *out=!cancelled&&!timeout&&result?result:failure(cancelled?"task_cancelled":"computer_tool_timeout");
   if(out!=result){cJSON_Delete(result);}
   result=NULL;cJSON_Delete(pending);pending=NULL;
   xSemaphoreGive(mutex);return out;
  }
  xSemaphoreGive(mutex);
 }
}
esp_err_t ty_computer_api(void *request){
 httpd_req_t *r=request;cJSON *out=NULL;
 if(r->method==HTTP_GET){
  xSemaphoreTake(mutex,portMAX_DELAY);heartbeat=esp_timer_get_time();
  out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);
  cJSON_AddNumberToObject(out,"boot_id",ty_board_boot_id());cJSON_AddBoolToObject(out,"clock_ready",time(NULL)>=1735689600);
  if(pending&&!ty_cancelled()&&esp_timer_get_time()<expires)cJSON_AddItemToObject(out,"request",cJSON_Duplicate(pending,true));
  xSemaphoreGive(mutex);
 }else{
  if(r->content_len<1||r->content_len>11000)return ESP_ERR_INVALID_ARG;
  char *buf=calloc(1,r->content_len+1);if(!buf)return ESP_ERR_NO_MEM;int n,used=0;
  while(used<r->content_len&&(n=httpd_req_recv(r,buf+used,r->content_len-used))>0)used+=n;
  cJSON *j=used==r->content_len?cJSON_Parse(buf):NULL;free(buf);
  cJSON *clock=cJSON_GetObjectItem(j,"clock"),*boot=cJSON_GetObjectItem(j,"boot_id");
  if(cJSON_IsNumber(clock)){
   bool valid=cJSON_IsNumber(boot)&&(uint32_t)boot->valuedouble==ty_board_boot_id()&&clock->valuedouble>=1735689600&&clock->valuedouble<=4102444800;
   if(valid&&time(NULL)<1735689600){struct timeval tv={.tv_sec=(time_t)clock->valuedouble,.tv_usec=0};valid=settimeofday(&tv,NULL)==0;if(valid)ty_event("clock_sync","paired_PC",true,0);}
   cJSON_Delete(j);if(!valid)httpd_resp_set_status(r,"409 Conflict");
   return httpd_resp_sendstr(r,valid?"{\"accepted\":true}":"{\"accepted\":false}");
  }
  xSemaphoreTake(mutex,portMAX_DELAY);
  bool valid=pending&&!result&&!ty_cancelled()&&esp_timer_get_time()<expires;
  const char *keys[]={"boot_id","task_id","request_id"};
  for(unsigned i=0;i<3&&valid;i++){
   cJSON *a=cJSON_GetObjectItem(j,keys[i]),*b=cJSON_GetObjectItem(pending,keys[i]);
   valid=cJSON_IsNumber(a)&&cJSON_IsNumber(b)&&a->valuedouble==b->valuedouble;
  }
  cJSON *value=cJSON_GetObjectItem(j,"result"),*ok=cJSON_GetObjectItem(value,"ok");
  valid=valid&&cJSON_IsObject(value)&&cJSON_IsBool(ok);
  char *check=valid?cJSON_PrintUnformatted(value):NULL;
  valid=valid&&check&&strlen(check)<7800;free(check);
  if(valid)result=cJSON_Duplicate(value,true);
  xSemaphoreGive(mutex);cJSON_Delete(j);
  if(!valid){httpd_resp_set_status(r,"409 Conflict");}
  out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"accepted",valid);
 }
 char *text=cJSON_PrintUnformatted(out);cJSON_Delete(out);esp_err_t e=httpd_resp_sendstr(r,text?text:"{}");free(text);return e;
}

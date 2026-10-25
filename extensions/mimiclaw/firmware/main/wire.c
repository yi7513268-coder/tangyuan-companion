#include "wire.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
static const char *s(const cJSON *o,const char *key){cJSON *v=cJSON_GetObjectItem(o,key);return cJSON_IsString(v)?v->valuestring:"";}
cJSON *ty_openai_payload(const cJSON *request,const char *model){
    cJSON *r=cJSON_CreateObject(),*msgs=cJSON_CreateArray(),*system=cJSON_CreateObject();
    cJSON_AddStringToObject(r,"model",model);cJSON_AddNumberToObject(r,"max_tokens",2800);
    cJSON_AddBoolToObject(r,"stream",false);cJSON_AddBoolToObject(r,"parallel_tool_calls",false);
    cJSON_AddNumberToObject(r,"temperature",.1);cJSON *thinking=cJSON_CreateObject();
    cJSON_AddStringToObject(thinking,"type","disabled");cJSON_AddItemToObject(r,"thinking",thinking);
    cJSON_AddStringToObject(system,"role","system");cJSON_AddStringToObject(system,"content",s(request,"system"));cJSON_AddItemToArray(msgs,system);
    cJSON *m;cJSON_ArrayForEach(m,cJSON_GetObjectItem(request,"messages")){
        const char *role=s(m,"role");cJSON *content=cJSON_GetObjectItem(m,"content");
        if(cJSON_IsString(content)){cJSON_AddItemToArray(msgs,cJSON_Duplicate(m,true));continue;}
        if(!strcmp(role,"assistant")){
            cJSON *out=cJSON_CreateObject(),*calls=cJSON_CreateArray();cJSON_AddStringToObject(out,"role","assistant");
            cJSON *b;bool has_text=false;
            cJSON_ArrayForEach(b,content){
                if(!strcmp(s(b,"type"),"text")&&!has_text){cJSON_AddStringToObject(out,"content",s(b,"text"));has_text=true;}
                if(!strcmp(s(b,"type"),"tool_use")){
                    cJSON *call=cJSON_CreateObject(),*fn=cJSON_CreateObject();
                    cJSON_AddStringToObject(call,"id",s(b,"id"));cJSON_AddStringToObject(call,"type","function");
                    cJSON_AddStringToObject(fn,"name",s(b,"name"));char *args=cJSON_PrintUnformatted(cJSON_GetObjectItem(b,"input"));
                    cJSON_AddStringToObject(fn,"arguments",args?args:"{}");free(args);
                    cJSON_AddItemToObject(call,"function",fn);cJSON_AddItemToArray(calls,call);
                }
            }
            if(!has_text)cJSON_AddNullToObject(out,"content");
            if(cJSON_GetArraySize(calls))cJSON_AddItemToObject(out,"tool_calls",calls);else cJSON_Delete(calls);
            cJSON_AddItemToArray(msgs,out);
        }else{
            cJSON *b;cJSON_ArrayForEach(b,content){
                if(strcmp(s(b,"type"),"tool_result")){cJSON_Delete(r);cJSON_Delete(msgs);return NULL;}
                cJSON *out=cJSON_CreateObject();cJSON_AddStringToObject(out,"role","tool");
                cJSON_AddStringToObject(out,"tool_call_id",s(b,"tool_use_id"));cJSON_AddStringToObject(out,"content",s(b,"content"));cJSON_AddItemToArray(msgs,out);
            }
        }
    }cJSON_AddItemToObject(r,"messages",msgs);
    cJSON *tools=cJSON_CreateArray(),*t;
    cJSON_ArrayForEach(t,cJSON_GetObjectItem(request,"tools")){
        cJSON *out=cJSON_CreateObject(),*fn=cJSON_CreateObject();cJSON_AddStringToObject(out,"type","function");
        cJSON_AddStringToObject(fn,"name",s(t,"name"));cJSON_AddStringToObject(fn,"description",s(t,"description"));
        cJSON_AddItemToObject(fn,"parameters",cJSON_Duplicate(cJSON_GetObjectItem(t,"input_schema"),true));
        cJSON_AddItemToObject(out,"function",fn);cJSON_AddItemToArray(tools,out);
    }cJSON_AddItemToObject(r,"tools",tools);return r;
}
cJSON *ty_openai_response(const cJSON *response){
    cJSON *choice=cJSON_GetArrayItem(cJSON_GetObjectItem(response,"choices"),0);
    if(!cJSON_IsObject(choice)||!strcmp(s(choice,"finish_reason"),"length"))return NULL;
    cJSON *m=cJSON_GetObjectItem(choice,"message");if(!cJSON_IsObject(m))return NULL;
    cJSON *r=cJSON_CreateObject(),*calls=cJSON_CreateArray(),*c;cJSON_AddStringToObject(r,"text",s(m,"content"));
    cJSON_ArrayForEach(c,cJSON_GetObjectItem(m,"tool_calls")){
        cJSON *fn=cJSON_GetObjectItem(c,"function"),*args=cJSON_Parse(s(fn,"arguments"));
        if(!cJSON_IsObject(args)){cJSON_Delete(args);cJSON_Delete(r);cJSON_Delete(calls);return NULL;}
        cJSON *out=cJSON_CreateObject();cJSON_AddStringToObject(out,"id",s(c,"id"));cJSON_AddStringToObject(out,"name",s(fn,"name"));
        cJSON_AddItemToObject(out,"input",args);cJSON_AddItemToArray(calls,out);
    }cJSON_AddItemToObject(r,"calls",calls);return r;
}

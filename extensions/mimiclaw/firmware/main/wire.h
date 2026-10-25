#pragma once
#include "cJSON.h"
cJSON *ty_openai_payload(const cJSON *request,const char *model);
cJSON *ty_openai_response(const cJSON *response);

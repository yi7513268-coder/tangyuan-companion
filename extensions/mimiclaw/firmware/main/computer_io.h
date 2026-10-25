#pragma once
#include "port.h"
void ty_computer_init(void);
cJSON *ty_computer_call(const char *name,const cJSON *arguments);
esp_err_t ty_computer_api(void *request);
cJSON *ty_computer_status(void);
unsigned ty_current_task_id(void);

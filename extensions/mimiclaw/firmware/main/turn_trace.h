#pragma once
#include "cJSON.h"
void ty_trace_begin(const cJSON *audio);
void ty_trace_task(unsigned task);
void ty_trace_model(unsigned task,unsigned milliseconds);
void ty_trace_reply(unsigned task,unsigned say);
cJSON *ty_trace_status(void);

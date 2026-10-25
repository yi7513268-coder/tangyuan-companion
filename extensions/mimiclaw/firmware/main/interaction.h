#pragma once
#include "port.h"
void ty_interaction_init(void);
void ty_interaction_result(const char *text,bool voice_task);
void ty_interaction_clear(void);
bool ty_interaction_user(const char *text);
cJSON *ty_interaction_status(void);
bool ty_interaction_control(const char *action);
bool ty_interaction_busy(void);

#pragma once
#include "robot.h"
bool ty_voice_controls(const char *text);
void ty_voice_control_init(void);
cJSON *ty_voice_control_status(void);
cJSON *ty_voice_control_pending(void);
bool ty_voice_control_ack(const cJSON *body);
void ty_voice_control_clear(void);

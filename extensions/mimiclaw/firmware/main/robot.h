#pragma once
#include "port.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void ty_robot_init(void);
void ty_robot_stop(void);
void ty_robot_suspend(void);
cJSON *ty_robot_command(const cJSON *args);
cJSON *ty_robot_status(void);
cJSON *ty_robot_preview(void);
bool ty_robot_gesture(const char *name,float score);
void ty_expression(const char *name,unsigned milliseconds);
void ty_touch_feedback(void);
const char *ty_expression_name(void);
/* Speech functions are implemented by the one audio-owner task. */
void ty_audio_init(void);
void ty_audio_mute(bool mute);
void ty_audio_cancel(void);
bool ty_audio_fail(unsigned job,unsigned generation);
void ty_audio_enable(bool enabled);
void ty_audio_volume(unsigned percent);
bool ty_audio_muted(void);
void ty_say(const char *text);
unsigned ty_say_if_idle(const char *text);
cJSON *ty_audio_status(void);
esp_err_t ty_audio_api(void *request,const char *route);
void ty_voice_text(const char *text);
bool ty_direct_command(const char *text);
void ty_dialog_begin(void);
void ty_dialog_close(void);
void ty_dialog_clear(void);
void ty_board_cancel(void);
void ty_board_quiet(void);
esp_err_t ty_submit_task(const char *text);
esp_err_t ty_submit_scheduled_task(const char *text);
unsigned ty_current_task_id(void);
uint32_t ty_board_boot_id(void);
bool ty_board_busy(void);
bool ty_setting_get(const char *key,bool fallback);
void ty_setting_put(const char *key,bool value);
unsigned ty_volume_setting(unsigned value,bool write);
#ifdef __cplusplus
}
#endif

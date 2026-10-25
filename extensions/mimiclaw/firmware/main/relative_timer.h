#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
typedef struct {unsigned seconds;bool repeating;int64_t at_epoch;char message[601];} ty_voice_schedule_t;
int ty_parse_voice_schedule(const char *text,time_t now,ty_voice_schedule_t *out);
/* 0 unrelated, 1 parsed, -1 malformed/out of supported range. */
int ty_parse_relative_timer(const char *text,unsigned *seconds,char *message,size_t capacity);

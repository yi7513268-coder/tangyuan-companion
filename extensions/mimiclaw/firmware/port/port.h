#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#define TY_STORE_MAX 16384
esp_err_t ty_http_get(const char *url, char **body, size_t limit);
esp_err_t ty_llm_request(const cJSON *request, cJSON **response);
esp_err_t ty_store_write(const char *name, const char *value);
esp_err_t ty_store_read(const char *name, char *value, size_t capacity);
bool ty_cancelled(void);
void ty_event(const char *event, const char *tool, bool ok, size_t bytes);
void ty_begin_task(void);
unsigned ty_read_budget_used(void);
const char *ty_task_artifact_name(void);
void ty_set_status(const char *phase);
const char *ty_status(void);
const char *ty_runtime_name(void);
bool ty_listening_style(void);

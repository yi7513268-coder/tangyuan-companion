#pragma once
#include <stdbool.h>
typedef enum {TY_HRI_NONE,TY_HRI_BUSY,TY_HRI_AVAILABLE,TY_HRI_CLARIFY,
              TY_HRI_INTRUSIVE,TY_HRI_HELPFUL} ty_hri_intent_t;
ty_hri_intent_t ty_hri_intent(const char *text,bool has_pending_result);

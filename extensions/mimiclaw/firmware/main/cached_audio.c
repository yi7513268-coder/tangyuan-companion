#include "cached_audio.h"
/* No reference recordings redistributed. The configured PC TTS helper must provide audio. */
const unsigned char *ty_cached_audio(const char *text,size_t *bytes){(void)text;*bytes=0;return NULL;}

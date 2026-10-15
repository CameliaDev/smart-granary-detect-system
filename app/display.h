#ifndef DISPLAY_H
#define DISPLAY_H

#include "state.h"

/** Print sensor + AI + alarms to stdout (LCD substitute). */
void display_refresh(const app_state_t *st);

/** Optional framebuffer stub — currently no-op if FB unavailable. */
void display_framebuffer_stub(const app_state_t *st);

#endif

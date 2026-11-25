#pragma once
#include <stdint.h>
extern int64_t stub_now;
static inline int64_t esp_timer_get_time(void) { return stub_now; }

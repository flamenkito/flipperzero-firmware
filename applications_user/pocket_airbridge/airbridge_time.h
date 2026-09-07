#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Signed modular comparisons are valid while timestamps/counters differ by
 * less than half of UINT32_MAX, which is true for every AirBridge deadline and
 * queued-input window. */
static inline bool airbridge_u32_before(uint32_t lhs, uint32_t rhs) {
    return (int32_t)(lhs - rhs) < 0;
}

static inline bool airbridge_tick_reached(uint32_t now, uint32_t deadline) {
    return !airbridge_u32_before(now, deadline);
}

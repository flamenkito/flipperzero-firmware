#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    uint32_t tick;
    uint8_t step;
    uint32_t sent;
    uint32_t failed;
} AirbridgeMouse;

/* One attempt per minute, with no catch-up burst after a pause or tick wrap. */
static inline bool airbridge_mouse_due(AirbridgeMouse* mouse, bool active, uint32_t now) {
    if(!active || !mouse->active) {
        mouse->active = active;
        mouse->tick = now;
        mouse->step = 0;
        return false;
    }
    if(now - mouse->tick < 60000U) return false;
    mouse->tick = now;
    return true;
}

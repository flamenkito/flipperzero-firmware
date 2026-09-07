#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool* connected;
    uint32_t* connected_since;
    uint32_t* last_rx_tick;
    uint32_t* desync_since;
    uint32_t* generation;
} AirbridgeBleLinkStateRefs;

static inline bool airbridge_ble_link_apply_connected(
    AirbridgeBleLinkStateRefs state,
    uint32_t tick,
    uint32_t generation_advance) {
    if((generation_advance == 0) && *state.connected) return false;

    *state.generation += generation_advance;
    *state.connected = true;
    *state.connected_since = tick;
    *state.last_rx_tick = 0;
    *state.desync_since = 0;
    return true;
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef bool (*BtProfileReadersSnapshot)(void* context, uint32_t* readers);
typedef void (*BtProfileQuiescenceWait)(void* context);

typedef struct {
    void* context;
    BtProfileReadersSnapshot readers_snapshot;
    BtProfileQuiescenceWait wait_one_step;
} BtProfileQuiescenceOps;

static inline bool bt_profile_wait_quiescent_bounded(
    const BtProfileQuiescenceOps* ops,
    uint32_t max_wait_steps) {
    for(uint32_t waited = 0;; waited++) {
        uint32_t readers;
        if(!ops->readers_snapshot(ops->context, &readers)) return false;
        if(readers == 0) return true;
        if(waited >= max_wait_steps) return false;
        ops->wait_one_step(ops->context);
    }
}

#pragma once

#include <stdbool.h>

typedef struct {
    bool worker_quiesced;
    bool handlers_destroyed;
} BleEventTeardownOrder;

static inline BleEventTeardownOrder ble_event_teardown_order_initial(void) {
    return (BleEventTeardownOrder){0};
}

static inline bool
    ble_event_teardown_order_record_quiesce(BleEventTeardownOrder* order, bool quiesced) {
    order->worker_quiesced = quiesced;
    return quiesced;
}

static inline bool
    ble_event_teardown_order_may_destroy_handlers(const BleEventTeardownOrder* order) {
    return order->worker_quiesced;
}

static inline void
    ble_event_teardown_order_record_handler_destruction(BleEventTeardownOrder* order) {
    order->handlers_destroyed = true;
}

static inline bool
    ble_event_teardown_order_handlers_destroyed(const BleEventTeardownOrder* order) {
    return order->handlers_destroyed;
}

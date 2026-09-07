#pragma once

#include "airbridge_exit_contract.h"
#include "airbridge_operation.h"

typedef struct {
    void* context;
    /* Returns only after Closing has been committed to the display. */
    void (*show_closing)(void* context);
    bool (*restore_usb)(void* context);
    bool (*restore_ble)(void* context);
    uint32_t (*now)(void* context);
    void (*wait)(void* context);
} AirbridgeLifecycleOps;

/* A timeout only informs the independent supervisor. This owner must remain
 * loaded until these platform operations actually finish, including retries. */
static inline void airbridge_lifecycle_close(
    const AirbridgeLifecycleOps* ops,
    AirbridgeOperationMonitor* monitor,
    AirbridgeExitContract* contract) {
    airbridge_operation_start(monitor, AirbridgeOperationClosing, ops->now(ops->context));
    ops->show_closing(ops->context);

    airbridge_operation_start(monitor, AirbridgeOperationUsbStop, ops->now(ops->context));
    while(!airbridge_exit_contract_record_usb_detach(contract, ops->restore_usb(ops->context))) {
        ops->wait(ops->context);
    }

    airbridge_operation_start(monitor, AirbridgeOperationBleStop, ops->now(ops->context));
    while(!airbridge_exit_contract_record_ble_detach(contract, ops->restore_ble(ops->context))) {
        ops->wait(ops->context);
    }
    airbridge_operation_end(monitor);
}

#include <assert.h>
#include <stdint.h>

#include "../../applications_user/pocket_airbridge/airbridge_operation.h"

static void blocked_operation_is_reported_without_its_cooperation(void) {
    AirbridgeOperationMonitor monitor = {0};
    airbridge_operation_begin(&monitor, AirbridgeOperationBleSend, 1000, 100);
    assert(!airbridge_operation_observe(&monitor, 1099, false).stalled);
    AirbridgeOperationStatus status = airbridge_operation_observe(&monitor, 1100, false);
    assert(status.stalled);
    assert(status.operation == AirbridgeOperationBleSend);
}

static void pairing_time_does_not_exhaust_the_operation_budget(void) {
    AirbridgeOperationMonitor monitor = {0};
    airbridge_operation_begin(&monitor, AirbridgeOperationBleStart, 1000, 100);
    assert(!airbridge_operation_observe(&monitor, 1000, true).stalled);
    assert(!airbridge_operation_observe(&monitor, 5000, true).stalled);
    assert(!airbridge_operation_observe(&monitor, 5000, false).stalled);
    assert(!airbridge_operation_observe(&monitor, 5099, false).stalled);
    assert(airbridge_operation_observe(&monitor, 5100, false).stalled);
}

static void idle_and_completed_operations_do_not_stall(void) {
    AirbridgeOperationMonitor monitor = {0};
    assert(!airbridge_operation_observe(&monitor, 500000, false).stalled);
    airbridge_operation_begin(&monitor, AirbridgeOperationUsbStart, 1000, 100);
    assert(!airbridge_operation_observe(&monitor, 1099, false).stalled);
    airbridge_operation_end(&monitor);
    assert(!airbridge_operation_observe(&monitor, 900000, false).stalled);
}

static void late_completion_does_not_clear_a_reported_fault(void) {
    AirbridgeOperationMonitor monitor = {0};
    airbridge_operation_begin(&monitor, AirbridgeOperationBleStop, 1000, 100);
    assert(airbridge_operation_observe(&monitor, 1100, false).stalled);
    airbridge_operation_end(&monitor);
    assert(airbridge_operation_observe(&monitor, 1101, false).stalled);
}

static void deadline_survives_tick_wrap(void) {
    AirbridgeOperationMonitor monitor = {0};
    airbridge_operation_begin(&monitor, AirbridgeOperationBleReconnect, UINT32_MAX - 50, 100);
    assert(!airbridge_operation_observe(&monitor, 48, false).stalled);
    assert(airbridge_operation_observe(&monitor, 49, false).stalled);
}

static void pairing_does_not_hide_a_stalled_usb_operation(void) {
    AirbridgeOperationMonitor monitor = {0};
    airbridge_operation_begin(&monitor, AirbridgeOperationUsbStop, 1000, 100);
    assert(airbridge_operation_observe(&monitor, 1100, true).stalled);
}

static void operation_published_after_clock_sample_does_not_stall(void) {
    AirbridgeOperationMonitor monitor = {0};
    airbridge_operation_begin(&monitor, AirbridgeOperationBleSend, 1001, 100);
    assert(!airbridge_operation_observe(&monitor, 1000, false).stalled);
    assert(!airbridge_operation_observe(&monitor, 1100, false).stalled);
    assert(airbridge_operation_observe(&monitor, 1101, false).stalled);
}

int main(void) {
    blocked_operation_is_reported_without_its_cooperation();
    pairing_time_does_not_exhaust_the_operation_budget();
    idle_and_completed_operations_do_not_stall();
    late_completion_does_not_clear_a_reported_fault();
    deadline_survives_tick_wrap();
    pairing_does_not_hide_a_stalled_usb_operation();
    operation_published_after_clock_sample_does_not_stall();
    return 0;
}

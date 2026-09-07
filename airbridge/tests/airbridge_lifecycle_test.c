#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REQUIRE(condition)                 \
    do {                                   \
        if(!(condition)) __builtin_trap(); \
    } while(false)

typedef enum {
    BtStatusUnavailable,
    BtStatusOff,
    BtStatusAdvertising,
    BtStatusConnected,
} BtStatus;

typedef void (*BtStatusChangedCallback)(BtStatus status, void* context);

#define BT_STATUS_REGISTRATION_TYPES_DEFINED
#include "../../applications/services/bt/bt_service/bt_profile_quiescence.h"
#include "../../applications/services/bt/bt_service/bt_status_registration.h"
#include "../../targets/f7/ble_glue/gap_command.h"

typedef struct {
    bool snapshot_available;
    uint32_t readers;
    uint32_t snapshots;
    uint32_t waits;
} QuiescenceFixture;

static bool read_quiescence(void* context, uint32_t* readers) {
    QuiescenceFixture* fixture = context;
    fixture->snapshots++;
    if(!fixture->snapshot_available) return false;
    *readers = fixture->readers;
    return true;
}

static void wait_quiescence(void* context) {
    QuiescenceFixture* fixture = context;
    fixture->waits++;
}

static bool mutex_timeout_returns_within_bound(void) {
    QuiescenceFixture fixture = {.snapshot_available = false};
    const BtProfileQuiescenceOps ops = {
        .context = &fixture,
        .readers_snapshot = read_quiescence,
        .wait_one_step = wait_quiescence,
    };

    REQUIRE(!bt_profile_wait_quiescent_bounded(&ops, 10));
    REQUIRE(fixture.snapshots == 1);
    REQUIRE(fixture.waits == 0);
    return true;
}

static bool stuck_reader_returns_within_bound(void) {
    QuiescenceFixture fixture = {.snapshot_available = true, .readers = 1};
    const BtProfileQuiescenceOps ops = {
        .context = &fixture,
        .readers_snapshot = read_quiescence,
        .wait_one_step = wait_quiescence,
    };

    REQUIRE(!bt_profile_wait_quiescent_bounded(&ops, 10));
    REQUIRE(fixture.snapshots == 11);
    REQUIRE(fixture.waits == 10);
    return true;
}

typedef struct {
    uint32_t dispatch_depth;
    bool status_locked;
    bool callback_observed_lock;
    uint32_t callback_count;
    BtStatus current_status;
    BtStatus delivered_status;
    BtStatusChangedCallback callback;
    void* callback_context;
    BtStatusRegistration registration;
} StatusFixture;

static bool acquire_dispatch(void* context, uint32_t timeout) {
    (void)timeout;
    StatusFixture* fixture = context;
    fixture->dispatch_depth++;
    return true;
}

static void release_dispatch(void* context) {
    StatusFixture* fixture = context;
    REQUIRE(fixture->dispatch_depth > 0);
    fixture->dispatch_depth--;
}

static bool acquire_status(void* context, uint32_t timeout) {
    (void)timeout;
    StatusFixture* fixture = context;
    REQUIRE(fixture->dispatch_depth > 0);
    REQUIRE(!fixture->status_locked);
    fixture->status_locked = true;
    return true;
}

static void release_status(void* context) {
    StatusFixture* fixture = context;
    REQUIRE(fixture->status_locked);
    fixture->status_locked = false;
}

static BtStatus snapshot_status(void* context) {
    StatusFixture* fixture = context;
    REQUIRE(fixture->status_locked);
    REQUIRE(fixture->callback != NULL);
    REQUIRE(fixture->callback_context == fixture);
    return fixture->current_status;
}

static void record_status(BtStatus status, void* context) {
    StatusFixture* fixture = context;
    fixture->callback_count++;
    fixture->callback_observed_lock = fixture->status_locked;
    fixture->delivered_status = status;
}

static bool snapshot_delivery_is_ordered_outside_lock(void) {
    StatusFixture fixture = {.current_status = BtStatusConnected};
    fixture.registration = (BtStatusRegistration){
        .context = &fixture,
        .acquire_dispatch = acquire_dispatch,
        .release_dispatch = release_dispatch,
        .acquire_status = acquire_status,
        .release_status = release_status,
        .snapshot = snapshot_status,
        .callback_slot = &fixture.callback,
        .callback_context_slot = &fixture.callback_context,
    };

    BtStatus delivered;
    REQUIRE(bt_status_register_and_deliver_ordered(
        &fixture.registration, record_status, &fixture, 1U, &delivered));
    REQUIRE(delivered == BtStatusConnected);
    REQUIRE(!fixture.callback_observed_lock);
    REQUIRE(fixture.callback_count == 1);
    REQUIRE(fixture.delivered_status == BtStatusConnected);
    REQUIRE(fixture.callback == record_status);
    REQUIRE(fixture.callback_context == &fixture);
    REQUIRE(fixture.dispatch_depth == 0);
    REQUIRE(!fixture.status_locked);
    return true;
}

static void self_unregister_status(BtStatus status, void* context) {
    StatusFixture* fixture = context;
    record_status(status, context);
    REQUIRE(bt_status_callback_set_bounded(&fixture->registration, NULL, NULL, 1U));
}

static bool callback_can_unregister_itself(void) {
    StatusFixture fixture = {.current_status = BtStatusAdvertising};
    fixture.registration = (BtStatusRegistration){
        .context = &fixture,
        .acquire_dispatch = acquire_dispatch,
        .release_dispatch = release_dispatch,
        .acquire_status = acquire_status,
        .release_status = release_status,
        .snapshot = snapshot_status,
        .callback_slot = &fixture.callback,
        .callback_context_slot = &fixture.callback_context,
    };

    BtStatus delivered;
    REQUIRE(bt_status_register_and_deliver_ordered(
        &fixture.registration, self_unregister_status, &fixture, 1U, &delivered));
    REQUIRE(delivered == BtStatusAdvertising);
    REQUIRE(!fixture.callback_observed_lock);
    REQUIRE(fixture.callback_count == 1);
    REQUIRE(fixture.delivered_status == BtStatusAdvertising);
    REQUIRE(fixture.callback == NULL);
    REQUIRE(fixture.callback_context == NULL);
    REQUIRE(fixture.dispatch_depth == 0);
    REQUIRE(!fixture.status_locked);
    return true;
}

static bool stale_advfast_is_discarded_during_stop(void) {
    REQUIRE(!gap_command_allowed_during_stop(true, GapCommandAdvFast));
    REQUIRE(gap_command_allowed_during_stop(true, GapCommandKillThread));
    REQUIRE(gap_command_allowed_during_stop(false, GapCommandAdvFast));
    return true;
}

int main(void) {
    if(!mutex_timeout_returns_within_bound()) return 1;
    if(!stuck_reader_returns_within_bound()) return 1;
    if(!snapshot_delivery_is_ordered_outside_lock()) return 1;
    if(!callback_can_unregister_itself()) return 1;
    if(!stale_advfast_is_discarded_during_stop()) return 1;
    return 0;
}

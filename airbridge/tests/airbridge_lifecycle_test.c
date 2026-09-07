#include <stdbool.h>
#include <stdint.h>

#define REQUIRE(condition) \
    do {                   \
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
#include "../../furi/core/resumable_phase.h"
#include "../../furi/core/timer_delete_state.h"
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
    bool locked;
    bool callback_observed_lock;
    BtStatus current_status;
    BtStatus delivered_status;
    BtStatusChangedCallback callback;
    void* callback_context;
} StatusFixture;

static void lock_status(void* context) {
    StatusFixture* fixture = context;
    REQUIRE(!fixture->locked);
    fixture->locked = true;
}

static void unlock_status(void* context) {
    StatusFixture* fixture = context;
    REQUIRE(fixture->locked);
    fixture->locked = false;
}

static BtStatus snapshot_status(void* context) {
    StatusFixture* fixture = context;
    REQUIRE(fixture->locked);
    return fixture->current_status;
}

static void record_status(BtStatus status, void* context) {
    StatusFixture* fixture = context;
    fixture->callback_observed_lock = fixture->locked;
    fixture->delivered_status = status;
}

static bool snapshot_delivery_is_ordered_under_lock(void) {
    StatusFixture fixture = {.current_status = BtStatusConnected};
    const BtStatusRegistration registration = {
        .context = &fixture,
        .lock = lock_status,
        .unlock = unlock_status,
        .snapshot = snapshot_status,
        .callback_slot = &fixture.callback,
        .callback_context_slot = &fixture.callback_context,
    };

    BtStatus delivered =
        bt_status_register_and_deliver_ordered(&registration, record_status, &fixture);
    REQUIRE(delivered == BtStatusConnected);
    REQUIRE(fixture.callback_observed_lock);
    REQUIRE(fixture.delivered_status == BtStatusConnected);
    REQUIRE(fixture.callback == record_status);
    REQUIRE(fixture.callback_context == &fixture);
    REQUIRE(!fixture.locked);
    return true;
}

static bool stale_advfast_is_discarded_during_stop(void) {
    REQUIRE(!gap_command_allowed_during_stop(true, GapCommandAdvFast));
    REQUIRE(gap_command_allowed_during_stop(true, GapCommandKillThread));
    REQUIRE(gap_command_allowed_during_stop(false, GapCommandAdvFast));
    return true;
}

typedef struct {
    uint32_t delete_submissions;
    uint32_t fence_submissions;
    bool fence_complete;
} TimerDeleteFixture;

static bool submit_timer_delete(void* context) {
    TimerDeleteFixture* fixture = context;
    fixture->delete_submissions++;
    return true;
}

static bool submit_timer_delete_fence(void* context) {
    TimerDeleteFixture* fixture = context;
    fixture->fence_submissions++;
    return true;
}

static bool timer_delete_fence_complete(void* context) {
    TimerDeleteFixture* fixture = context;
    return fixture->fence_complete;
}

static bool timer_delete_timeout_resumes_without_resubmission(void) {
    TimerDeleteFixture fixture = {0};
    FuriTimerDeleteState state = {0};
    const FuriTimerDeleteOps ops = {
        .context = &fixture,
        .submit_delete = submit_timer_delete,
        .submit_fence = submit_timer_delete_fence,
        .fence_complete = timer_delete_fence_complete,
    };

    REQUIRE(!furi_timer_delete_state_run(&state, &ops));
    REQUIRE(fixture.delete_submissions == 1);
    REQUIRE(fixture.fence_submissions == 1);

    fixture.fence_complete = true;
    REQUIRE(furi_timer_delete_state_run(&state, &ops));
    REQUIRE(fixture.delete_submissions == 1);
    REQUIRE(fixture.fence_submissions == 1);
    return true;
}

typedef enum {
    TeardownPhaseHciReset,
    TeardownPhaseBleAppDeinit,
    TeardownPhaseFence,
    TeardownPhaseFree,
    TeardownPhaseDone,
} TeardownPhase;

typedef struct {
    uint32_t hci_resets;
    uint32_t ble_app_deinits;
    uint32_t fence_attempts;
    uint32_t resource_frees;
    bool hci_after_deinit;
} TeardownFixture;

static ResumablePhaseStepResult run_teardown_phase(void* context, uint8_t phase) {
    TeardownFixture* fixture = context;
    switch((TeardownPhase)phase) {
    case TeardownPhaseHciReset:
        if(fixture->ble_app_deinits != 0) fixture->hci_after_deinit = true;
        fixture->hci_resets++;
        return ResumablePhaseStepAdvanceAndRetry;
    case TeardownPhaseBleAppDeinit:
        fixture->ble_app_deinits++;
        return ResumablePhaseStepAdvance;
    case TeardownPhaseFence:
        fixture->fence_attempts++;
        return fixture->fence_attempts == 1 ? ResumablePhaseStepRetry :
                                             ResumablePhaseStepAdvance;
    case TeardownPhaseFree:
        fixture->resource_frees++;
        return ResumablePhaseStepAdvance;
    case TeardownPhaseDone:
        return ResumablePhaseStepComplete;
    }
    __builtin_unreachable();
}

static bool partial_teardown_retry_does_not_reexecute_completed_phases(void) {
    TeardownFixture fixture = {0};
    ResumablePhaseMachine machine = {.phase = TeardownPhaseHciReset};

    REQUIRE(!resumable_phase_machine_run(&machine, run_teardown_phase, &fixture));
    REQUIRE(machine.phase == TeardownPhaseBleAppDeinit);
    REQUIRE(!resumable_phase_machine_run(&machine, run_teardown_phase, &fixture));
    REQUIRE(machine.phase == TeardownPhaseFence);
    REQUIRE(resumable_phase_machine_run(&machine, run_teardown_phase, &fixture));
    REQUIRE(fixture.hci_resets == 1);
    REQUIRE(fixture.ble_app_deinits == 1);
    REQUIRE(fixture.resource_frees == 1);
    REQUIRE(!fixture.hci_after_deinit);
    return true;
}

int main(void) {
    if(!mutex_timeout_returns_within_bound()) return 1;
    if(!stuck_reader_returns_within_bound()) return 1;
    if(!snapshot_delivery_is_ordered_under_lock()) return 1;
    if(!stale_advfast_is_discarded_during_stop()) return 1;
    if(!timer_delete_timeout_resumes_without_resubmission()) return 1;
    if(!partial_teardown_retry_does_not_reexecute_completed_phases()) return 1;
    return 0;
}

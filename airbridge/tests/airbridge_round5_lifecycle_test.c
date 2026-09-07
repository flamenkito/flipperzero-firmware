#include <stdbool.h>
#include <stdint.h>

#include "../../applications_user/pocket_airbridge/airbridge_exit_contract.h"
#include "../../lib/stm32wb_copro/wpan/interface/patterns/ble_thread/tl/command_response_wait.h"
#include "../../targets/f7/ble_glue/event_teardown_order.h"
#include "../../targets/f7/ble_glue/gap_advertise_state.h"
#include "../../targets/f7/ble_glue/bt_radio_start_state.h"

#define REQUIRE(condition) \
    do {                   \
        if(!(condition)) __builtin_trap(); \
    } while(false)

typedef struct {
    uint32_t now;
    uint32_t release_at;
} CommandWaitFixture;

static bool command_released(void* context) {
    CommandWaitFixture* fixture = context;
    return fixture->now >= fixture->release_at;
}

static uint32_t command_now(void* context) {
    CommandWaitFixture* fixture = context;
    return fixture->now;
}

static void command_wait_step(void* context) {
    CommandWaitFixture* fixture = context;
    fixture->now++;
}

static bool shci_timeout_propagates_without_stale_response(void) {
    CommandWaitFixture fixture = {.release_at = 20};
    const BleCommandResponseWaitOps ops = {
        .context = &fixture,
        .is_released = command_released,
        .now = command_now,
        .wait_step = command_wait_step,
    };

    REQUIRE(!ble_command_response_wait_bounded(&ops, 10));
    REQUIRE(fixture.now == 10);
    REQUIRE(!command_released(&fixture));
    return true;
}

static bool callback_detach_deadline_retains_owner(void) {
    AirbridgeExitContract contract = airbridge_exit_contract_initial();

    REQUIRE(!airbridge_exit_contract_record_detach(&contract, false));
    REQUIRE(airbridge_exit_contract_owner_retained(&contract));
    REQUIRE(!airbridge_exit_contract_may_destroy(&contract));

    REQUIRE(airbridge_exit_contract_record_detach(&contract, true));
    REQUIRE(!airbridge_exit_contract_owner_retained(&contract));
    REQUIRE(airbridge_exit_contract_may_destroy(&contract));
    return true;
}

static bool timer_queue_saturation_unwinds_advertising(void) {
    GapAdvertiseState state = GapAdvertiseStateIdle;

    REQUIRE(gap_advertise_state_after_radio_start(&state, true, false) ==
            GapAdvertiseStartTimerRejected);
    REQUIRE(state == GapAdvertiseStateIdle);
    REQUIRE(gap_advertise_state_after_radio_start(&state, true, true) ==
            GapAdvertiseStartReady);
    REQUIRE(state == GapAdvertiseStateActive);
    return true;
}

typedef struct {
    uint32_t starts;
    uint32_t quiesces;
    uint32_t deinits;
    uint32_t glue_stops;
    bool start_succeeds;
    bool quiesce_succeeds;
    bool glue_stop_succeeds;
} RadioStartFixture;

static bool radio_start(void* context) {
    RadioStartFixture* fixture = context;
    fixture->starts++;
    return fixture->start_succeeds;
}

static void radio_deinit(void* context) {
    RadioStartFixture* fixture = context;
    fixture->deinits++;
}

static bool radio_quiesce(void* context) {
    RadioStartFixture* fixture = context;
    fixture->quiesces++;
    return fixture->quiesce_succeeds;
}

static bool radio_stop_glue(void* context) {
    RadioStartFixture* fixture = context;
    fixture->glue_stops++;
    return fixture->glue_stop_succeeds;
}

static bool radio_start_failure_cleanup_is_resumable(void) {
    BtRadioStartState state = bt_radio_start_state_initial();
    RadioStartFixture fixture = {.quiesce_succeeds = true};
    const BtRadioStartOps ops = {
        .context = &fixture,
        .start = radio_start,
        .quiesce = radio_quiesce,
        .deinit = radio_deinit,
        .stop_glue = radio_stop_glue,
    };

    REQUIRE(!bt_radio_start_state_run(&state, &ops));
    REQUIRE(fixture.starts == 1);
    REQUIRE(fixture.quiesces == 1);
    REQUIRE(fixture.deinits == 1);
    REQUIRE(fixture.glue_stops == 1);

    fixture.glue_stop_succeeds = true;
    REQUIRE(!bt_radio_start_state_run(&state, &ops));
    REQUIRE(fixture.starts == 1);
    REQUIRE(fixture.deinits == 1);
    REQUIRE(fixture.glue_stops == 2);
    REQUIRE(bt_radio_start_state_cleanup_complete(&state));
    return true;
}

static bool event_worker_fence_precedes_handler_teardown(void) {
    BleEventTeardownOrder order = ble_event_teardown_order_initial();

    REQUIRE(!ble_event_teardown_order_record_quiesce(&order, false));
    REQUIRE(!ble_event_teardown_order_may_destroy_handlers(&order));
    REQUIRE(ble_event_teardown_order_record_quiesce(&order, true));
    REQUIRE(ble_event_teardown_order_may_destroy_handlers(&order));
    ble_event_teardown_order_record_handler_destruction(&order);
    REQUIRE(ble_event_teardown_order_handlers_destroyed(&order));
    return true;
}

typedef enum {
    GapStateIdle,
    GapStateDisconnecting,
    GapStateConnected,
} TestGapState;

typedef struct {
    TestGapState state;
    bool enable_adv;
    bool force_idle_queued;
    bool advertising_restarted;
} ForceIdleFixture;

static void force_idle_recover(ForceIdleFixture* fixture) {
    if(fixture->state == GapStateDisconnecting) {
        fixture->state = GapStateIdle;
        if(fixture->enable_adv) {
            fixture->advertising_restarted = true;
        }
    }
}

static bool stuck_disconnecting_self_heals_via_force_idle(void) {
    ForceIdleFixture fixture = {
        .state = GapStateDisconnecting,
        .enable_adv = true,
        .force_idle_queued = true,
    };

    REQUIRE(fixture.state == GapStateDisconnecting);
    force_idle_recover(&fixture);
    REQUIRE(fixture.state == GapStateIdle);
    REQUIRE(fixture.advertising_restarted);
    return true;
}

typedef struct {
    bool gap_active;
    bool gap_connected;
    uint32_t desync_since;
    uint32_t now;
    bool kicked;
} WatchdogFixture;

static void watchdog_path_b_step(WatchdogFixture* fixture) {
    if(!fixture->gap_active) {
        fixture->desync_since = 0;
        return;
    }
    if(fixture->desync_since == 0) {
        fixture->desync_since = fixture->now;
        return;
    }
    if(fixture->now - fixture->desync_since < 15) return;
    fixture->kicked = true;
}

static bool watchdog_evicts_on_gap_active_not_connected(void) {
    WatchdogFixture fixture = {
        .gap_active = true,
        .gap_connected = false,
        .now = 0,
    };

    watchdog_path_b_step(&fixture);
    REQUIRE(fixture.desync_since == 0);
    fixture.now = 1;
    watchdog_path_b_step(&fixture);
    REQUIRE(fixture.desync_since == 1);
    REQUIRE(!fixture.kicked);
    fixture.now = 20;
    watchdog_path_b_step(&fixture);
    REQUIRE(fixture.kicked);
    return true;
}

static bool watchdog_does_not_evict_when_gap_idle(void) {
    WatchdogFixture fixture = {
        .gap_active = false,
        .gap_connected = false,
        .now = 0,
    };

    watchdog_path_b_step(&fixture);
    REQUIRE(fixture.desync_since == 0);
    fixture.now = 20;
    watchdog_path_b_step(&fixture);
    REQUIRE(!fixture.kicked);
    return true;
}

int main(void) {
    if(!shci_timeout_propagates_without_stale_response()) return 1;
    if(!callback_detach_deadline_retains_owner()) return 1;
    if(!timer_queue_saturation_unwinds_advertising()) return 1;
    if(!radio_start_failure_cleanup_is_resumable()) return 1;
    if(!event_worker_fence_precedes_handler_teardown()) return 1;
    if(!stuck_disconnecting_self_heals_via_force_idle()) return 1;
    if(!watchdog_evicts_on_gap_active_not_connected()) return 1;
    if(!watchdog_does_not_evict_when_gap_idle()) return 1;
    return 0;
}

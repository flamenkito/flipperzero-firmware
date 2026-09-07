#include <assert.h>
#include <pthread.h>
#include <time.h>

#include "../../applications_user/pocket_airbridge/airbridge_ble.h"
#include "../../applications_user/pocket_airbridge/airbridge_operation.h"
#include <furi_hal_bt.h>

struct Bt {
    unsigned unused;
};

static Bt bt;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint32_t tick;
    GapState state;
    unsigned state_reads;
    unsigned advertising_starts;
    bool waiting;
    bool released;
    bool completed;
} platform = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

typedef struct {
    AirbridgeBle ble;
    AirbridgeOperationMonitor operation;
} Fixture;

static void await_change(void) {
    struct timespec deadline;
    assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_sec += 5;
    assert(pthread_cond_timedwait(&platform.changed, &platform.mutex, &deadline) == 0);
}

uint32_t furi_get_tick(void) {
    pthread_mutex_lock(&platform.mutex);
    const uint32_t tick = platform.tick;
    pthread_mutex_unlock(&platform.mutex);
    return tick;
}

GapState gap_get_state(void) {
    pthread_mutex_lock(&platform.mutex);
    platform.state_reads++;
    const GapState state = platform.state;
    pthread_mutex_unlock(&platform.mutex);
    return state;
}

void furi_delay_ms(uint32_t milliseconds) {
    assert(milliseconds == 25);
    pthread_mutex_lock(&platform.mutex);
    platform.waiting = true;
    pthread_cond_broadcast(&platform.changed);
    while(!platform.released)
        await_change();
    pthread_mutex_unlock(&platform.mutex);
}

bool furi_hal_bt_is_active(void) {
    return gap_get_state() > GapStateIdle;
}

void furi_hal_bt_start_advertising(void) {
    pthread_mutex_lock(&platform.mutex);
    assert(platform.state == GapStateIdle);
    platform.advertising_starts++;
    platform.state = GapStateAdvFast;
    pthread_mutex_unlock(&platform.mutex);
}

/* These paths are linked from the real module but must never run while merely
 * servicing advertising. In particular, a warning must not replace a profile. */
const FuriHalBleProfileTemplate* const ble_profile_airbridge = NULL;

void* furi_record_open(const char* name) {
    UNUSED(name);
    abort();
}

void furi_record_close(const char* name) {
    UNUSED(name);
    abort();
}

FuriHalBleProfileBase* bt_profile_start(
    Bt* instance,
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params) {
    UNUSED(instance);
    UNUSED(profile_template);
    UNUSED(params);
    abort();
}

bool bt_profile_restore_default(Bt* instance) {
    UNUSED(instance);
    abort();
}

void bt_disconnect(Bt* instance) {
    UNUSED(instance);
    abort();
}

FuriHalBleProfileBase* bt_current_profile_acquire(Bt* instance) {
    UNUSED(instance);
    abort();
}

void bt_current_profile_release(Bt* instance) {
    UNUSED(instance);
    abort();
}

bool bt_set_status_changed_callback_bounded(
    Bt* instance,
    BtStatusChangedCallback callback,
    void* context,
    uint32_t timeout) {
    UNUSED(instance);
    UNUSED(callback);
    UNUSED(context);
    UNUSED(timeout);
    abort();
}

BtStatus bt_set_status_changed_callback_with_snapshot(
    Bt* instance,
    BtStatusChangedCallback callback,
    void* context) {
    UNUSED(instance);
    UNUSED(callback);
    UNUSED(context);
    abort();
}

bool airbridge_profile_send(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t len) {
    UNUSED(profile);
    UNUSED(data);
    UNUSED(len);
    abort();
}

bool airbridge_profile_subscribed(FuriHalBleProfileBase* profile) {
    UNUSED(profile);
    abort();
}

static Fixture fixture_init(GapState state, uint32_t tick) {
    platform.tick = tick;
    platform.state = state;
    platform.state_reads = 0;
    platform.advertising_starts = 0;
    platform.waiting = false;
    platform.released = false;
    platform.completed = false;
    return (Fixture){.ble = {.bt = &bt, .ble_profile_installed = true}};
}

static void service_pending(Fixture* fixture) {
    /* Match the runtime bracket used on every screen, including DeployPrompt.
     * No Bridge-only watchdog call or screen state is supplied by this test. */
    airbridge_operation_start(
        &fixture->operation, AirbridgeOperationBleReconnect, furi_get_tick());
    airbridge_ble_service_pending(&fixture->ble);
    airbridge_operation_end(&fixture->operation);
}

static void* service_worker(void* context) {
    service_pending(context);
    pthread_mutex_lock(&platform.mutex);
    platform.completed = true;
    pthread_cond_broadcast(&platform.changed);
    pthread_mutex_unlock(&platform.mutex);
    return NULL;
}

static void test_prompt_blocked_advertising(GapState blocked, GapState completion) {
    const uint32_t started = BLE_BRIDGE_ADV_WATCHDOG_MS;
    Fixture fixture = fixture_init(blocked, started);
    fixture.ble.profile_params.context = &fixture;
    assert(!fixture.ble.restart_adv_pending);
    assert(fixture.ble.published_sequence == fixture.ble.consumed_sequence);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, service_worker, &fixture) == 0);
    pthread_mutex_lock(&platform.mutex);
    while(!platform.waiting && !platform.completed)
        await_change();
    assert(platform.waiting && !platform.completed);
    assert(platform.advertising_starts == 0);
    assert(fixture.ble.ble_bridge_last_watchdog_tick == started);

    /* The supervisor observes the real BLE call while its worker is blocked.
     * A Deploy confirmation prompt is not a Bluetooth pairing ceremony. */
    platform.tick = started + AirbridgeOperationBleBudgetMs - 1;
    AirbridgeOperationStatus status =
        airbridge_operation_observe(&fixture.operation, platform.tick, false);
    assert(status.operation == AirbridgeOperationBleReconnect && !status.stalled);
    platform.tick++;
    status = airbridge_operation_observe(&fixture.operation, platform.tick, false);
    assert(status.operation == AirbridgeOperationBleReconnect && status.stalled);
    assert(!platform.completed);
    assert(fixture.ble.bt == &bt && fixture.ble.ble_profile_installed);
    assert(fixture.ble.profile_params.context == &fixture);

    platform.tick += AirbridgeOperationBleBudgetMs;
    platform.state = completion;
    platform.released = true;
    pthread_cond_broadcast(&platform.changed);
    while(!platform.completed)
        await_change();
    pthread_mutex_unlock(&platform.mutex);
    assert(pthread_join(worker, NULL) == 0);
    assert(platform.state_reads >= 3);
    assert(platform.advertising_starts == 0);
    assert(fixture.ble.ble_profile_installed);
    assert(fixture.ble.profile_params.context == &fixture);
    assert(
        __atomic_load_n(&fixture.operation.operation, __ATOMIC_RELAXED) == AirbridgeOperationIdle);
    assert(airbridge_operation_observe(&fixture.operation, platform.tick, false).stalled);
}

static void test_idle_and_watchdog_throttle(void) {
    Fixture fixture = fixture_init(GapStateAdvFast, BLE_BRIDGE_ADV_WATCHDOG_MS - 1);
    service_pending(&fixture);
    assert(platform.state_reads == 0);
    platform.tick++;
    service_pending(&fixture);
    assert(platform.state_reads > 0);
    assert(platform.advertising_starts == 0);
    const unsigned reads = platform.state_reads;
    platform.tick++;
    service_pending(&fixture);
    assert(platform.state_reads == reads);

    const GapState healthy[] = {
        GapStateAdvFast, GapStateAdvLowPower, GapStateConnected, GapStateIdle};
    for(size_t i = 0; i < COUNT_OF(healthy); i++) {
        platform.state = healthy[i];
        platform.tick += 60000;
        service_pending(&fixture);
        AirbridgeOperationStatus status =
            airbridge_operation_observe(&fixture.operation, platform.tick, false);
        assert(status.operation == AirbridgeOperationIdle && !status.stalled);
        assert(!platform.waiting);
    }
    assert(platform.advertising_starts == 1);
    /* Waiting for a browser or prompt input between operations is not a stall. */
    assert(
        !airbridge_operation_observe(&fixture.operation, platform.tick + 600000, false).stalled);
}

static void test_disconnect_restart_before_watchdog_interval(void) {
    Fixture fixture = fixture_init(GapStateIdle, 1);
    fixture.ble.ble_connected = true;
    fixture.ble.published_sequence = 1;
    fixture.ble.published_connected = false;
    service_pending(&fixture);
    assert(fixture.ble.consumed_sequence == 1);
    assert(!fixture.ble.ble_connected && !fixture.ble.restart_adv_pending);
    assert(fixture.ble.ble_bridge_last_watchdog_tick == 0);
    assert(platform.advertising_starts == 1);
    service_pending(&fixture);
    assert(platform.advertising_starts == 1);
}

static void test_uninstalled_noop(void) {
    Fixture fixture = fixture_init(GapStateStartingAdv, BLE_BRIDGE_ADV_WATCHDOG_MS);
    fixture.ble.ble_profile_installed = false;
    service_pending(&fixture);
    assert(platform.state_reads == 0);
    assert(platform.advertising_starts == 0);
    assert(!platform.waiting);
}

int main(void) {
    test_prompt_blocked_advertising(GapStateStartingAdv, GapStateAdvLowPower);
    /* gap_get_state reports Disconnecting while the GAP state mutex is busy. */
    test_prompt_blocked_advertising(GapStateDisconnecting, GapStateConnected);
    test_idle_and_watchdog_throttle();
    test_disconnect_restart_before_watchdog_interval();
    test_uninstalled_noop();
    pthread_cond_destroy(&platform.changed);
    pthread_mutex_destroy(&platform.mutex);
    return 0;
}

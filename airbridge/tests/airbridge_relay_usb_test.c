#include "../../applications_user/pocket_airbridge/airbridge_lifecycle.h"
#include "../../applications_user/pocket_airbridge/airbridge_relay.h"

#ifndef AIRBRIDGE_SAFE_TEARDOWN_ENABLED
#define AIRBRIDGE_SAFE_TEARDOWN_ENABLED 1
#endif

extern FuriHalUsbInterface usb_cdc_single;
extern FuriHalUsbInterface relay_test_spoof_logitech;
extern FuriHalUsbInterface relay_test_spoof_dell;
extern FuriHalUsbInterface relay_test_fap_profile;
extern FuriHalUsbInterface* relay_test_current;
extern FuriHalUsbInterface* relay_test_begin_current;
extern FuriHalUsbInterface* relay_test_set_targets[32];
extern unsigned relay_test_set_calls;
extern unsigned relay_test_unlock_calls;
extern unsigned relay_test_lock_calls;
extern unsigned relay_test_delay_calls;
extern uint32_t relay_test_last_delay;
extern bool relay_test_locked;
extern bool relay_test_begin_result;
extern bool relay_test_end_resume;
extern unsigned relay_test_begin_calls;
extern unsigned relay_test_end_calls;
extern char relay_test_call_log[256];

void relay_test_reset(void);
void relay_test_script_set(bool result);

typedef struct {
    AirbridgeRelay relay;
    AirbridgeOperationMonitor monitor;
    AirbridgeExitContract contract;
    unsigned waits;
} LifecycleFixture;

static void fixture_init(AirbridgeRelay* relay) {
    memset(relay, 0, sizeof(*relay));
    airbridge_relay_init(relay);
}

static void show_closing(void* context) {
    UNUSED(context);
}

static bool restore_usb(void* context) {
    return airbridge_relay_restore_usb(&((LifecycleFixture*)context)->relay);
}

static bool restore_ble(void* context) {
    UNUSED(context);
    return true;
}

static uint32_t now(void* context) {
    UNUSED(context);
    return 100;
}

static void wait_for_retry(void* context) {
    LifecycleFixture* fixture = context;
    fixture->waits++;
    assert(!airbridge_exit_contract_may_destroy(&fixture->contract));
}

static void lifecycle_close(LifecycleFixture* fixture) {
    const AirbridgeLifecycleOps operations = {
        .context = fixture,
        .show_closing = show_closing,
        .restore_usb = restore_usb,
        .restore_ble = restore_ble,
        .now = now,
        .wait = wait_for_retry,
    };
    fixture->contract = airbridge_exit_contract_initial();
    airbridge_lifecycle_close(&operations, &fixture->monitor, &fixture->contract);
}

static bool configure(AirbridgeRelay* relay) {
    uint8_t selected = 0xFF;
    const bool configured = airbridge_relay_configure_usb(relay, 0, &selected);
    if(configured) assert(selected == 0);
    return configured;
}

#if AIRBRIDGE_SAFE_TEARDOWN_ENABLED
static void test_teardown_success_and_takeover_order(void) {
    relay_test_reset();
    relay_test_begin_result = true;
    relay_test_script_set(true);
    relay_test_script_set(true);
    relay_test_script_set(true);
    relay_test_script_set(true);
    AirbridgeRelay relay;
    fixture_init(&relay);
    assert(configure(&relay));
    assert(relay.usb_mode_prev == &relay_test_spoof_logitech);
    assert(relay.usb_configured && !relay.usb_transition_dirty);
    assert(relay_test_current == &relay_test_fap_profile);
    assert(relay_test_delay_calls == 1 && relay_test_last_delay == 500);
    assert(strchr(relay_test_call_log, 'B') < strchr(relay_test_call_log, 'S'));
    assert(relay_test_lock_calls == 1 && relay_test_locked);
    assert(airbridge_relay_restore_usb(&relay));
    assert(relay_test_current == &relay_test_spoof_logitech);
    assert(!relay.usb_configured && !relay.usb_transition_dirty);
    assert(relay_test_unlock_calls == 1 && relay_test_end_calls == 1);
    assert(relay_test_end_resume);
    assert(strrchr(relay_test_call_log, 'S') < strchr(relay_test_call_log, 'E'));
    airbridge_relay_deinit(&relay);
}

static void test_entry_install_failure_rolls_back(void) {
    relay_test_reset();
    relay_test_script_set(true);
    relay_test_script_set(false);
    relay_test_script_set(true);
    AirbridgeRelay relay;
    fixture_init(&relay);
    assert(!configure(&relay));
    assert(relay_test_current == &relay_test_spoof_logitech);
    assert(!relay.usb_configured && !relay.usb_transition_dirty);
    assert(relay_test_set_calls == 3 && relay_test_end_calls == 1);
    assert(!relay_test_end_resume);
    airbridge_relay_deinit(&relay);
}

static void test_teardown_failure_does_not_mutate_source(void) {
    relay_test_reset();
    relay_test_script_set(false);
    AirbridgeRelay relay;
    fixture_init(&relay);
    assert(!configure(&relay));
    assert(relay_test_current == &relay_test_spoof_logitech);
    assert(relay_test_set_calls == 1);
    assert(!relay.usb_configured && !relay.usb_transition_dirty);
    airbridge_relay_deinit(&relay);
}

static void test_rollback_failure_lifecycle_retry(void) {
    relay_test_reset();
    relay_test_script_set(true);
    relay_test_script_set(false);
    relay_test_script_set(false);
    relay_test_script_set(false);
    relay_test_script_set(true);
    LifecycleFixture fixture = {0};
    fixture_init(&fixture.relay);
    assert(!configure(&fixture.relay));
    assert(relay_test_current == NULL);
    assert(!fixture.relay.usb_configured && fixture.relay.usb_transition_dirty);
    lifecycle_close(&fixture);
    assert(fixture.waits == 1);
    assert(relay_test_current == &relay_test_spoof_logitech);
    assert(airbridge_exit_contract_may_destroy(&fixture.contract));
    airbridge_relay_deinit(&fixture.relay);
}

static void test_exit_rollback_keeps_restoration_mandatory(void) {
    relay_test_reset();
    relay_test_current = &relay_test_fap_profile;
    relay_test_locked = true;
    relay_test_script_set(true);
    relay_test_script_set(false);
    relay_test_script_set(true);
    relay_test_script_set(true);
    relay_test_script_set(true);
    LifecycleFixture fixture = {0};
    fixture_init(&fixture.relay);
    fixture.relay.usb_mode_prev = &relay_test_spoof_logitech;
    fixture.relay.usb_mode_owned = &relay_test_fap_profile;
    fixture.relay.usb_configured = true;
    fixture.relay.usb_lock_held = true;
    fixture.relay.usb_takeover_active = true;
    assert(!airbridge_relay_restore_usb(&fixture.relay));
    assert(relay_test_current == &relay_test_fap_profile);
    assert(fixture.relay.usb_configured && !fixture.relay.usb_transition_dirty);
    lifecycle_close(&fixture);
    assert(relay_test_current == &relay_test_spoof_logitech);
    assert(airbridge_exit_contract_may_destroy(&fixture.contract));
    assert(relay_test_end_calls == 1);
    airbridge_relay_deinit(&fixture.relay);
}

static void test_startup_failure_restores_through_lifecycle(void) {
    relay_test_reset();
    relay_test_script_set(true);
    relay_test_script_set(false);
    relay_test_script_set(false);
    relay_test_script_set(true);
    LifecycleFixture fixture = {0};
    fixture_init(&fixture.relay);
    assert(!configure(&fixture.relay));
    assert(fixture.relay.usb_transition_dirty);
    lifecycle_close(&fixture);
    assert(relay_test_current == &relay_test_spoof_logitech);
    assert(airbridge_exit_contract_may_destroy(&fixture.contract));
    airbridge_relay_deinit(&fixture.relay);
}

static void test_recovery_rows(void) {
    relay_test_reset();
    relay_test_current = NULL;
    relay_test_script_set(true);
    AirbridgeRelay relay;
    fixture_init(&relay);
    relay.usb_mode_prev = &usb_cdc_single;
    relay.usb_transition_dirty = true;
    assert(airbridge_relay_restore_usb(&relay));
    assert(relay_test_current == &usb_cdc_single && relay_test_set_calls == 1);
    airbridge_relay_deinit(&relay);

    relay_test_reset();
    relay_test_current = NULL;
    relay_test_script_set(true);
    fixture_init(&relay);
    relay.usb_mode_prev = NULL;
    relay.usb_transition_dirty = true;
    assert(airbridge_relay_restore_usb(&relay));
    assert(relay_test_current == &relay_test_spoof_logitech && relay_test_set_calls == 1);
    airbridge_relay_deinit(&relay);

    relay_test_reset();
    relay_test_current = &usb_cdc_single;
    fixture_init(&relay);
    relay.usb_mode_prev = &usb_cdc_single;
    relay.usb_transition_dirty = true;
    assert(airbridge_relay_restore_usb(&relay));
    assert(relay_test_set_calls == 0 && !relay.usb_transition_dirty);
    airbridge_relay_deinit(&relay);
}

static void test_entry_capture_is_post_takeover(void) {
    relay_test_reset();
    relay_test_current = &relay_test_spoof_dell;
    relay_test_begin_current = &usb_cdc_single;
    relay_test_begin_result = true;
    relay_test_script_set(true);
    relay_test_script_set(true);
    AirbridgeRelay relay;
    fixture_init(&relay);
    assert(configure(&relay));
    assert(relay.usb_mode_prev == &usb_cdc_single);
    assert(relay_test_set_targets[0] == &relay_test_fap_profile);
    assert(airbridge_relay_restore_usb(&relay));
    assert(relay_test_current == &usb_cdc_single);
    assert(relay_test_end_resume);
    airbridge_relay_deinit(&relay);
}

static void test_locked_entry_force_unlocks_once(void) {
    relay_test_reset();
    relay_test_current = &usb_cdc_single;
    relay_test_locked = true;
    relay_test_script_set(true);
    relay_test_script_set(true);
    AirbridgeRelay relay;
    fixture_init(&relay);
    assert(configure(&relay));
    assert(relay_test_unlock_calls == 1);
    assert(airbridge_relay_restore_usb(&relay));
    assert(relay_test_unlock_calls == 2);
    assert(relay_test_lock_calls == 1);
    airbridge_relay_deinit(&relay);
}
#else
static void assert_admission_rejected(FuriHalUsbInterface* source) {
    relay_test_reset();
    relay_test_current = source;
    AirbridgeRelay relay;
    fixture_init(&relay);
    assert(!configure(&relay));
    assert(relay.admission_rejected);
    assert(relay.usb_mode_prev == source);
    assert(relay_test_set_calls == 0);
    assert(relay_test_begin_calls == 1 && relay_test_end_calls == 1);
    airbridge_relay_deinit(&relay);
}

static void test_flag_off_admission(void) {
    assert_admission_rejected(&relay_test_spoof_logitech);
    assert_admission_rejected(NULL);
}

static void test_flag_off_cdc_paths(void) {
    relay_test_reset();
    relay_test_current = &usb_cdc_single;
    relay_test_begin_result = true;
    relay_test_script_set(true);
    relay_test_script_set(true);
    LifecycleFixture fixture = {0};
    fixture_init(&fixture.relay);
    assert(configure(&fixture.relay));
    assert(relay_test_current == &relay_test_fap_profile);
    lifecycle_close(&fixture);
    assert(relay_test_current == &usb_cdc_single);
    assert(relay_test_set_calls == 2);
    assert(relay_test_end_resume);
    assert(airbridge_exit_contract_may_destroy(&fixture.contract));
    airbridge_relay_deinit(&fixture.relay);
}
#endif

static void test_deploy_classification_is_direction_aware(void) {
    AirbridgeRelay relay;
    fixture_init(&relay);
    BridgeEvent event;
    memset(&event, 0, sizeof(event));
    event.len = 1;
    event.data[0] = 0x42;

    /* USB-origin 0x42 while Waiting armed USB → USB deploy. */
    event.to_ble = true;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenWaiting, AirbridgeTypingTransportUsb) ==
        AirbridgeRelayDeployRequestedUsb);

    /* BLE-origin 0x42 while Waiting armed BLE → BLE deploy. */
    event.to_ble = false;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenWaiting, AirbridgeTypingTransportBle) ==
        AirbridgeRelayDeployRequestedBle);

    /* Cross-direction 0x42 is bridge data, not a deploy: relayed normally. */
    event.to_ble = true;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenWaiting, AirbridgeTypingTransportBle) ==
        AirbridgeRelayHandled);
    event.to_ble = false;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenWaiting, AirbridgeTypingTransportUsb) ==
        AirbridgeRelayHandled);

    const AirbridgeScreen utility_screens[] = {
        AirbridgeScreenSettings, AirbridgeScreenPasswords, AirbridgeScreenPasswordTyping};
    for(size_t i = 0; i < COUNT_OF(utility_screens); i++) {
        event.to_ble = true;
        assert(
            airbridge_relay_handle(
                &relay, NULL, &event, utility_screens[i], AirbridgeTypingTransportUsb) ==
            AirbridgeRelayHandled);
        event.to_ble = false;
        assert(
            airbridge_relay_handle(
                &relay, NULL, &event, utility_screens[i], AirbridgeTypingTransportBle) ==
            AirbridgeRelayHandled);
    }

    /* Deploy screens outside Waiting still reject an unarmed request. */
    event.to_ble = true;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenStreaming, AirbridgeTypingTransportUsb) ==
        AirbridgeRelayDeployNotArmed);
    event.to_ble = false;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenTyping, AirbridgeTypingTransportBle) ==
        AirbridgeRelayDeployNotArmed);

    /* 0x42 on the Bridge screen and non-0x42 in Waiting relay normally. */
    event.to_ble = true;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenBridge, AirbridgeTypingTransportUsb) ==
        AirbridgeRelayHandled);
    event.data[0] = 0x01;
    assert(
        airbridge_relay_handle(
            &relay, NULL, &event, AirbridgeScreenWaiting, AirbridgeTypingTransportUsb) ==
        AirbridgeRelayHandled);

    airbridge_relay_deinit(&relay);
}

int main(void) {
    test_deploy_classification_is_direction_aware();
#if AIRBRIDGE_SAFE_TEARDOWN_ENABLED
    test_teardown_success_and_takeover_order();
    test_entry_install_failure_rolls_back();
    test_teardown_failure_does_not_mutate_source();
    test_rollback_failure_lifecycle_retry();
    test_exit_rollback_keeps_restoration_mandatory();
    test_startup_failure_restores_through_lifecycle();
    test_recovery_rows();
    test_entry_capture_is_post_takeover();
    test_locked_entry_force_unlocks_once();
#else
    test_flag_off_admission();
    test_flag_off_cdc_paths();
#endif
    return 0;
}

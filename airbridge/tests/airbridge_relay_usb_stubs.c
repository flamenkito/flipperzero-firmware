#include "../../applications_user/pocket_airbridge/airbridge_relay.h"
#include "../../applications_user/pocket_airbridge/airbridge_usb.h"

#include <cli/cli_vcp.h>
#include <furi_hal_usb_spoof.h>

struct CliVcp {
    bool fixture;
};

FuriHalUsbInterface usb_cdc_single;
FuriHalUsbInterface usb_cdc_dual;
FuriHalUsbInterface relay_test_spoof_logitech;
FuriHalUsbInterface relay_test_spoof_dell;
FuriHalUsbInterface relay_test_fap_profile;

FuriHalUsbInterface* relay_test_current;
FuriHalUsbInterface* relay_test_begin_current;
FuriHalUsbInterface* relay_test_set_targets[32];
bool relay_test_set_results[32];
unsigned relay_test_set_result_count;
unsigned relay_test_set_result_index;
unsigned relay_test_set_calls;
unsigned relay_test_unlock_calls;
unsigned relay_test_lock_calls;
unsigned relay_test_delay_calls;
uint32_t relay_test_last_delay;
bool relay_test_locked;
bool relay_test_begin_result;
bool relay_test_end_resume;
unsigned relay_test_begin_calls;
unsigned relay_test_end_calls;
char relay_test_call_log[256];
size_t relay_test_call_log_length;

static struct CliVcp cli_vcp_fixture;

static void log_call(char call) {
    assert(relay_test_call_log_length + 1 < sizeof(relay_test_call_log));
    relay_test_call_log[relay_test_call_log_length++] = call;
    relay_test_call_log[relay_test_call_log_length] = '\0';
}

void relay_test_reset(void) {
    relay_test_current = &relay_test_spoof_logitech;
    relay_test_begin_current = NULL;
    memset(relay_test_set_targets, 0, sizeof(relay_test_set_targets));
    memset(relay_test_set_results, 0, sizeof(relay_test_set_results));
    relay_test_set_result_count = 0;
    relay_test_set_result_index = 0;
    relay_test_set_calls = 0;
    relay_test_unlock_calls = 0;
    relay_test_lock_calls = 0;
    relay_test_delay_calls = 0;
    relay_test_last_delay = 0;
    relay_test_locked = false;
    relay_test_begin_result = false;
    relay_test_end_resume = false;
    relay_test_begin_calls = 0;
    relay_test_end_calls = 0;
    relay_test_call_log_length = 0;
    relay_test_call_log[0] = '\0';
}

void relay_test_script_set(bool result) {
    assert(relay_test_set_result_count < COUNT_OF(relay_test_set_results));
    relay_test_set_results[relay_test_set_result_count++] = result;
}

void* furi_record_open(const char* name) {
    assert(strcmp(name, RECORD_CLI_VCP) == 0);
    UNUSED(name);
    return &cli_vcp_fixture;
}

void furi_record_close(const char* name) {
    assert(strcmp(name, RECORD_CLI_VCP) == 0);
    UNUSED(name);
}

uint32_t furi_get_tick(void) {
    static uint32_t tick;
    return ++tick;
}

void furi_delay_ms(uint32_t milliseconds) {
    relay_test_delay_calls++;
    relay_test_last_delay = milliseconds;
    log_call('D');
}

FuriHalUsbInterface* furi_hal_usb_get_config(void) {
    log_call('G');
    return relay_test_current;
}

bool furi_hal_usb_set_config(FuriHalUsbInterface* target, void* context) {
    UNUSED(context);
    log_call('S');
    assert(relay_test_set_calls < COUNT_OF(relay_test_set_targets));
    relay_test_set_targets[relay_test_set_calls++] = target;
    assert(relay_test_set_result_index < relay_test_set_result_count);
    const bool result = relay_test_set_results[relay_test_set_result_index++];
    if(result) relay_test_current = target;
    return result;
}

void furi_hal_usb_lock(void) {
    log_call('L');
    relay_test_lock_calls++;
    relay_test_locked = true;
}

void furi_hal_usb_unlock(void) {
    log_call('U');
    relay_test_unlock_calls++;
    relay_test_locked = false;
}

bool furi_hal_usb_is_locked(void) {
    log_call('I');
    return relay_test_locked;
}

FuriHalUsbInterface* airbridge_usb_get_profile(uint8_t index) {
    return index == 0 ? &relay_test_fap_profile : NULL;
}

void airbridge_usb_vendor_set_callback(HidVendorCallback callback, void* context) {
    UNUSED(callback);
    UNUSED(context);
    log_call('C');
}

uint32_t airbridge_usb_vendor_get_request(uint8_t* data) {
    UNUSED(data);
    return 0;
}

bool airbridge_usb_vendor_send_response_blocking(uint8_t* data, uint8_t length, uint32_t timeout) {
    assert(timeout > 0 && timeout <= 10);
    UNUSED(timeout);
    UNUSED(data);
    UNUSED(length);
    return true;
}

bool airbridge_ble_send(AirbridgeBle* ble, uint8_t* data, uint16_t length) {
    UNUSED(ble);
    UNUSED(data);
    UNUSED(length);
    return true;
}

FuriHalUsbInterface* furi_hal_usb_spoof_get_active_interface(void) {
    return &relay_test_spoof_logitech;
}

FuriHalUsbInterface* furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfile profile) {
    return profile == FuriHalUsbSpoofProfileDell ? &relay_test_spoof_dell :
                                                  &relay_test_spoof_logitech;
}

bool cli_vcp_usb_takeover_begin(CliVcp* cli_vcp) {
    assert(cli_vcp == &cli_vcp_fixture);
    UNUSED(cli_vcp);
    log_call('B');
    relay_test_begin_calls++;
    if(relay_test_begin_current != NULL) relay_test_current = relay_test_begin_current;
    return relay_test_begin_result;
}

void cli_vcp_usb_takeover_end(CliVcp* cli_vcp, bool resume) {
    assert(cli_vcp == &cli_vcp_fixture);
    UNUSED(cli_vcp);
    log_call('E');
    relay_test_end_calls++;
    relay_test_end_resume = resume;
}

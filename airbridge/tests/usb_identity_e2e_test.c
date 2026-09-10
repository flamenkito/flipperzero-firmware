#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if(!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                              \
        }                                                                         \
    } while(false)

#include "cli_vcp_model.h"

#include "../../applications_user/pocket_airbridge/airbridge_relay.h"
#include "../../applications_user/pocket_airbridge/airbridge_usb.h"

#include <furi_hal_rtc.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_usb_spoof.h>

struct CliVcp {
    const CliVcpInternalApi* api;
};

static Model model;
static uint32_t rtc_usb_identity;
static HidVendorCallback vendor_callback;
static void* vendor_callback_context;

static const struct usb_device_descriptor cdc_device_desc = {
    .idVendor = 0x0483,
    .idProduct = 0x5740,
};
static const struct usb_device_descriptor fap_logitech_device_desc = {
    .idVendor = 0x046D,
    .idProduct = 0xC31C,
};
static const struct usb_device_descriptor fap_dell_device_desc = {
    .idVendor = 0x413C,
    .idProduct = 0x2113,
};

FuriHalUsbInterface usb_cdc_single = {.dev_descr = (void*)&cdc_device_desc};
FuriHalUsbInterface usb_cdc_dual = {.dev_descr = (void*)&cdc_device_desc};
static FuriHalUsbInterface fap_logitech = {.dev_descr = (void*)&fap_logitech_device_desc};
static FuriHalUsbInterface fap_dell = {.dev_descr = (void*)&fap_dell_device_desc};

static UsbInterface interface_kind(FuriHalUsbInterface* interface) {
    if(interface == NULL) return UsbNull;
    if(interface == &usb_cdc_single) return UsbCdcSingle;
    if(interface == &usb_cdc_dual) return UsbCdcDual;
    if(interface == furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileLogitech)) {
        return UsbSpoofLatched;
    }
    if(interface == furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileDell)) {
        return UsbSpoofDell;
    }
    if(interface == &fap_logitech) return UsbAppComposite;
    if(interface == &fap_dell) return UsbAppDell;
    REQUIRE(false);
    return UsbNull;
}

static FuriHalUsbInterface* kind_interface(UsbInterface interface) {
    switch(interface) {
    case UsbNull:
        return NULL;
    case UsbCdcSingle:
        return &usb_cdc_single;
    case UsbCdcDual:
        return &usb_cdc_dual;
    case UsbSpoofLatched:
        return furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileLogitech);
    case UsbSpoofDell:
        return furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileDell);
    case UsbAppComposite:
        return &fap_logitech;
    case UsbAppDell:
        return &fap_dell;
    case UsbSpoofMutablePreference:
        break;
    }
    REQUIRE(false);
    return NULL;
}

FuriHalUsbIdentity furi_hal_rtc_get_usb_identity(void) {
    FuriHalUsbIdentity identity = (FuriHalUsbIdentity)rtc_usb_identity;
    /* Mirrors targets/f7/furi_hal/furi_hal_rtc.c:274-281: invalid [0,1]
     * system-register values clamp to the Logitech boot identity. */
    if(identity >= FuriHalUsbIdentityCount) identity = FuriHalUsbIdentityLogitech;
    return identity;
}

FuriHalUsbInterface* furi_hal_usb_get_config(void) {
    return kind_interface(model.current);
}

bool furi_hal_usb_set_config(FuriHalUsbInterface* target, void* context) {
    UNUSED(context);
    return model_set_config(&model, interface_kind(target));
}

void furi_hal_usb_lock(void) {
    model.lock_calls++;
    model.locked = true;
}

void furi_hal_usb_unlock(void) {
    model.unlock_calls++;
    model.locked = false;
}

bool furi_hal_usb_is_locked(void) {
    return model.locked;
}

uint32_t furi_get_tick(void) {
    return ++model.tick;
}

void furi_delay_ms(uint32_t milliseconds) {
    model.delay_calls++;
    model.last_delay = milliseconds;
}

static Model* cli_model(CliVcp* cli_vcp);

static bool cli_set_override(CliVcp* cli_vcp, bool enabled) {
    Model* shared = cli_model(cli_vcp);
    return enabled ? model_enable(shared, true) : model_disable(shared);
}

static bool cli_get_override(CliVcp* cli_vcp) {
    return cli_model(cli_vcp)->user_override;
}

static bool cli_is_enabled(CliVcp* cli_vcp) {
    return cli_model(cli_vcp)->is_enabled;
}

static void cli_disable_for_lock(CliVcp* cli_vcp) {
    model_disable_for_lock(cli_model(cli_vcp));
}

static void cli_retry_lock_convergence(CliVcp* cli_vcp) {
    model_retry_lock_convergence(cli_model(cli_vcp));
}

static bool cli_try_install_boot_identity(CliVcp* cli_vcp, bool only_if_currently_null) {
    return model_try_install_boot_identity(cli_model(cli_vcp), only_if_currently_null);
}

static const CliVcpInternalApi cli_api = {
    .set_override = cli_set_override,
    .get_override = cli_get_override,
    .is_enabled = cli_is_enabled,
    .disable_for_lock = cli_disable_for_lock,
    .retry_lock_convergence = cli_retry_lock_convergence,
    .try_install_boot_identity = cli_try_install_boot_identity,
};
static struct CliVcp cli_fixture = {.api = &cli_api};

static Model* cli_model(CliVcp* cli_vcp) {
    REQUIRE(cli_vcp == &cli_fixture);
    return &model;
}

void cli_vcp_enable(CliVcp* cli_vcp) {
    REQUIRE(model_enable(cli_model(cli_vcp), false));
}

void cli_vcp_disable(CliVcp* cli_vcp) {
    REQUIRE(model_disable(cli_model(cli_vcp)));
}

bool cli_vcp_usb_takeover_begin(CliVcp* cli_vcp) {
    Model* shared = cli_model(cli_vcp);
    shared->takeover_begin_calls++;
    return model_takeover_begin(shared);
}

void cli_vcp_usb_takeover_end(CliVcp* cli_vcp, bool resume) {
    Model* shared = cli_model(cli_vcp);
    shared->takeover_end_calls++;
    shared->takeover_end_resume = resume;
    model_takeover_end(shared, resume);
}

void* furi_record_open(const char* name) {
    REQUIRE(strcmp(name, RECORD_CLI_VCP) == 0);
    return &cli_fixture;
}

void furi_record_close(const char* name) {
    REQUIRE(strcmp(name, RECORD_CLI_VCP) == 0);
}

FuriHalUsbInterface* airbridge_usb_get_profile(uint8_t index) {
    if(index == 0) return &fap_logitech;
    if(index == 1) return &fap_dell;
    return NULL;
}

void airbridge_usb_vendor_set_callback(HidVendorCallback callback, void* context) {
    vendor_callback = callback;
    vendor_callback_context = context;
    model.vendor_callback_calls++;
}

uint32_t airbridge_usb_vendor_get_request(uint8_t* data) {
    UNUSED(data);
    return 0;
}

bool airbridge_usb_vendor_send_response(uint8_t* data, uint8_t length) {
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

static UsbInterface spoof_kind(FuriHalUsbIdentity identity) {
    return identity == FuriHalUsbIdentityDell ? UsbSpoofDell : UsbSpoofLatched;
}

static void simulated_boot(uint32_t stored_identity) {
    rtc_usb_identity = stored_identity;
    const FuriHalUsbIdentity identity = furi_hal_rtc_get_usb_identity();
    model = model_make();
    model.active_spoof = spoof_kind(identity);
    model.previous_interface = model.active_spoof;
    model.current = UsbNull;
    furi_hal_usb_spoof_latch_active((FuriHalUsbSpoofProfile)identity);
    vendor_callback = NULL;
    vendor_callback_context = NULL;
}

static void install_boot_identity(void) {
    REQUIRE(cli_vcp_try_install_boot_identity(&cli_fixture, false));
    REQUIRE(model.current == model.active_spoof);
    REQUIRE(furi_hal_usb_get_config() == furi_hal_usb_spoof_get_active_interface());
}

static void assert_device_identity(uint16_t vendor, uint16_t product) {
    FuriHalUsbInterface* current = furi_hal_usb_get_config();
    REQUIRE(current != NULL && current->dev_descr != NULL);
    REQUIRE(current->dev_descr->idVendor == vendor);
    REQUIRE(current->dev_descr->idProduct == product);
}

static void relay_init(AirbridgeRelay* relay) {
    memset(relay, 0, sizeof(*relay));
    airbridge_relay_init(relay);
}

static void relay_configure(AirbridgeRelay* relay, uint8_t profile) {
    uint8_t selected = UINT8_MAX;
    REQUIRE(airbridge_relay_configure_usb(relay, profile, &selected));
    REQUIRE(selected == profile);
    REQUIRE(relay->usb_configured && relay->usb_takeover_active && relay->usb_lock_held);
    REQUIRE(model.locked && model.usb_takeover_active);
}

/* MANDATORY CORE S1+S2: DEFAULT-HID-UNTIL-EXPLICIT-SWITCH. */
static void scenario_s1_s2_default_hid_until_explicit_switch(void) {
    /* S1: cold boot clamps an invalid/default RTC value to Logitech, latches,
     * installs the production spoof identity, and does not expose STM32 CDC. */
    simulated_boot(UINT32_MAX);
    REQUIRE(model.active_spoof == UsbSpoofLatched);
    install_boot_identity();
    assert_device_identity(0x046D, 0xC31C);
    REQUIRE(furi_hal_usb_get_config() != &usb_cdc_single);

    /* USB Identity is next-boot-only. */
    rtc_usb_identity = FuriHalUsbIdentityDell;
    REQUIRE(model.current == UsbSpoofLatched);
    assert_device_identity(0x046D, 0xC31C);

    /* Simulated reboot resets HAL/model state and latches Dell. */
    simulated_boot(FuriHalUsbIdentityDell);
    install_boot_identity();
    REQUIRE(model.active_spoof == UsbSpoofDell);
    assert_device_identity(0x413C, 0x2113);

    /* S2: explicit Flipper USB exposes 0483 CDC; OFF normalizes the observed
     * CDC candidate back to the ACTIVE-LATCHED Dell spoof identity. */
    REQUIRE(cli_vcp_set_override(&cli_fixture, true));
    REQUIRE(cli_vcp_get_override(&cli_fixture));
    REQUIRE(cli_vcp_is_enabled(&cli_fixture));
    REQUIRE(model.current == UsbCdcSingle);
    REQUIRE(model.previous_interface == UsbSpoofDell);
    assert_device_identity(0x0483, 0x5740);
    REQUIRE(cli_vcp_set_override(&cli_fixture, false));
    REQUIRE(!cli_vcp_get_override(&cli_fixture));
    REQUIRE(!cli_vcp_is_enabled(&cli_fixture));
    REQUIRE(model.current == UsbSpoofDell);
    assert_device_identity(0x413C, 0x2113);
}

/* S3: CLI takeover is the real production relay call routed into the mirror. */
static void scenario_s3_override_relay_takeover_resume(void) {
    simulated_boot(FuriHalUsbIdentityLogitech);
    install_boot_identity();
    REQUIRE(cli_vcp_set_override(&cli_fixture, true));
    REQUIRE(model.current == UsbCdcSingle && model.callbacks_registered);
    model.is_currently_transmitting = true;
    model.previous_tx_length = 64;

    AirbridgeRelay relay;
    relay_init(&relay);
    relay_configure(&relay, 0);
    REQUIRE(model.current == UsbAppComposite);
    REQUIRE(model.takeover_begin_calls == 1);
    REQUIRE(!model.is_enabled && !model.callbacks_registered);
    REQUIRE(!model.is_currently_transmitting && model.previous_tx_length == 0);
    REQUIRE(relay.usb_mode_prev == &usb_cdc_single && relay.cli_was_enabled);

    REQUIRE(airbridge_relay_restore_usb(&relay));
    REQUIRE(model.current == UsbCdcSingle);
    REQUIRE(model.takeover_end_calls == 1 && model.takeover_end_resume);
    REQUIRE(model.is_enabled && model.callbacks_registered && !model.locked);
    REQUIRE(!relay.usb_configured && !relay.usb_takeover_active && !relay.usb_lock_held);
    airbridge_relay_deinit(&relay);
}

static void cross_identity_exit(
    FuriHalUsbIdentity boot_identity,
    uint8_t fap_profile,
    UsbInterface fap_kind,
    uint16_t restored_vendor,
    uint16_t restored_product) {
    simulated_boot(boot_identity);
    install_boot_identity();
    const UsbInterface boot_kind = model.active_spoof;

    AirbridgeRelay relay;
    relay_init(&relay);
    relay_configure(&relay, fap_profile);
    REQUIRE(model.current == fap_kind);
    REQUIRE(relay.usb_mode_prev == kind_interface(boot_kind));
    REQUIRE(model.delay_calls == 1 && model.last_delay == 500);
    REQUIRE(model.direct_composite_transitions == 0);

    REQUIRE(airbridge_relay_restore_usb(&relay));
    REQUIRE(model.current == boot_kind);
    REQUIRE(furi_hal_usb_get_config() == furi_hal_usb_spoof_get_active_interface());
    REQUIRE(furi_hal_usb_get_config() != kind_interface(fap_kind));
    REQUIRE(furi_hal_usb_get_config() != &usb_cdc_single);
    assert_device_identity(restored_vendor, restored_product);
    REQUIRE(!model.locked && model.takeover_end_calls == 1);
    REQUIRE(!relay.usb_configured && !relay.usb_transition_dirty);
    airbridge_relay_deinit(&relay);
}

/* MANDATORY CORE S4: CROSS-IDENTITY EXIT, mirroring hardware row 8b. */
static void scenario_s4_cross_identity_exit(void) {
    cross_identity_exit(
        FuriHalUsbIdentityDell, 0, UsbAppComposite, 0x413C, 0x2113);
    cross_identity_exit(
        FuriHalUsbIdentityLogitech, 1, UsbAppDell, 0x046D, 0xC31C);
}

/* S5: PIN convergence clears intent, restores spoof, and never strands lock. */
static void scenario_s5_pin_convergence(void) {
    simulated_boot(FuriHalUsbIdentityDell);
    install_boot_identity();
    REQUIRE(cli_vcp_set_override(&cli_fixture, true));
    model.locked = true;
    cli_vcp_disable_for_lock(&cli_fixture);
    REQUIRE(!model.locked && !model.user_override && !model.is_enabled);
    REQUIRE(!model.callbacks_registered && !model.lock_convergence_pending);
    REQUIRE(model.current == UsbSpoofDell);
    cli_vcp_retry_lock_convergence(&cli_fixture);
    REQUIRE(model.current == UsbSpoofDell && !model.lock_convergence_pending);
    REQUIRE(cli_vcp_set_override(&cli_fixture, true));
    REQUIRE(model.current == UsbCdcSingle && model.user_override && !model.locked);
}

/* S6: the legacy USB-UART path cannot retain CDC as its restore target. */
static void scenario_s6_legacy_usb_uart_exit(void) {
    simulated_boot(FuriHalUsbIdentityDell);
    install_boot_identity();
    REQUIRE(furi_hal_usb_set_config(&usb_cdc_single, NULL));
    cli_vcp_enable(&cli_fixture);
    REQUIRE(model.current == UsbCdcSingle && model.is_enabled);
    REQUIRE(!model.user_override && model.previous_interface == UsbSpoofDell);
    REQUIRE(cli_vcp_set_override(&cli_fixture, true));
    REQUIRE(cli_vcp_set_override(&cli_fixture, false));
    REQUIRE(model.current == UsbSpoofDell && !model.is_enabled && !model.user_override);
}

/* S7: dual CDC is preserved while owned; FAP admission accepts it. Once the
 * legacy owner exits, its normalized restore target is the active spoof. */
static void scenario_s7_dual_cdc_preservation_and_exit(void) {
    simulated_boot(FuriHalUsbIdentityLogitech);
    install_boot_identity();
    REQUIRE(furi_hal_usb_set_config(&usb_cdc_dual, NULL));
    cli_vcp_enable(&cli_fixture);
    REQUIRE(model.current == UsbCdcDual && model.is_enabled);
    REQUIRE(model.previous_interface == UsbSpoofLatched);
    REQUIRE(model.set_config_calls == 2);

    AirbridgeRelay relay;
    relay_init(&relay);
    relay_configure(&relay, 1);
    REQUIRE(relay.usb_mode_prev == &usb_cdc_dual);
    REQUIRE(model.current == UsbAppDell && relay.usb_configured);
    REQUIRE(airbridge_relay_restore_usb(&relay));
    REQUIRE(model.current == UsbCdcDual && model.is_enabled);
    REQUIRE(model.takeover_end_resume && !model.locked);
    airbridge_relay_deinit(&relay);

    /* The legacy dual owner releases to single before CLI reconciliation; OFF
     * then uses the normalized, active-latched spoof restore target. */
    REQUIRE(furi_hal_usb_set_config(&usb_cdc_single, NULL));
    REQUIRE(cli_vcp_set_override(&cli_fixture, true));
    REQUIRE(cli_vcp_set_override(&cli_fixture, false));
    REQUIRE(model.current == UsbSpoofLatched);
    REQUIRE(furi_hal_usb_get_config() == furi_hal_usb_spoof_get_active_interface());
}

int main(void) {
    scenario_s1_s2_default_hid_until_explicit_switch();
    scenario_s3_override_relay_takeover_resume();
    scenario_s4_cross_identity_exit();
    scenario_s5_pin_convergence();
    scenario_s6_legacy_usb_uart_exit();
    scenario_s7_dual_cdc_preservation_and_exit();
    return 0;
}

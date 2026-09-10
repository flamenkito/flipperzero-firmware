#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../applications/services/cli/cli_vcp.h"

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if(!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                              \
        }                                                                         \
    } while(false)

#include "cli_vcp_model.h"

typedef bool (*SetOverrideSignature)(CliVcp*, bool);
typedef bool (*GetBoolSignature)(CliVcp*);
typedef void (*VoidRequestSignature)(CliVcp*);
typedef void (*TakeoverEndSignature)(CliVcp*, bool);
typedef bool (*BootInstallSignature)(CliVcp*, bool);

_Static_assert(
    __builtin_types_compatible_p(__typeof__(&cli_vcp_set_override), SetOverrideSignature),
    "cli_vcp_set_override signature drift");
_Static_assert(
    __builtin_types_compatible_p(__typeof__(&cli_vcp_get_override), GetBoolSignature),
    "cli_vcp_get_override signature drift");
_Static_assert(
    __builtin_types_compatible_p(__typeof__(&cli_vcp_is_enabled), GetBoolSignature),
    "cli_vcp_is_enabled signature drift");
_Static_assert(
    __builtin_types_compatible_p(__typeof__(&cli_vcp_disable_for_lock), VoidRequestSignature),
    "cli_vcp_disable_for_lock signature drift");
_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(&cli_vcp_retry_lock_convergence), VoidRequestSignature),
    "cli_vcp_retry_lock_convergence signature drift");
_Static_assert(
    __builtin_types_compatible_p(__typeof__(&cli_vcp_usb_takeover_begin), GetBoolSignature),
    "cli_vcp_usb_takeover_begin signature drift");
_Static_assert(
    __builtin_types_compatible_p(__typeof__(&cli_vcp_usb_takeover_end), TakeoverEndSignature),
    "cli_vcp_usb_takeover_end signature drift");
_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(&cli_vcp_try_install_boot_identity), BootInstallSignature),
    "cli_vcp_try_install_boot_identity signature drift");

static void busy_precedes_idempotence_and_observed_cdc_is_idempotent(void) {
    Model model = model_make();
    model.current = UsbCdcSingle;
    model.is_enabled = true;
    model.callbacks_registered = true;
    model.previous_interface = UsbSpoofLatched;
    model.locked = true;
    REQUIRE(!model_enable(&model, true));
    REQUIRE(!model.user_override);
    REQUIRE(model.is_enabled);
    REQUIRE(model.previous_interface == UsbSpoofLatched);

    model.locked = false;
    REQUIRE(model_enable(&model, true));
    REQUIRE(model.user_override);
    REQUIRE(model.previous_interface == UsbSpoofLatched);
    REQUIRE(model.set_config_calls == 0);

    model.current = UsbCdcDual;
    REQUIRE(model_enable(&model, false));
    REQUIRE(model.current == UsbCdcDual);
    REQUIRE(model.set_config_calls == 0);
}

static void dual_cdc_owner_is_never_clobbered(void) {
    Model model = model_make();
    model.current = UsbCdcSingle;
    model.is_enabled = true;
    model.callbacks_registered = true;
    model.user_override = true;
    model.previous_interface = UsbSpoofLatched;

    model.current = UsbCdcDual;
    REQUIRE(model_enable(&model, false));
    REQUIRE(model.current == UsbCdcDual);
    REQUIRE(model.callbacks_registered);
    REQUIRE(model_disable(&model));
    REQUIRE(model.current == UsbCdcDual);
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.callbacks_registered);
    REQUIRE(!model.user_override);
    REQUIRE(model.set_config_calls == 0);

    model = model_make();
    model.current = UsbCdcDual;
    REQUIRE(model_enable(&model, false));
    REQUIRE(model.current == UsbCdcDual);
    REQUIRE(model.previous_interface == UsbSpoofLatched);
    REQUIRE(model.callbacks_registered);
    REQUIRE(model.callback_register_calls == 1);
    REQUIRE(model.set_config_calls == 0);
}

static void stale_enabled_falls_through_and_failed_enable_preserves_restore(void) {
    Model model = model_make();
    model.current = UsbAppComposite;
    model.is_enabled = true;
    model.callbacks_registered = true;
    model.previous_interface = UsbSpoofLatched;
    model.set_results[0] = false;
    model.set_result_count = 1;

    REQUIRE(!model_enable(&model, true));
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.callbacks_registered);
    REQUIRE(model.previous_interface == UsbSpoofLatched);
    REQUIRE(!model.user_override);

    model.set_results[1] = true;
    model.set_result_count = 2;
    REQUIRE(model_enable(&model, true));
    REQUIRE(model.current == UsbCdcSingle);
    REQUIRE(model.previous_interface == UsbAppComposite);
    REQUIRE(model.user_override);
}

static void restore_targets_normalize_to_active_latch(void) {
    const UsbInterface candidates[] = {UsbNull, UsbCdcSingle, UsbCdcDual};
    for(size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        Model model = model_make();
        model.current = candidates[i];
        REQUIRE(model_enable(&model, true));
        REQUIRE(model.previous_interface == UsbSpoofLatched);
        REQUIRE(model.previous_interface != model.mutable_preference);
    }
}

static void disable_restore_failure_preserves_bookkeeping(void) {
    Model model = model_make();
    model.current = UsbCdcSingle;
    model.previous_interface = UsbSpoofLatched;
    model.is_enabled = true;
    model.user_override = true;
    model.callbacks_registered = true;
    model.set_results[0] = false;
    model.set_result_count = 1;

    REQUIRE(!model_disable(&model));
    REQUIRE(model.is_enabled);
    REQUIRE(model.user_override);
    REQUIRE(model.callbacks_registered);
    REQUIRE(model.previous_interface == UsbSpoofLatched);
    REQUIRE(model.current == UsbCdcSingle);

    model.set_results[1] = true;
    model.set_result_count = 2;
    REQUIRE(model_disable(&model));
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.user_override);
    REQUIRE(!model.callbacks_registered);
    REQUIRE(model.current == UsbSpoofLatched);
}

static void pending_disable_has_first_precedence(void) {
    Model model = model_make();
    model.current = UsbCdcSingle;
    model.lock_convergence_pending = true;
    model.user_override = true;
    model.locked = true;
    REQUIRE(!model_disable(&model));
    REQUIRE(model.lock_convergence_pending);
    REQUIRE(model.user_override);
    REQUIRE(model.set_config_calls == 0);

    model.locked = false;
    model.set_results[0] = false;
    model.set_result_count = 1;
    REQUIRE(!model_disable(&model));
    REQUIRE(model.lock_convergence_pending);
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.callbacks_registered);
    REQUIRE(model.user_override);

    model.set_results[1] = true;
    model.set_result_count = 2;
    REQUIRE(model_disable(&model));
    REQUIRE(!model.lock_convergence_pending);
    REQUIRE(!model.user_override);
    REQUIRE(model.current == UsbSpoofLatched);
}

static void explicit_enable_cancels_pending_only_on_success(void) {
    Model model = model_make();
    model.current = UsbSpoofLatched;
    model.lock_convergence_pending = true;
    model.locked = true;
    REQUIRE(!model_enable(&model, true));
    REQUIRE(model.lock_convergence_pending);

    model.locked = false;
    model.set_results[0] = false;
    model.set_result_count = 1;
    REQUIRE(!model_enable(&model, true));
    REQUIRE(model.lock_convergence_pending);
    REQUIRE(!model.user_override);

    model.set_results[1] = true;
    model.set_result_count = 2;
    REQUIRE(model_enable(&model, true));
    REQUIRE(!model.lock_convergence_pending);
    REQUIRE(model.user_override);
}

static void pin_lock_fences_unconditionally_and_retries_convergence(void) {
    Model model = model_make();
    model.current = UsbCdcSingle;
    model.previous_interface = UsbSpoofLatched;
    model.is_enabled = true;
    model.user_override = true;
    model.callbacks_registered = true;
    model.is_currently_transmitting = true;
    model.previous_tx_length = 64;
    model.locked = true;
    model.lock_after_unlock = true;

    model_disable_for_lock(&model);
    REQUIRE(model.unlock_calls == 1);
    REQUIRE(model.locked);
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.user_override);
    REQUIRE(!model.callbacks_registered);
    REQUIRE(!model.is_currently_transmitting);
    REQUIRE(model.previous_tx_length == 0);
    REQUIRE(model.lock_convergence_pending);
    REQUIRE(model.current == UsbCdcSingle);

    model.locked = false;
    model.is_enabled = true;
    model.callbacks_registered = true;
    model.is_currently_transmitting = true;
    model.previous_tx_length = 64;
    model.set_results[0] = false;
    model.set_results[1] = true;
    model.set_result_count = 2;
    model_retry_lock_convergence(&model);
    REQUIRE(model.lock_convergence_pending);
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.callbacks_registered);
    REQUIRE(!model.is_currently_transmitting);
    REQUIRE(model.previous_tx_length == 0);
    model_retry_lock_convergence(&model);
    REQUIRE(!model.lock_convergence_pending);
    REQUIRE(model.current == UsbSpoofLatched);

    Model double_failure = model_make();
    double_failure.current = UsbCdcSingle;
    double_failure.lock_convergence_pending = true;
    double_failure.is_enabled = true;
    double_failure.callbacks_registered = true;
    double_failure.set_results[0] = false;
    double_failure.set_results[1] = false;
    double_failure.set_result_count = 2;
    model_disable_for_lock(&double_failure);
    REQUIRE(double_failure.set_config_calls == 2);
    REQUIRE(double_failure.current == UsbCdcSingle);
    REQUIRE(double_failure.lock_convergence_pending);
    REQUIRE(!double_failure.is_enabled);
    REQUIRE(!double_failure.callbacks_registered);
}

static void repeated_lock_convergence_is_idempotent(void) {
    Model model = model_make();
    model.current = UsbSpoofLatched;
    model.lock_convergence_pending = true;
    model.is_enabled = true;
    model.callbacks_registered = true;
    model_disable_for_lock(&model);
    REQUIRE(!model.lock_convergence_pending);
    REQUIRE(model.set_config_calls == 1);
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.callbacks_registered);
}

static void takeover_fences_io_resets_tx_and_cleans_disconnected_pipe(void) {
    Model model = model_make();
    model.current = UsbCdcSingle;
    model.is_enabled = true;
    model.callbacks_registered = true;
    model.user_override = true;
    model.is_currently_transmitting = true;
    model.previous_tx_length = 64;
    model.pipe_connected = true;

    bool resume = model_takeover_begin(&model);
    REQUIRE(resume);
    REQUIRE(model.usb_takeover_active);
    REQUIRE(!model.is_enabled);
    REQUIRE(!model.callbacks_registered);
    REQUIRE(!model.is_currently_transmitting);
    REQUIRE(model.previous_tx_length == 0);
    REQUIRE(model.user_override);
    model_tx_event(&model);
    model_rx_event(&model);
    REQUIRE(model.endpoint_send_calls == 0);
    REQUIRE(model.endpoint_receive_calls == 0);

    model.is_currently_transmitting = true;
    model.previous_tx_length = 64;
    model_disconnected_event(&model);
    REQUIRE(model.disconnected_cleanup_calls == 1);
    REQUIRE(!model.pipe_connected);
    REQUIRE(!model.is_currently_transmitting);
    REQUIRE(model.previous_tx_length == 0);

    model_takeover_end(&model, resume);
    REQUIRE(!model.usb_takeover_active);
    REQUIRE(model.is_enabled);
    REQUIRE(model.callbacks_registered);
    model_tx_event(&model);
    REQUIRE(model.endpoint_send_calls == 1);
}

static void takeover_resume_accepts_single_and_dual_but_skips_pending(void) {
    const UsbInterface cdc_interfaces[] = {UsbCdcSingle, UsbCdcDual};
    for(size_t i = 0; i < sizeof(cdc_interfaces) / sizeof(cdc_interfaces[0]); i++) {
        Model model = model_make();
        model.current = cdc_interfaces[i];
        model.is_enabled = true;
        bool resume = model_takeover_begin(&model);
        model.current = cdc_interfaces[i];
        model_takeover_end(&model, resume);
        REQUIRE(model.is_enabled);
        REQUIRE(model.callbacks_registered);
        REQUIRE(model.current == cdc_interfaces[i]);
    }

    Model pending = model_make();
    pending.current = UsbCdcSingle;
    pending.is_enabled = true;
    bool resume = model_takeover_begin(&pending);
    pending.lock_convergence_pending = true;
    model_takeover_end(&pending, resume);
    REQUIRE(!pending.usb_takeover_active);
    REQUIRE(!pending.is_enabled);
    REQUIRE(!pending.callbacks_registered);
}

static void boot_install_is_serialized_with_takeover(void) {
    Model before = model_make();
    before.current = UsbNull;
    REQUIRE(model_try_install_boot_identity(&before, false));
    REQUIRE(before.current == UsbSpoofLatched);
    (void)model_takeover_begin(&before);
    UsbInterface post_takeover_capture = before.current;
    REQUIRE(post_takeover_capture == UsbSpoofLatched);
    REQUIRE(before.direct_composite_transitions == 0);

    Model after = model_make();
    after.current = UsbNull;
    (void)model_takeover_begin(&after);
    REQUIRE(!model_try_install_boot_identity(&after, false));
    REQUIRE(after.current == UsbNull);
    REQUIRE(after.set_config_calls == 0);

    Model locked = model_make();
    locked.current = UsbNull;
    locked.locked = true;
    REQUIRE(!model_try_install_boot_identity(&locked, false));
    REQUIRE(locked.current == UsbNull);
    REQUIRE(locked.set_config_calls == 0);

    Model during_teardown = model_make();
    during_teardown.current = UsbSpoofLatched;
    (void)model_takeover_begin(&during_teardown);
    during_teardown.current = UsbNull;
    REQUIRE(!model_try_install_boot_identity(&during_teardown, false));
    REQUIRE(during_teardown.current == UsbNull);
    REQUIRE(during_teardown.direct_composite_transitions == 0);

    Model null_only = model_make();
    null_only.current = UsbCdcSingle;
    REQUIRE(model_try_install_boot_identity(&null_only, true));
    REQUIRE(null_only.current == UsbCdcSingle);
    REQUIRE(null_only.set_config_calls == 0);
    null_only.current = UsbNull;
    REQUIRE(model_try_install_boot_identity(&null_only, true));
    REQUIRE(null_only.current == UsbSpoofLatched);
}

int main(void) {
    busy_precedes_idempotence_and_observed_cdc_is_idempotent();
    dual_cdc_owner_is_never_clobbered();
    stale_enabled_falls_through_and_failed_enable_preserves_restore();
    restore_targets_normalize_to_active_latch();
    disable_restore_failure_preserves_bookkeeping();
    pending_disable_has_first_precedence();
    explicit_enable_cancels_pending_only_on_success();
    pin_lock_fences_unconditionally_and_retries_convergence();
    repeated_lock_convergence_is_idempotent();
    takeover_fences_io_resets_tx_and_cleans_disconnected_pipe();
    takeover_resume_accepts_single_and_dual_but_skips_pending();
    boot_install_is_serialized_with_takeover();
    return 0;
}

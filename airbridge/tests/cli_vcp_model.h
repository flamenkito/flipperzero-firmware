#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Drift tripwire: this state-machine mirror follows the queued request handlers,
 * fencing helpers, and public wrappers in applications/services/cli/cli_vcp.c.
 * Keep it aligned with the full TODO-3 ENABLE/DISABLE, PIN-lock convergence,
 * takeover, and serialized boot-install branch tables. */

typedef enum {
    UsbNull,
    UsbCdcSingle,
    UsbCdcDual,
    UsbSpoofLatched,
    UsbSpoofDell,
    UsbSpoofMutablePreference,
    UsbAppComposite,
    UsbAppDell,
} UsbInterface;

typedef struct {
    bool is_enabled;
    bool user_override;
    bool lock_convergence_pending;
    bool usb_takeover_active;
    bool locked;
    bool callbacks_registered;
    bool is_currently_transmitting;
    bool pipe_connected;
    bool lock_after_unlock;
    size_t previous_tx_length;
    UsbInterface current;
    UsbInterface previous_interface;
    UsbInterface active_spoof;
    UsbInterface mutable_preference;
    bool set_results[8];
    size_t set_result_count;
    size_t set_result_index;
    size_t set_config_calls;
    size_t callback_register_calls;
    size_t callback_clear_calls;
    size_t unlock_calls;
    size_t lock_calls;
    size_t endpoint_send_calls;
    size_t endpoint_receive_calls;
    size_t disconnected_cleanup_calls;
    size_t direct_composite_transitions;
    size_t takeover_begin_calls;
    size_t takeover_end_calls;
    bool takeover_end_resume;
    size_t vendor_callback_calls;
    size_t delay_calls;
    uint32_t last_delay;
    uint32_t tick;
} Model;

static inline Model model_make(void) {
    Model model = {
        .active_spoof = UsbSpoofLatched,
        .mutable_preference = UsbSpoofMutablePreference,
        .current = UsbSpoofLatched,
        .previous_interface = UsbSpoofLatched,
    };
    return model;
}

static inline bool is_cdc(UsbInterface interface) {
    return interface == UsbCdcSingle || interface == UsbCdcDual;
}

static inline bool is_composite(UsbInterface interface) {
    return interface == UsbSpoofLatched || interface == UsbSpoofDell ||
           interface == UsbSpoofMutablePreference || interface == UsbAppComposite ||
           interface == UsbAppDell;
}

static inline UsbInterface normalize_restore_target(
    const Model* model,
    UsbInterface candidate) {
    return candidate == UsbNull || is_cdc(candidate) ? model->active_spoof : candidate;
}

static inline bool model_set_config(Model* model, UsbInterface target) {
    model->set_config_calls++;
    if(model->locked) return false;
    bool result = true;
    if(model->set_result_index < model->set_result_count) {
        result = model->set_results[model->set_result_index++];
    }
    if(!result) return false;
    if(is_composite(model->current) && is_composite(target) && model->current != target) {
        model->direct_composite_transitions++;
    }
    model->current = target;
    return true;
}

static inline void model_register_callbacks(Model* model) {
    model->callbacks_registered = true;
    model->callback_register_calls++;
}

static inline void model_clear_callbacks(Model* model) {
    model->callbacks_registered = false;
    model->callback_clear_calls++;
}

static inline void model_fence(Model* model) {
    model->is_enabled = false;
    model_clear_callbacks(model);
    model->is_currently_transmitting = false;
    model->previous_tx_length = 0;
}

static inline void model_enable_success(Model* model, bool explicit_override) {
    model->is_enabled = true;
    if(explicit_override) {
        model->user_override = true;
        model->lock_convergence_pending = false;
    }
}

static inline bool model_enable(Model* model, bool explicit_override) {
    /* BUSY is deliberately first, including before idempotent intent commit. */
    if(model->locked) return false;

    if(model->is_enabled) {
        if(is_cdc(model->current)) {
            if(explicit_override) {
                model->user_override = true;
                model->lock_convergence_pending = false;
            }
            return true;
        }
        model->is_enabled = false;
        model_clear_callbacks(model);
    }

    UsbInterface candidate = model->current;
    if(candidate == UsbCdcDual) {
        model->previous_interface = normalize_restore_target(model, candidate);
        model_register_callbacks(model);
        model_enable_success(model, explicit_override);
        return true;
    }

    if(!model_set_config(model, UsbCdcSingle)) return false;
    model->previous_interface = normalize_restore_target(model, candidate);
    model_register_callbacks(model);
    model_enable_success(model, explicit_override);
    return true;
}

static inline bool model_disable(Model* model) {
    if(model->lock_convergence_pending) {
        if(model->locked) return false;
        model_fence(model);
        if(!model_set_config(model, model->active_spoof)) return false;
        model->lock_convergence_pending = false;
        model->user_override = false;
        return true;
    }

    if(!model->is_enabled) {
        model->user_override = false;
        return true;
    }
    if(model->locked) return false;
    if(model->current != UsbCdcSingle) {
        model->is_enabled = false;
        model_clear_callbacks(model);
        model->user_override = false;
        return true;
    }
    if(!model_set_config(model, model->previous_interface)) return false;
    model->is_enabled = false;
    model_clear_callbacks(model);
    model->user_override = false;
    return true;
}

static inline void model_disable_for_lock(Model* model) {
    model->user_override = false;
    if(model->locked) {
        model->locked = false;
        model->unlock_calls++;
        if(model->lock_after_unlock) model->locked = true;
    }

    /* Step 3.5: unconditional fencing precedes every fallible HAL operation. */
    model_fence(model);
    (void)model_disable(model);

    if(model->current == model->active_spoof) {
        model->lock_convergence_pending = false;
    } else if(model_set_config(model, model->active_spoof)) {
        model->lock_convergence_pending = false;
    } else {
        model->lock_convergence_pending = true;
    }
}

static inline void model_retry_lock_convergence(Model* model) {
    if(!model->lock_convergence_pending || model->locked) return;
    model_fence(model);
    if(model_set_config(model, model->active_spoof)) {
        model->lock_convergence_pending = false;
    }
}

static inline bool model_takeover_begin(Model* model) {
    bool resume = model->is_enabled;
    model_fence(model);
    model->usb_takeover_active = true;
    return resume;
}

static inline void model_takeover_end(Model* model, bool resume) {
    model->usb_takeover_active = false;
    if(model->lock_convergence_pending) return;
    if(resume && is_cdc(model->current)) {
        model_register_callbacks(model);
        model->is_enabled = true;
    }
}

static inline bool model_try_install_boot_identity(
    Model* model,
    bool only_if_currently_null) {
    if(model->locked || model->usb_takeover_active) return false;
    if(only_if_currently_null && model->current != UsbNull) return true;
    return model_set_config(model, model->active_spoof);
}

static inline void model_tx_event(Model* model) {
    if(!model->is_enabled) return;
    model->endpoint_send_calls++;
}

static inline void model_rx_event(Model* model) {
    if(!model->is_enabled) return;
    model->endpoint_receive_calls++;
}

static inline void model_disconnected_event(Model* model) {
    if(!model->pipe_connected) return;
    model->pipe_connected = false;
    model->is_currently_transmitting = false;
    model->previous_tx_length = 0;
    model->disconnected_cleanup_calls++;
}

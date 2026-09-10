#include "cli_vcp.h"
#include <furi_hal_usb_cdc.h>
#include <furi_hal_usb_spoof.h>
#include <furi_hal.h>
#include <furi.h>
#include <stdint.h>
#include <toolbox/pipe.h>
#include <toolbox/cli/shell/cli_shell.h>
#include <toolbox/api_lock.h>
#include "cli.h"
#include "cli_main_shell.h"

#define TAG "CliVcp"

#define USB_CDC_PKT_LEN   CDC_DATA_SZ
#define VCP_BUF_SIZE      (USB_CDC_PKT_LEN * 3)
#define VCP_IF_NUM        0
#define VCP_MESSAGE_Q_LEN 8

#ifdef CLI_VCP_TRACE
#define VCP_TRACE(...) FURI_LOG_T(__VA_ARGS__)
#else
#define VCP_TRACE(...)
#endif

typedef struct {
    enum {
        CliVcpMessageTypeEnable,
        CliVcpMessageTypeDisable,
        CliVcpMessageTypeSetOverride,
        CliVcpMessageTypeGetOverride,
        CliVcpMessageTypeIsEnabled,
        CliVcpMessageTypeDisableForLock,
        CliVcpMessageTypeRetryLockConvergence,
        CliVcpMessageTypeUsbTakeoverBegin,
        CliVcpMessageTypeUsbTakeoverEnd,
        CliVcpMessageTypeTryInstallBootIdentity,
    } type;
    FuriApiLock api_lock;
    union {
        struct {
            bool enabled;
            bool* result;
        } set_override;
        bool* bool_result;
        bool resume;
        struct {
            bool only_if_currently_null;
            bool* result;
        } install_boot_identity;
    } data;
} CliVcpMessage;

typedef enum {
    CliVcpInternalEventConnected,
    CliVcpInternalEventDisconnected,
    CliVcpInternalEventTxDone,
    CliVcpInternalEventRx,
} CliVcpInternalEvent;

struct CliVcp {
    const CliVcpInternalApi* internal_api;
    FuriEventLoop* event_loop;
    FuriMessageQueue* message_queue; // <! external messages
    FuriMessageQueue* internal_evt_queue;

    /* user_override is intent, is_enabled is actual, and the HAL interface pointer is observation. */
    volatile bool user_override;
    bool is_enabled, is_connected;
    bool lock_convergence_pending;
    bool usb_takeover_active;
    FuriHalUsbInterface* previous_interface;

    PipeSide* own_pipe;
    PipeSide* shell_pipe;
    volatile bool is_currently_transmitting;
    size_t previous_tx_length;

    CliRegistry* main_registry;
    CliShell* shell;
};

static bool cli_vcp_set_override_impl(CliVcp* cli_vcp, bool enabled);
static bool cli_vcp_get_override_impl(CliVcp* cli_vcp);
static bool cli_vcp_is_enabled_impl(CliVcp* cli_vcp);
static void cli_vcp_disable_for_lock_impl(CliVcp* cli_vcp);
static void cli_vcp_retry_lock_convergence_impl(CliVcp* cli_vcp);
static bool
    cli_vcp_try_install_boot_identity_impl(CliVcp* cli_vcp, bool only_if_currently_null);

static const CliVcpInternalApi cli_vcp_internal_api = {
    .set_override = cli_vcp_set_override_impl,
    .get_override = cli_vcp_get_override_impl,
    .is_enabled = cli_vcp_is_enabled_impl,
    .disable_for_lock = cli_vcp_disable_for_lock_impl,
    .retry_lock_convergence = cli_vcp_retry_lock_convergence_impl,
    .try_install_boot_identity = cli_vcp_try_install_boot_identity_impl,
};

// ============
// Data copying
// ============

/**
 * Called in the following cases:
 *   - previous transfer has finished;
 *   - new data became available to send.
 */
static void cli_vcp_maybe_send_data(CliVcp* cli_vcp) {
    // A queued pre-fence event must never touch a reused CDC endpoint.
    if(!cli_vcp->is_enabled) return;
    if(cli_vcp->is_currently_transmitting) return;
    if(!cli_vcp->own_pipe) return;

    uint8_t buf[USB_CDC_PKT_LEN];
    size_t to_receive_from_pipe = MIN(sizeof(buf), pipe_bytes_available(cli_vcp->own_pipe));
    size_t length = pipe_receive(cli_vcp->own_pipe, buf, to_receive_from_pipe);
    if(length > 0 || cli_vcp->previous_tx_length == USB_CDC_PKT_LEN) {
        VCP_TRACE(TAG, "cdc_send length=%zu", length);
        cli_vcp->is_currently_transmitting = true;
        furi_hal_cdc_send(VCP_IF_NUM, buf, length);
    }
    cli_vcp->previous_tx_length = length;
}

/**
 * Called in the following cases:
 *   - new data arrived at the endpoint;
 *   - data was read out of the pipe.
 */
static void cli_vcp_maybe_receive_data(CliVcp* cli_vcp) {
    // A queued pre-fence event must never touch a reused CDC endpoint or a freed pipe.
    if(!cli_vcp->is_enabled) return;
    if(!cli_vcp->own_pipe) return;
    if(pipe_spaces_available(cli_vcp->own_pipe) < USB_CDC_PKT_LEN) return;

    uint8_t buf[USB_CDC_PKT_LEN];
    size_t length = furi_hal_cdc_receive(VCP_IF_NUM, buf, sizeof(buf));
    VCP_TRACE(TAG, "cdc_receive length=%zu", length);
    furi_check(pipe_send(cli_vcp->own_pipe, buf, length) == length);
}

// =============
// CDC callbacks
// =============

static void cli_vcp_signal_internal_event(CliVcp* cli_vcp, CliVcpInternalEvent event) {
    furi_check(furi_message_queue_put(cli_vcp->internal_evt_queue, &event, 0) == FuriStatusOk);
}

static void cli_vcp_cdc_tx_done(void* context) {
    CliVcp* cli_vcp = context;
    cli_vcp->is_currently_transmitting = false;
    cli_vcp_signal_internal_event(cli_vcp, CliVcpInternalEventTxDone);
}

static void cli_vcp_cdc_rx(void* context) {
    CliVcp* cli_vcp = context;
    cli_vcp_signal_internal_event(cli_vcp, CliVcpInternalEventRx);
}

static void cli_vcp_cdc_state_callback(void* context, CdcState state) {
    CliVcp* cli_vcp = context;
    if(state == CdcStateDisconnected) {
        cli_vcp_signal_internal_event(cli_vcp, CliVcpInternalEventDisconnected);
    }
    // `Connected` events are generated by DTR going active
}

static void cli_vcp_cdc_ctrl_line_callback(void* context, CdcCtrlLine ctrl_lines) {
    CliVcp* cli_vcp = context;
    if(ctrl_lines & CdcCtrlLineDTR) {
        cli_vcp_signal_internal_event(cli_vcp, CliVcpInternalEventConnected);
    } else {
        cli_vcp_signal_internal_event(cli_vcp, CliVcpInternalEventDisconnected);
    }
}

static CdcCallbacks cdc_callbacks = {
    .tx_ep_callback = cli_vcp_cdc_tx_done,
    .rx_ep_callback = cli_vcp_cdc_rx,
    .state_callback = cli_vcp_cdc_state_callback,
    .ctrl_line_callback = cli_vcp_cdc_ctrl_line_callback,
    .config_callback = NULL,
};

// ======================
// Pipe callback handlers
// ======================

static void cli_vcp_data_from_shell(PipeSide* pipe, void* context) {
    UNUSED(pipe);
    CliVcp* cli_vcp = context;
    cli_vcp_maybe_send_data(cli_vcp);
}

static void cli_vcp_shell_ready(PipeSide* pipe, void* context) {
    UNUSED(pipe);
    CliVcp* cli_vcp = context;
    cli_vcp_maybe_receive_data(cli_vcp);
}

/**
 * Processes messages arriving from other threads
 */
static bool cli_vcp_observed_cdc(FuriHalUsbInterface* interface) {
    return interface == &usb_cdc_single || interface == &usb_cdc_dual;
}

static FuriHalUsbInterface*
    cli_vcp_normalize_restore_target(FuriHalUsbInterface* candidate) {
    if(!candidate || cli_vcp_observed_cdc(candidate)) {
        return furi_hal_usb_spoof_get_active_interface();
    }
    return candidate;
}

static void cli_vcp_reset_transport_bookkeeping(CliVcp* cli_vcp) {
    cli_vcp->is_currently_transmitting = false;
    cli_vcp->previous_tx_length = 0;
}

static void cli_vcp_fence_io(CliVcp* cli_vcp) {
    cli_vcp->is_enabled = false;
    furi_hal_cdc_set_callbacks(VCP_IF_NUM, NULL, NULL);
    cli_vcp_reset_transport_bookkeeping(cli_vcp);
}

static void cli_vcp_enable_succeeded(CliVcp* cli_vcp, bool explicit_override) {
    cli_vcp->is_enabled = true;
    if(explicit_override) {
        cli_vcp->user_override = true;
        cli_vcp->lock_convergence_pending = false;
    }
}

static bool cli_vcp_handle_enable(CliVcp* cli_vcp, bool explicit_override) {
    if(furi_hal_usb_is_locked()) return false;

    FuriHalUsbInterface* observed = furi_hal_usb_get_config();
    if(cli_vcp->is_enabled) {
        if(cli_vcp_observed_cdc(observed)) {
            if(explicit_override) {
                cli_vcp->user_override = true;
                cli_vcp->lock_convergence_pending = false;
            }
            return true;
        }

        cli_vcp->is_enabled = false;
        furi_hal_cdc_set_callbacks(VCP_IF_NUM, NULL, NULL);
    }

    if(observed == &usb_cdc_dual) {
        cli_vcp->previous_interface = cli_vcp_normalize_restore_target(observed);
        furi_hal_cdc_set_callbacks(VCP_IF_NUM, &cdc_callbacks, cli_vcp);
        cli_vcp_enable_succeeded(cli_vcp, explicit_override);
        return true;
    }

    if(!furi_hal_usb_set_config(&usb_cdc_single, NULL)) return false;

    cli_vcp->previous_interface = cli_vcp_normalize_restore_target(observed);
    furi_hal_cdc_set_callbacks(VCP_IF_NUM, &cdc_callbacks, cli_vcp);
    cli_vcp_enable_succeeded(cli_vcp, explicit_override);
    return true;
}

static bool cli_vcp_handle_disable(CliVcp* cli_vcp) {
    if(cli_vcp->lock_convergence_pending) {
        if(furi_hal_usb_is_locked()) return false;
        cli_vcp_fence_io(cli_vcp);
        if(!furi_hal_usb_set_config(furi_hal_usb_spoof_get_active_interface(), NULL)) {
            return false;
        }
        cli_vcp->lock_convergence_pending = false;
        cli_vcp->user_override = false;
        return true;
    }

    if(!cli_vcp->is_enabled) {
        cli_vcp->user_override = false;
        return true;
    }
    if(furi_hal_usb_is_locked()) return false;

    if(furi_hal_usb_get_config() != &usb_cdc_single) {
        cli_vcp->is_enabled = false;
        furi_hal_cdc_set_callbacks(VCP_IF_NUM, NULL, NULL);
        cli_vcp->user_override = false;
        return true;
    }

    if(!furi_hal_usb_set_config(cli_vcp->previous_interface, NULL)) return false;

    cli_vcp->is_enabled = false;
    furi_hal_cdc_set_callbacks(VCP_IF_NUM, NULL, NULL);
    cli_vcp->user_override = false;
    return true;
}

static void cli_vcp_handle_disable_for_lock(CliVcp* cli_vcp) {
    cli_vcp->user_override = false;
    if(furi_hal_usb_is_locked()) {
        // PIN-lock convergence intentionally terminates qFlipper/VCP ownership.
        furi_hal_usb_unlock();
    }

    // Security postcondition step 3.5: fence before any fallible HAL operation.
    cli_vcp_fence_io(cli_vcp);
    cli_vcp_handle_disable(cli_vcp);

    FuriHalUsbInterface* active = furi_hal_usb_spoof_get_active_interface();
    if(furi_hal_usb_get_config() == active) {
        cli_vcp->lock_convergence_pending = false;
    } else if(furi_hal_usb_set_config(active, NULL)) {
        cli_vcp->lock_convergence_pending = false;
    } else {
        FURI_LOG_E(TAG, "Failed to converge USB identity for PIN lock");
        cli_vcp->lock_convergence_pending = true;
    }
}

static void cli_vcp_handle_retry_lock_convergence(CliVcp* cli_vcp) {
    if(!cli_vcp->lock_convergence_pending || furi_hal_usb_is_locked()) return;

    cli_vcp_fence_io(cli_vcp);
    if(furi_hal_usb_set_config(furi_hal_usb_spoof_get_active_interface(), NULL)) {
        cli_vcp->lock_convergence_pending = false;
    } else {
        FURI_LOG_E(TAG, "Failed to retry USB identity convergence after PIN lock");
    }
}

static void cli_vcp_message_received(FuriEventLoopObject* object, void* context) {
    CliVcp* cli_vcp = context;
    CliVcpMessage message;
    furi_check(furi_message_queue_get(object, &message, 0) == FuriStatusOk);

    switch(message.type) {
    case CliVcpMessageTypeEnable:
        FURI_LOG_D(TAG, "Enabling");
        cli_vcp_handle_enable(cli_vcp, false);
        break;

    case CliVcpMessageTypeDisable:
        FURI_LOG_D(TAG, "Disabling");
        cli_vcp_handle_disable(cli_vcp);
        break;

    case CliVcpMessageTypeSetOverride:
        *message.data.set_override.result =
            message.data.set_override.enabled ? cli_vcp_handle_enable(cli_vcp, true) :
                                                cli_vcp_handle_disable(cli_vcp);
        break;

    case CliVcpMessageTypeGetOverride:
        *message.data.bool_result = cli_vcp->user_override;
        break;

    case CliVcpMessageTypeIsEnabled:
        *message.data.bool_result = cli_vcp->is_enabled;
        break;

    case CliVcpMessageTypeDisableForLock:
        cli_vcp_handle_disable_for_lock(cli_vcp);
        break;

    case CliVcpMessageTypeRetryLockConvergence:
        cli_vcp_handle_retry_lock_convergence(cli_vcp);
        break;

    case CliVcpMessageTypeUsbTakeoverBegin:
        *message.data.bool_result = cli_vcp->is_enabled;
        // The external and CDC queues share one event loop but not one FIFO: callbacks are removed,
        // and the is_enabled guards make already-queued endpoint events harmless after return.
        cli_vcp_fence_io(cli_vcp);
        cli_vcp->usb_takeover_active = true;
        break;

    case CliVcpMessageTypeUsbTakeoverEnd:
        cli_vcp->usb_takeover_active = false;
        if(!cli_vcp->lock_convergence_pending && message.data.resume &&
           cli_vcp_observed_cdc(furi_hal_usb_get_config())) {
            furi_hal_cdc_set_callbacks(VCP_IF_NUM, &cdc_callbacks, cli_vcp);
            cli_vcp->is_enabled = true;
        }
        break;

    case CliVcpMessageTypeTryInstallBootIdentity:
        if(furi_hal_usb_is_locked() || cli_vcp->usb_takeover_active) {
            *message.data.install_boot_identity.result = false;
        } else if(message.data.install_boot_identity.only_if_currently_null &&
                  furi_hal_usb_get_config() != NULL) {
            *message.data.install_boot_identity.result = true;
        } else {
            *message.data.install_boot_identity.result = furi_hal_usb_set_config(
                furi_hal_usb_spoof_get_active_interface(), NULL);
        }
        break;
    }

    api_lock_unlock(message.api_lock);
}

/**
 * Processes messages arriving from CDC event callbacks
 */
static void cli_vcp_internal_event_happened(FuriEventLoopObject* object, void* context) {
    CliVcp* cli_vcp = context;
    CliVcpInternalEvent event;
    furi_check(furi_message_queue_get(object, &event, 0) == FuriStatusOk);

    switch(event) {
    case CliVcpInternalEventRx: {
        VCP_TRACE(TAG, "Rx");
        if(!cli_vcp->is_enabled) break;
        cli_vcp_maybe_receive_data(cli_vcp);
        break;
    }

    case CliVcpInternalEventTxDone: {
        VCP_TRACE(TAG, "TxDone");
        if(!cli_vcp->is_enabled) break;
        cli_vcp_maybe_send_data(cli_vcp);
        break;
    }

    case CliVcpInternalEventDisconnected: {
        cli_vcp_reset_transport_bookkeeping(cli_vcp);
        if(!cli_vcp->is_connected) return;
        FURI_LOG_D(TAG, "Disconnected");
        cli_vcp->is_connected = false;

        // disconnect our side of the pipe
        pipe_detach_from_event_loop(cli_vcp->own_pipe);
        pipe_free(cli_vcp->own_pipe);
        cli_vcp->own_pipe = NULL;

        // wait for shell to stop
        cli_shell_join(cli_vcp->shell);
        cli_shell_free(cli_vcp->shell);
        pipe_free(cli_vcp->shell_pipe);
        break;
    }

    case CliVcpInternalEventConnected: {
        if(cli_vcp->is_connected) return;
        FURI_LOG_D(TAG, "Connected");
        cli_vcp->is_connected = true;

        // start shell thread
        PipeSideBundle bundle = pipe_alloc(VCP_BUF_SIZE, 1);
        cli_vcp->own_pipe = bundle.alices_side;
        cli_vcp->shell_pipe = bundle.bobs_side;
        pipe_attach_to_event_loop(cli_vcp->own_pipe, cli_vcp->event_loop);
        pipe_set_callback_context(cli_vcp->own_pipe, cli_vcp);
        pipe_set_data_arrived_callback(
            cli_vcp->own_pipe, cli_vcp_data_from_shell, FuriEventLoopEventFlagEdge);
        pipe_set_space_freed_callback(
            cli_vcp->own_pipe, cli_vcp_shell_ready, FuriEventLoopEventFlagEdge);
        furi_delay_ms(33); // we are too fast, minicom isn't ready yet
        cli_vcp->shell = cli_shell_alloc(
            cli_main_motd, NULL, cli_vcp->shell_pipe, cli_vcp->main_registry, &cli_main_ext_config);
        cli_shell_start(cli_vcp->shell);
        break;
    }
    }
}

// ============
// Thread stuff
// ============

static CliVcp* cli_vcp_alloc(void) {
    CliVcp* cli_vcp = malloc(sizeof(CliVcp));

    cli_vcp->internal_api = &cli_vcp_internal_api;
    cli_vcp->user_override = false;
    cli_vcp->is_enabled = false;
    cli_vcp->is_connected = false;
    cli_vcp->lock_convergence_pending = false;
    cli_vcp->usb_takeover_active = false;
    cli_vcp->previous_interface = NULL;
    cli_vcp->own_pipe = NULL;
    cli_vcp->shell_pipe = NULL;
    cli_vcp->is_currently_transmitting = false;
    cli_vcp->previous_tx_length = 0;
    cli_vcp->shell = NULL;

    cli_vcp->event_loop = furi_event_loop_alloc();

    cli_vcp->message_queue = furi_message_queue_alloc(VCP_MESSAGE_Q_LEN, sizeof(CliVcpMessage));
    furi_event_loop_subscribe_message_queue(
        cli_vcp->event_loop,
        cli_vcp->message_queue,
        FuriEventLoopEventIn,
        cli_vcp_message_received,
        cli_vcp);

    cli_vcp->internal_evt_queue =
        furi_message_queue_alloc(VCP_MESSAGE_Q_LEN, sizeof(CliVcpInternalEvent));
    furi_event_loop_subscribe_message_queue(
        cli_vcp->event_loop,
        cli_vcp->internal_evt_queue,
        FuriEventLoopEventIn,
        cli_vcp_internal_event_happened,
        cli_vcp);

    cli_vcp->main_registry = furi_record_open(RECORD_CLI);

    return cli_vcp;
}

int32_t cli_vcp_srv(void* p) {
    UNUSED(p);

    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        FURI_LOG_W(TAG, "Skipping start in special boot mode");
        furi_thread_suspend(furi_thread_get_current_id());
        return 0;
    }

    CliVcp* cli_vcp = cli_vcp_alloc();
    furi_record_create(RECORD_CLI_VCP, cli_vcp);
    furi_event_loop_run(cli_vcp->event_loop);

    return 0;
}

// ==========
// Public API
// ==========

static void cli_vcp_synchronous_request(CliVcp* cli_vcp, CliVcpMessage* message) {
    message->api_lock = api_lock_alloc_locked();
    furi_message_queue_put(cli_vcp->message_queue, message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message->api_lock);
}

void cli_vcp_enable(CliVcp* cli_vcp) {
    furi_check(cli_vcp);
    CliVcpMessage message = {
        .type = CliVcpMessageTypeEnable,
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
}

void cli_vcp_disable(CliVcp* cli_vcp) {
    furi_check(cli_vcp);
    CliVcpMessage message = {
        .type = CliVcpMessageTypeDisable,
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
}

static bool cli_vcp_set_override_impl(CliVcp* cli_vcp, bool enabled) {
    furi_check(cli_vcp);
    bool result = false;
    CliVcpMessage message = {
        .type = CliVcpMessageTypeSetOverride,
        .data.set_override = {.enabled = enabled, .result = &result},
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
    return result;
}

static bool cli_vcp_get_override_impl(CliVcp* cli_vcp) {
    furi_check(cli_vcp);
    bool result = false;
    CliVcpMessage message = {
        .type = CliVcpMessageTypeGetOverride,
        .data.bool_result = &result,
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
    return result;
}

static bool cli_vcp_is_enabled_impl(CliVcp* cli_vcp) {
    furi_check(cli_vcp);
    bool result = false;
    CliVcpMessage message = {
        .type = CliVcpMessageTypeIsEnabled,
        .data.bool_result = &result,
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
    return result;
}

static void cli_vcp_disable_for_lock_impl(CliVcp* cli_vcp) {
    furi_check(cli_vcp);
    CliVcpMessage message = {.type = CliVcpMessageTypeDisableForLock};
    cli_vcp_synchronous_request(cli_vcp, &message);
}

static void cli_vcp_retry_lock_convergence_impl(CliVcp* cli_vcp) {
    furi_check(cli_vcp);
    CliVcpMessage message = {.type = CliVcpMessageTypeRetryLockConvergence};
    cli_vcp_synchronous_request(cli_vcp, &message);
}

bool cli_vcp_usb_takeover_begin(CliVcp* cli_vcp) {
    furi_check(cli_vcp);
    bool resume = false;
    CliVcpMessage message = {
        .type = CliVcpMessageTypeUsbTakeoverBegin,
        .data.bool_result = &resume,
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
    return resume;
}

void cli_vcp_usb_takeover_end(CliVcp* cli_vcp, bool resume) {
    furi_check(cli_vcp);
    CliVcpMessage message = {
        .type = CliVcpMessageTypeUsbTakeoverEnd,
        .data.resume = resume,
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
}

static bool
    cli_vcp_try_install_boot_identity_impl(CliVcp* cli_vcp, bool only_if_currently_null) {
    furi_check(cli_vcp);
    bool result = false;
    CliVcpMessage message = {
        .type = CliVcpMessageTypeTryInstallBootIdentity,
        .data.install_boot_identity = {
            .only_if_currently_null = only_if_currently_null,
            .result = &result,
        },
    };
    cli_vcp_synchronous_request(cli_vcp, &message);
    return result;
}

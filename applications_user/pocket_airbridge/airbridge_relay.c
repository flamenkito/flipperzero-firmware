#include <stdlib.h>
#include <string.h>

#include "airbridge_relay.h"

#include "airbridge_usb.h"
#include "airbridge_exit_contract.h"
#include <furi_hal_usb_hid.h>
#include <furi_hal_usb_spoof.h>

#include <bt/bt_service/bt.h>

#ifndef AIRBRIDGE_SAFE_TEARDOWN_ENABLED
#define AIRBRIDGE_SAFE_TEARDOWN_ENABLED 1
#endif

#define TAG "AirBridge"

static void airbridge_relay_increment(uint32_t* counter) {
    FURI_CRITICAL_ENTER();
    (*counter)++;
    FURI_CRITICAL_EXIT();
}

void airbridge_relay_count_drop(AirbridgeRelay* relay) {
    airbridge_relay_increment(&relay->metrics.dropped);
}

void airbridge_relay_metrics_snapshot(AirbridgeRelay* relay, AirbridgeRelayMetrics* snapshot) {
    FURI_CRITICAL_ENTER();
    *snapshot = relay->metrics;
    FURI_CRITICAL_EXIT();
}

static void usb_event_callback(HidVendorEvent ev, void* context) {
    AirbridgeRelay* relay = context;
    FuriMessageQueue* queue = relay->event_queue;
    BridgeEvent be = {0};
    if(ev == HidVendorConnected) {
        be.type = EVENT_TYPE_USB;
        be.to_ble = true;
        if(furi_message_queue_put(queue, &be, 0) != FuriStatusOk) {
            airbridge_relay_count_drop(relay);
        }
    } else if(ev == HidVendorDisconnected) {
        be.type = EVENT_TYPE_USB;
        be.to_ble = false;
        if(furi_message_queue_put(queue, &be, 0) != FuriStatusOk) {
            airbridge_relay_count_drop(relay);
        }
    } else if(ev == HidVendorRequest) {
        uint32_t len = airbridge_usb_vendor_get_request(be.data);
        if(len > 0 && len <= HID_VENDOR_PACKET_LEN) {
            be.type = EVENT_TYPE_RELAY;
            be.len = len;
            be.to_ble = true;
            if(furi_message_queue_put(queue, &be, 0) != FuriStatusOk) {
                airbridge_relay_count_drop(relay);
            }
        }
    }
}

uint16_t airbridge_relay_ble_event(SerialServiceEvent event, void* context) {
    /* Confirmation and legacy RPC-reset writes are not bridge data. */
    if(event.event != SerialServiceEventTypeDataReceived) return 0;
    const uint8_t* data = event.data.buffer;
    const uint16_t len = event.data.size;
    AirbridgeRelay* relay = context;
    if(len > 0 && len <= HID_VENDOR_PACKET_LEN) {
        BridgeEvent be = {
            .type = EVENT_TYPE_RELAY,
            .tick = furi_get_tick(),
            .len = len,
            .to_ble = false,
        };
        memcpy(be.data, data, len);
        if(furi_message_queue_put(relay->event_queue, &be, 0) == FuriStatusOk) {
            return HID_VENDOR_PACKET_LEN;
        }
        airbridge_relay_count_drop(relay);
    }
    return 0;
}

static bool airbridge_usb_is_cdc(FuriHalUsbInterface* interface) {
    return interface == &usb_cdc_single || interface == &usb_cdc_dual;
}

static bool airbridge_usb_is_spoof(FuriHalUsbInterface* interface) {
    return interface == furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileLogitech) ||
           interface == furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfileDell);
}

static bool airbridge_usb_switch_safe(AirbridgeRelay* relay, FuriHalUsbInterface* target) {
    FuriHalUsbInterface* source = furi_hal_usb_get_config();
    if(source == target) {
        relay->usb_transition_dirty = false;
        return true;
    }

    const bool source_is_fap = source == relay->usb_mode_owned;
    const bool target_is_fap = target == relay->usb_mode_owned;
    const bool source_is_cdc = airbridge_usb_is_cdc(source);
    const bool target_is_cdc = airbridge_usb_is_cdc(target);
    const bool source_is_spoof = airbridge_usb_is_spoof(source);
    const bool target_is_spoof = airbridge_usb_is_spoof(target);
    const bool direct = (source_is_fap && target_is_cdc) ||
                        (source_is_cdc && target_is_fap) ||
                        (source == NULL &&
                         (target_is_cdc || target == furi_hal_usb_spoof_get_active_interface() ||
                          (target_is_fap && AIRBRIDGE_SAFE_TEARDOWN_ENABLED)));

    if(direct) {
        if(!furi_hal_usb_set_config(target, NULL)) {
            if(source == NULL && target_is_fap) relay->usb_transition_dirty = true;
            return false;
        }
        relay->usb_transition_dirty = false;
        return true;
    }

    const bool teardown = AIRBRIDGE_SAFE_TEARDOWN_ENABLED &&
                          ((source_is_spoof && target_is_fap) ||
                           (source_is_fap && target_is_spoof));
    if(!teardown) return false;

    /* Experimental: HAL NULL deinitializes and disconnects without resetting the
     * peripheral. Hardware row 8 decides whether this strategy remains enabled. */
    if(!furi_hal_usb_set_config(NULL, NULL)) {
        FURI_LOG_E(TAG, "USB teardown failed");
        return false;
    }
    relay->usb_transition_dirty = true;
    furi_delay_ms(500);
    if(furi_hal_usb_set_config(target, NULL)) {
        relay->usb_transition_dirty = false;
        return true;
    }

    FURI_LOG_E(TAG, "USB install failed; rolling back");
    if(furi_hal_usb_set_config(source, NULL)) {
        relay->usb_transition_dirty = false;
        relay->usb_configured = !target_is_fap;
    } else {
        relay->usb_configured = false;
        relay->usb_transition_dirty = true;
        FURI_LOG_E(TAG, "USB rollback failed; lifecycle retry required");
    }
    return false;
}

bool airbridge_relay_configure_usb(
    AirbridgeRelay* relay,
    uint8_t profile_index,
    uint8_t* selected_profile_index) {
    FuriHalUsbInterface* profile = airbridge_usb_get_profile(profile_index);
    if(profile == NULL) return false;

    if(furi_hal_usb_is_locked()) {
        furi_hal_usb_unlock();
    }
    relay->cli_was_enabled = cli_vcp_usb_takeover_begin(relay->cli_vcp);
    relay->usb_takeover_active = true;
    relay->usb_mode_prev = furi_hal_usb_get_config();
    relay->usb_mode_owned = profile;
    relay->admission_rejected = false;

    if(!AIRBRIDGE_SAFE_TEARDOWN_ENABLED && !airbridge_usb_is_cdc(relay->usb_mode_prev)) {
        relay->admission_rejected = true;
        cli_vcp_usb_takeover_end(relay->cli_vcp, relay->cli_was_enabled);
        relay->usb_takeover_active = false;
        return false;
    }

    if(!airbridge_usb_switch_safe(relay, profile)) {
        FURI_LOG_E(TAG, "USB profile install failed");
        cli_vcp_usb_takeover_end(relay->cli_vcp, relay->cli_was_enabled);
        relay->usb_takeover_active = false;
        return false;
    }

    *selected_profile_index = profile_index;
    relay->usb_configured = true;
    relay->usb_transition_dirty = false;
    furi_hal_usb_lock();
    relay->usb_lock_held = true;

    airbridge_usb_vendor_set_callback(usb_event_callback, relay);
    return true;
}

bool airbridge_relay_restore_usb(AirbridgeRelay* relay) {
    if(!relay->usb_configured && !relay->usb_transition_dirty) return true;

    if(relay->usb_lock_held) {
        furi_hal_usb_unlock();
        relay->usb_lock_held = false;
    }

    airbridge_usb_vendor_set_callback(NULL, NULL);
    FuriHalUsbInterface* target = relay->usb_mode_prev;
    if(target == NULL) target = furi_hal_usb_spoof_get_active_interface();
    if(!airbridge_usb_switch_safe(relay, target)) return false;

    relay->usb_configured = false;
    relay->usb_transition_dirty = false;
    if(relay->usb_takeover_active) {
        cli_vcp_usb_takeover_end(relay->cli_vcp, relay->cli_was_enabled);
        relay->usb_takeover_active = false;
    }
    /* Never re-lock here: a terminated RPC owner could otherwise strand the
     * advisory lock. A late RPC cleanup unlock remains an accepted residual. */
    return true;
}

void airbridge_relay_init(AirbridgeRelay* relay) {
    relay->event_queue = furi_message_queue_alloc(8, sizeof(BridgeEvent));
    relay->cli_vcp = furi_record_open(RECORD_CLI_VCP);
}

void airbridge_relay_deinit(AirbridgeRelay* relay) {
    furi_record_close(RECORD_CLI_VCP);
    furi_message_queue_free(relay->event_queue);
}

void airbridge_relay_wake(AirbridgeRelay* relay) {
    BridgeEvent event = {.type = EVENT_TYPE_WAKE};
    (void)furi_message_queue_put(relay->event_queue, &event, 0);
}

FuriStatus airbridge_relay_poll(AirbridgeRelay* relay, BridgeEvent* event, uint32_t timeout) {
    return furi_message_queue_get(relay->event_queue, event, timeout);
}

void airbridge_relay_set_usb_connected(AirbridgeRelay* relay, bool connected) {
    FURI_CRITICAL_ENTER();
    relay->metrics.usb_connected = connected;
    FURI_CRITICAL_EXIT();
}

AirbridgeRelayResult airbridge_relay_handle(
    AirbridgeRelay* relay,
    AirbridgeBle* ble,
    BridgeEvent* be,
    AirbridgeScreen screen,
    AirbridgeTypingTransport armed_transport) {
    /* Windows delivers the full 64-byte OUT transfer; macOS sends a short 1-byte
       transfer. Match on the first byte only so both arm deploy. Direction
       selects the deploy path: a USB fetch hits the vendor OUT endpoint
       (to_ble), a BLE fetch arrives over serial (BLE-origin). */
    const bool usb_deploy_request = be->to_ble && (be->len >= 1) && (be->data[0] == 0x42);
    const bool ble_deploy_request = !be->to_ble && (be->len >= 1) && (be->data[0] == 0x42);
    if(screen == AirbridgeScreenWaiting) {
        /* Waiting is armed per transport by the matching prompt's typing
         * completion. A cross-direction 0x42 is bridge data, not a deploy:
         * relay it normally below. */
        if(usb_deploy_request && armed_transport == AirbridgeTypingTransportUsb) {
            return AirbridgeRelayDeployRequestedUsb;
        }
        if(ble_deploy_request && armed_transport == AirbridgeTypingTransportBle) {
            return AirbridgeRelayDeployRequestedBle;
        }
    } else if((usb_deploy_request || ble_deploy_request) && screen != AirbridgeScreenBridge) {
        return AirbridgeRelayDeployNotArmed;
    }
    if(be->to_ble) {
        if(airbridge_ble_send(ble, be->data, be->len)) {
            airbridge_relay_increment(&relay->metrics.chunks_usb_to_ble);
        } else {
            airbridge_relay_increment(&relay->metrics.tx_errors);
        }
    } else {
        uint8_t report[HID_VENDOR_PACKET_LEN] = {0};
        memcpy(report, be->data, be->len);
        if(airbridge_usb_vendor_send_response_blocking(
               report, HID_VENDOR_PACKET_LEN, AIRBRIDGE_EXIT_POLL_INTERVAL_MS)) {
            airbridge_relay_increment(&relay->metrics.chunks_ble_to_usb);
        } else {
            airbridge_relay_increment(&relay->metrics.tx_errors);
        }
    }
    return AirbridgeRelayHandled;
}

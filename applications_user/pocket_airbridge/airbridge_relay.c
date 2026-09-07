#include <stdlib.h>
#include <string.h>

#include "airbridge_relay.h"

#include <furi_hal_usb_airbridge.h>
#include <furi_hal_usb_hid.h>

#include <bt/bt_service/bt.h>

static void airbridge_relay_increment(uint32_t* counter) {
    FURI_CRITICAL_ENTER();
    (*counter)++;
    FURI_CRITICAL_EXIT();
}

void airbridge_relay_count_drop(AirbridgeRelay* relay) {
    airbridge_relay_increment(&relay->metrics.dropped);
}

void airbridge_relay_metrics_snapshot(
    AirbridgeRelay* relay,
    AirbridgeRelayMetrics* snapshot) {
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
        uint32_t len = furi_hal_hid_vendor_get_request(be.data);
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

static uint16_t ble_raw_serial_callback(const uint8_t* data, uint16_t len, void* context) {
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

static bool app_apply_profile(
    AirbridgeRelay* relay,
    uint8_t profile_index,
    uint8_t* selected_profile_index) {
    FuriHalUsbInterface* profile = furi_hal_usb_airbridge_get_profile(profile_index);
    if(profile == NULL || !furi_hal_usb_set_config(profile, NULL)) {
        return false;
    }

    *selected_profile_index = profile_index;
    relay->usb_configured = true;
    return true;
}

bool airbridge_relay_configure_usb(
    AirbridgeRelay* relay,
    uint8_t profile_index,
    uint8_t* selected_profile_index) {
    FuriHalUsbInterface* profile = furi_hal_usb_airbridge_get_profile(profile_index);
    if(profile == NULL) return false;

    if(furi_hal_usb_get_config() != profile) {
        furi_hal_usb_unlock();
        if(!app_apply_profile(relay, profile_index, selected_profile_index)) return false;
    } else {
        *selected_profile_index = profile_index;
        relay->usb_configured = true;
    }

    furi_hal_hid_vendor_set_callback(usb_event_callback, relay);
    bt_set_raw_serial_callback(ble_raw_serial_callback, relay);
    return true;
}

bool airbridge_relay_restore_usb(AirbridgeRelay* relay) {
    if(!relay->usb_configured) return true;

    bt_set_raw_serial_callback(NULL, NULL);
    furi_hal_hid_vendor_set_callback(NULL, NULL);
    bool restored = furi_hal_usb_set_config_async(relay->usb_mode_prev, NULL);
    relay->usb_configured = false;
    return restored;
}

void airbridge_relay_init(AirbridgeRelay* relay) {
    relay->event_queue = furi_message_queue_alloc(8, sizeof(BridgeEvent));
}

void airbridge_relay_deinit(AirbridgeRelay* relay) {
    furi_message_queue_free(relay->event_queue);
}

void airbridge_relay_wake(AirbridgeRelay* relay) {
    BridgeEvent event = {.type = EVENT_TYPE_WAKE};
    (void)furi_message_queue_put(relay->event_queue, &event, 0);
}

FuriStatus airbridge_relay_poll(
    AirbridgeRelay* relay,
    BridgeEvent* event,
    uint32_t timeout) {
    return furi_message_queue_get(relay->event_queue, event, timeout);
}

void airbridge_relay_set_usb_connected(AirbridgeRelay* relay, bool connected) {
    FURI_CRITICAL_ENTER();
    relay->metrics.usb_connected = connected;
    FURI_CRITICAL_EXIT();
}

AirbridgeRelayResult airbridge_relay_handle(
    AirbridgeRelay* relay,
    BridgeEvent* be,
    AirbridgeScreen screen) {
    const bool deploy_request = be->to_ble && (be->len == 1) && (be->data[0] == 0x42);
    if(deploy_request && screen == AirbridgeScreenWaiting) {
        return AirbridgeRelayDeployRequested;
    }
    if(deploy_request && screen != AirbridgeScreenBridge) {
        return AirbridgeRelayDeployNotArmed;
    }
    if(be->to_ble) {
        if(bt_serial_tx(be->data, be->len)) {
            airbridge_relay_increment(&relay->metrics.chunks_usb_to_ble);
        } else {
            airbridge_relay_increment(&relay->metrics.tx_errors);
        }
    } else {
        uint8_t report[HID_VENDOR_PACKET_LEN] = {0};
        memcpy(report, be->data, be->len);
        if(furi_hal_hid_vendor_send_response(report, HID_VENDOR_PACKET_LEN)) {
            airbridge_relay_increment(&relay->metrics.chunks_ble_to_usb);
        } else {
            airbridge_relay_increment(&relay->metrics.tx_errors);
        }
    }
    return AirbridgeRelayHandled;
}

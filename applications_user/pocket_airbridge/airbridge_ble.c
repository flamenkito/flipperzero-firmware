#include "airbridge_ble.h"
#include "airbridge_ble_state.h"

#include <furi.h>
#include <furi_hal_bt.h>

#include "airbridge_profile.h"

#include "airbridge_time.h"

#define TAG                             "AirBridge"
#define AIRBRIDGE_BLE_DETACH_TIMEOUT_MS (250U)

void airbridge_ble_set_hids_adv(AirbridgeBle* ble, bool enable) {
    if(!ble->ble_profile_installed) return;
    furi_hal_bt_set_adv_hids(enable);
}

bool airbridge_ble_ensure_serial_adv(AirbridgeBle* ble) {
    if(!ble->ble_profile_installed) return false;
    /* Owner-approved restored policy (2026-09-15, BLE Deploy restoration): HIDS
     * stays in the advertising packet for the app's whole lifetime so a BLE
     * deploy target discovers the keyboard in host BT settings without prompt
     * navigation; it is dropped only at teardown. Re-arm every call:
     * gap_set_adv_hids early-outs when unchanged, so this is free. A bonded
     * host HID daemon may claim the single BLE link before Web Bluetooth
     * subscribes to serial TX; the typing/stream layers (link-settle, pairing
     * hold, squatter kick) own that window. */
    furi_hal_bt_set_adv_hids(true);
    if(gap_get_state() == GapStateIdle) {
        furi_hal_bt_start_advertising();
    }
    /* Starting advertising only queues work. Keep the operation supervised
     * until the GAP worker completes it, including blocked interval refreshes. */
    GapState state;
    do {
        state = gap_get_state();
        if(state == GapStateStartingAdv || state == GapStateDisconnecting) furi_delay_ms(25);
    } while(state == GapStateStartingAdv || state == GapStateDisconnecting);
    return state == GapStateAdvFast || state == GapStateAdvLowPower || state == GapStateConnected;
}

static void app_ble_status_changed_callback(BtStatus status, void* context) {
    AirbridgeBle* ble = context;
    const bool connected = status == BtStatusConnected;
    FURI_CRITICAL_ENTER();
    if(connected != ble->published_connected) {
        ble->published_connected = connected;
        ble->published_tick = furi_get_tick();
        if(connected) ble->published_connect_count++;
        ble->published_sequence++;
    }
    FURI_CRITICAL_EXIT();
}

static void
    airbridge_ble_apply_connected(AirbridgeBle* ble, uint32_t tick, uint32_t generation_advance) {
    AirbridgeBleLinkStateRefs state = {
        .connected = &ble->ble_connected,
        .connected_since = &ble->ble_connected_since,
        .last_rx_tick = &ble->ble_last_rx_tick,
        .desync_since = &ble->ble_desync_since,
        .generation = &ble->link_generation,
    };
    if(airbridge_ble_link_apply_connected(state, tick, generation_advance)) {
        FURI_LOG_D(
            TAG, "BLE central connected (generation %lu)", (unsigned long)ble->link_generation);
    }
}

static void airbridge_ble_apply_disconnected(AirbridgeBle* ble) {
    if(ble->ble_connected) {
        FURI_LOG_D(
            TAG, "BLE central disconnected (generation %lu)", (unsigned long)ble->link_generation);
    }
    ble->ble_connected = false;
    ble->ble_last_rx_tick = 0;
    ble->ble_desync_since = 0;
    ble->restart_adv_pending = true;
}

void airbridge_ble_service_pending(AirbridgeBle* ble) {
    if(!ble->ble_profile_installed) return;

    bool published_connected;
    uint32_t published_tick;
    uint32_t published_sequence;
    uint32_t published_connect_count;
    FURI_CRITICAL_ENTER();
    published_connected = ble->published_connected;
    published_tick = ble->published_tick;
    published_sequence = ble->published_sequence;
    published_connect_count = ble->published_connect_count;
    FURI_CRITICAL_EXIT();

    if(published_sequence != ble->consumed_sequence) {
        uint32_t new_connections = published_connect_count - ble->consumed_connect_count;
        uint32_t credited_connections = MIN(new_connections, ble->implicit_connect_credit);
        uint32_t generation_advance = new_connections - credited_connections;
        ble->implicit_connect_credit -= credited_connections;
        ble->consumed_connect_count = published_connect_count;
        ble->consumed_sequence = published_sequence;

        const bool gap_connected = (gap_get_state() == GapStateConnected);
        if(published_connected && gap_connected) {
            airbridge_ble_apply_connected(ble, published_tick, generation_advance);
        } else if(!published_connected && !gap_connected) {
            airbridge_ble_apply_disconnected(ble);
        }
    }

    if(ble->restart_adv_pending) {
        ble->restart_adv_pending = false;
        if(!ble->ble_connected) airbridge_ble_ensure_serial_adv(ble);
    }
    /* GAP interval changes also run while a Deploy prompt is waiting for input. */
    airbridge_ble_bridge_adv_watchdog(ble);
}

void airbridge_ble_note_rx(AirbridgeBle* ble, uint32_t tick) {
    if(!ble->ble_profile_installed || !(gap_get_state() == GapStateConnected)) return;
    if(ble->ble_connected && airbridge_u32_before(tick, ble->ble_connected_since)) return;
    if(!ble->ble_connected) ble->implicit_connect_credit++;
    airbridge_ble_apply_connected(ble, tick, !ble->ble_connected);
    ble->ble_last_rx_tick = tick;
    ble->ble_desync_since = 0;
}

void airbridge_ble_force_reconnect(AirbridgeBle* ble) {
    if(!ble->ble_profile_installed || ble->bt == NULL) return;
    bt_disconnect(ble->bt);
    airbridge_ble_apply_disconnected(ble);
    airbridge_ble_ensure_serial_adv(ble);
}

void airbridge_ble_bridge_adv_watchdog(AirbridgeBle* ble) {
    /* GAP state via furi_hal_bt_is_active() is the ground truth;
     * furi_hal_bt_start_advertising() is Idle-gated, and with the bt.c
     * disconnect-status fix the stale-ble_connected wedge class is closed —
     * the watchdog must not depend on FAP bookkeeping. */
    if(!ble->ble_profile_installed) return;

    uint32_t now = furi_get_tick();
    if(now - ble->ble_bridge_last_watchdog_tick < BLE_BRIDGE_ADV_WATCHDOG_MS) return;
    ble->ble_bridge_last_watchdog_tick = now;

    airbridge_ble_ensure_serial_adv(ble);

    if(furi_hal_bt_is_active()) return;

    /* Bridge mode must never kick a central off the chat link. Require GAP idle
     * before the kick, and rely on furi_hal_bt_start_advertising()'s own
     * GapStateIdle check as a second guard. This recovers the long-idle/relaunch
     * case without disturbing active BLE relay traffic. */
    FURI_LOG_D(TAG, "BLE Bridge watchdog: restart adv from idle");
    furi_hal_bt_start_advertising();
}

void airbridge_ble_squatter_watchdog(AirbridgeBle* ble) {
    if(!ble->ble_profile_installed || ble->bt == NULL) return;
    uint32_t now = furi_get_tick();
    /* A 4 s fast kick for bonded squatters (round 6, keyed on
     * bt_pairing_in_progress) was tried and REJECTED on hardware: it evicted
     * clients mid-discovery before they could subscribe (two connect attempts
     * died at ~4 s). 15 s is the proven window for bond-free connect+subscribe;
     * live pairing ceremonies are exempted below via the window restart —
     * human code confirmation on two devices does not fit any fixed budget. */
    if(ble->ble_connected) {
        if(ble->bt != NULL && bt_pairing_in_progress(ble->bt)) {
            /* Human-confirmed numeric-comparison ceremonies (code shown on BOTH
             * devices) can exceed the kick window. Never evict mid-ceremony;
             * restart the window so discovery+subscribe gets a full budget
             * after it completes. Bonded HID-daemon squatters never pair, so
             * they keep the 15 s budget. */
            ble->ble_connected_since = now;
            return;
        }
        if(now - ble->ble_connected_since < BLE_SQUATTER_KICK_MS) return;
        FuriHalBleProfileBase* profile = bt_current_profile_acquire(ble->bt);
        const bool subscribed = airbridge_profile_subscribed(profile);
        if(profile) bt_current_profile_release(ble->bt);
        if(subscribed) return;
        FURI_LOG_W(
            TAG,
            "BLE squatter kick: unsubscribed link held %lu ms",
            (unsigned long)(now - ble->ble_connected_since));
    } else {
        /* Path B (desync self-heal): GAP is active (Connected, or stuck in
         * Disconnecting) but the FAP never saw the connect. Key on GAP-active
         * (firmware truth), not just Connected, so a stuck Disconnecting state
         * is also evicted. Held for the kick window, force a disconnect to
         * resync. */
        if(!furi_hal_bt_is_active()) {
            ble->ble_desync_since = 0;
            return;
        }
        if(ble->ble_desync_since == 0) {
            ble->ble_desync_since = now;
            return;
        }
        if(now - ble->ble_desync_since < BLE_SQUATTER_KICK_MS) return;
        FURI_LOG_W(
            TAG,
            "BLE desync kick: ghost link held %lu ms",
            (unsigned long)(now - ble->ble_desync_since));
    }
    /* With honest GAP state, bt_disconnect blocks until the link is truly down,
     * so the follow-up start_advertising always starts clean advertising; a
     * re-grab by the daemon afterwards is a fresh, correctly-signaled
     * connection. */
    airbridge_ble_force_reconnect(ble);
}

bool airbridge_ble_configure(
    AirbridgeBle* ble,
    const AirbridgeBleIdentityParams* identity,
    AirbridgeSerialServiceEventCallback callback,
    void* context) {
    if(ble->ble_profile_installed) return true;

    ble->bt = furi_record_open(RECORD_BT);
    ble->profile_params = (AirbridgeBleProfileParams){
        .identity = *identity,
        .callback = callback,
        .context = context,
    };
    ble->ble_profile_installed =
        bt_profile_start(ble->bt, ble_profile_airbridge, &ble->profile_params) != NULL;
    if(ble->ble_profile_installed) {
        bt_set_status_changed_callback_with_snapshot(
            ble->bt, app_ble_status_changed_callback, ble);
        if(!airbridge_ble_ensure_serial_adv(ble)) return false;
    }
    return ble->ble_profile_installed;
}

bool airbridge_ble_send(AirbridgeBle* ble, uint8_t* data, uint16_t len) {
    if(!ble->bt || !ble->ble_profile_installed) return false;
    FuriHalBleProfileBase* profile = bt_current_profile_acquire(ble->bt);
    const bool sent = airbridge_profile_send(profile, data, len);
    if(profile) bt_current_profile_release(ble->bt);
    return sent;
}

bool airbridge_ble_kb_report(AirbridgeBle* ble, uint16_t key) {
    if(!ble->bt || !ble->ble_profile_installed) return false;
    uint8_t report[8] =
        {(uint8_t)(key >> 8), 0, (uint8_t)(key & 0xFF), 0, 0, 0, 0, 0};
    FuriHalBleProfileBase* profile = bt_current_profile_acquire(ble->bt);
    const bool sent = airbridge_profile_kb_report(profile, report, sizeof(report));
    if(profile) bt_current_profile_release(ble->bt);
    return sent;
}

bool airbridge_ble_kb_release(AirbridgeBle* ble) {
    return airbridge_ble_kb_report(ble, 0);
}

bool airbridge_ble_restore(AirbridgeBle* ble) {
    if(ble->bt == NULL) {
        ble->ble_profile_installed = false;
        return true;
    }

    Bt* bt = ble->bt;
    if(!bt_set_status_changed_callback_bounded(bt, NULL, NULL, AIRBRIDGE_BLE_DETACH_TIMEOUT_MS)) {
        return false;
    }
    if(!bt_profile_restore_default(bt)) return false;
    airbridge_ble_apply_disconnected(ble);

    ble->ble_profile_installed = false;
    furi_record_close(RECORD_BT);
    ble->bt = NULL;
    return true;
}

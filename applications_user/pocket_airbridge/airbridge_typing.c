#include "airbridge_typing.h"

#include <stdio.h>
#include <stdlib.h>

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb_hid.h>

#include <bt/bt_service/bt.h>

#include "airbridge_time.h"

#define TAG "AirBridge"

void airbridge_typing_init(
    AirbridgeTyping* typing,
    Storage* storage,
    AirbridgeBle* ble,
    AirbridgeScreen* screen,
    uint32_t* stream_started_tick,
    uint32_t* done_since,
    AirbridgeTypingServiceInput service_input,
    AirbridgeTypingShowError show_error,
    AirbridgeApp* app) {
    typing->io_file = storage_file_alloc(storage);
    typing->ble = ble;
    typing->screen = screen;
    typing->stream_started_tick = stream_started_tick;
    typing->done_since = done_since;
    typing->service_input = service_input;
    typing->show_error = show_error;
    typing->app = app;
}

void airbridge_typing_release_file(AirbridgeTyping* typing) {
    storage_file_free(typing->io_file);
}

void airbridge_typing_deinit(AirbridgeTyping* typing) {
    free(typing->bootstrap);
}

static bool airbridge_typing_ble_kb_report_with_retry(
    AirbridgeTyping* typing,
    uint8_t* report) {
    const uint32_t generation = typing->generation;
    uint8_t failures = 0;
    for(uint8_t attempt = 0; attempt < BLE_TYPING_RETRY_MAX; attempt++) {
        /* Service input at every retry boundary so a queued BACK aborts the
         * in-flight keystroke promptly instead of waiting out all retries. */
        typing->service_input(typing->app);
        if(*typing->screen != AirbridgeScreenTyping || generation != typing->generation) {
            return false;
        }
        /* Re-check immediately before the send: a report from an aborted
         * generation must never reach the host — queued BACK -> RIGHT -> OK
         * can start a NEW typing session that reuses AirbridgeScreenTyping. */
        if(*typing->screen != AirbridgeScreenTyping || generation != typing->generation) {
            return false;
        }
        bool error = bt_airbridge_kb_report(typing->ble->bt, report, 8);
        if(!error) {
            if(failures > 0) {
                FURI_LOG_W(TAG, "BLE kb report succeeded after %u retries", failures);
            }
            return true;
        }
        failures++;
        FURI_LOG_W(TAG, "BLE kb report failed (attempt %u/%u)", attempt + 1, BLE_TYPING_RETRY_MAX);
        if(attempt + 1 < BLE_TYPING_RETRY_MAX) furi_delay_ms(BLE_TYPING_RETRY_DELAY_MS);
    }
    FURI_LOG_E(TAG, "BLE kb report exhausted %u retries", failures);
    return false;
}

bool airbridge_typing_abort(AirbridgeTyping* typing) {
    /* Invalidate any in-flight retry helper from the aborted session: its next
     * boundary check sees the generation bump and returns without sending. */
    typing->generation++;
    const bool key_was_down = typing->key_down;
    typing->key_down = false;
    typing->enter_pending = false;
    typing->enter_done = false;
    bool release_ok = true;
    if(typing->transport == AirbridgeTypingTransportBle) {
        /* Release-all is DEFERRED to the main loop: the old synchronous send
         * through the retry wrapper could block the abort path for ~5 s under
         * gatt congestion. The flag is latched here, while the aborted
         * session's transport is known — the loop services it through Bt using
         * latched BLE state, never mutable typing_transport. This function must stay
         * free of report calls so abort latency never depends on gatt. */
        if(key_was_down) {
            typing->release_all_pending = true;
        }
    } else if(key_was_down) {
        /* USB release-all stays synchronous: ISR-driven, non-blocking. */
        release_ok = furi_hal_hid_airbridge_kb_release_all();
        if(!release_ok) {
            FURI_LOG_E(TAG, "release-all FAILED - tap a key on target");
        }
    }
    return release_ok;
}

static uint32_t airbridge_typing_jitter_ms(AirbridgeTyping* typing) {
    typing->jitter_state =
        typing->jitter_state * 1103515245U + 12345U + (uint32_t)typing->position;
    return (typing->jitter_state >> 16U) % (TYPE_JITTER_MAX_MS + 1U);
}

static bool airbridge_typing_load_bootstrap(AirbridgeTyping* typing) {
    const char* filename =
        typing->transport == AirbridgeTypingTransportBle ? "bootstrap-ble.js" : "bootstrap.js";
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_APP_DATA_PATH_PREFIX, filename);

    if(!storage_file_open(typing->io_file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(typing->io_file);
        typing->show_error(
            typing->app,
            typing->transport == AirbridgeTypingTransportBle ? "NO bootstrap-ble.js ON SD" :
                                                               "NO bootstrap.js ON SD");
        return false;
    }

    uint64_t file_size = storage_file_size(typing->io_file);
    if(file_size == 0) {
        storage_file_close(typing->io_file);
        typing->show_error(
            typing->app,
            typing->transport == AirbridgeTypingTransportBle ? "EMPTY bootstrap-ble.js" :
                                                               "EMPTY bootstrap.js");
        return false;
    }
    if(file_size > BOOTSTRAP_MAX_SIZE) {
        storage_file_close(typing->io_file);
        typing->show_error(
            typing->app,
            typing->transport == AirbridgeTypingTransportBle ? "bootstrap-ble.js TOO LARGE" :
                                                               "bootstrap.js TOO LARGE");
        return false;
    }

    if(typing->bootstrap) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
    }
    typing->bootstrap = malloc(file_size + 1);
    if(typing->bootstrap == NULL) {
        storage_file_close(typing->io_file);
        typing->show_error(
            typing->app,
            typing->transport == AirbridgeTypingTransportBle ? "bootstrap-ble.js MALLOC ERR" :
                                                               "bootstrap.js MALLOC ERR");
        return false;
    }

    typing->bootstrap_len = storage_file_read(typing->io_file, typing->bootstrap, file_size);
    storage_file_close(typing->io_file);
    if(typing->bootstrap_len != file_size) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
        typing->show_error(
            typing->app,
            typing->transport == AirbridgeTypingTransportBle ? "bootstrap-ble.js READ ERROR" :
                                                               "bootstrap.js READ ERROR");
        return false;
    }
    typing->bootstrap[typing->bootstrap_len] = '\0';

    for(size_t offset = 0; offset < typing->bootstrap_len; offset++) {
        const uint8_t value = (uint8_t)typing->bootstrap[offset];
        if(value < 0x20 || value > 0x7E) {
            char message[32];
            snprintf(
                message,
                sizeof(message),
                "BYTE 0x%02X AT OFFSET %lu",
                value,
                (unsigned long)offset);
            free(typing->bootstrap);
            typing->bootstrap = NULL;
            typing->bootstrap_len = 0;
            typing->show_error(typing->app, message);
            return false;
        }
    }

    return true;
}

void airbridge_typing_start(AirbridgeTyping* typing, AirbridgeTypingTransport transport) {
    typing->generation++;
    typing->transport = transport;
    if(!airbridge_typing_load_bootstrap(typing)) return;
    typing->position = 0;
    typing->key = HID_KEYBOARD_NONE;
    typing->key_down = false;
    typing->enter_pending = false;
    typing->enter_done = false;
    typing->jitter_state =
        furi_get_tick() ^ ((uint32_t)typing->bootstrap_len << 16U) ^ typing->generation;
    if(typing->jitter_state == 0) {
        typing->jitter_state = 0xA53C5A5AU;
    }
    typing->next_tick = furi_get_tick();
    typing->link_ready_tick = 0;
    typing->disconnected_since = 0;
    /* New deploy session: invalidate any stale stream stamp so a later Done
     * screen never zombie-kicks against a previous session's timeline. */
    *typing->stream_started_tick = 0;
    *typing->done_since = 0;
    *typing->screen = AirbridgeScreenTyping;
}

static bool airbridge_typing_press(AirbridgeTyping* typing, uint16_t key) {
    if(typing->transport == AirbridgeTypingTransportBle) {
        if(!typing->ble->ble_profile_installed) return false;
        uint8_t report[8] = {
            key >> 8,
            0,
            key & 0xFF,
            0,
            0,
            0,
            0,
            0,
        };
        return airbridge_typing_ble_kb_report_with_retry(typing, report);
    }
    return furi_hal_hid_airbridge_kb_press(key);
}

static bool airbridge_typing_release(AirbridgeTyping* typing, uint16_t key) {
    if(typing->transport == AirbridgeTypingTransportBle) {
        if(!typing->ble->ble_profile_installed) return false;
        uint8_t report[8] = {0};
        return airbridge_typing_ble_kb_report_with_retry(typing, report);
    }
    return furi_hal_hid_airbridge_kb_release(key);
}

void airbridge_typing_step(AirbridgeTyping* typing) {
    /* BLE typing holds until the host link is up AND the pairing ceremony has
     * completed. Burning keystrokes into the retry budget before the bonded
     * link exists would error out the session. Input stays serviced in the
     * main loop, so BACK still aborts instantly. */
    if(typing->transport == AirbridgeTypingTransportBle) {
        const uint32_t now = furi_get_tick();
        if(!typing->ble->ble_connected) {
            typing->link_ready_tick = 0;
            typing->next_tick = now;
            if(typing->disconnected_since == 0) {
                typing->disconnected_since = now == 0 ? 1 : now;
            } else if(now - typing->disconnected_since >= BLE_TYPING_DISCONNECT_TIMEOUT_MS) {
                (void)airbridge_typing_abort(typing);
                typing->show_error(typing->app, "BLE LINK TIMEOUT");
            }
            return;
        }
        typing->disconnected_since = 0;
        if(bt_pairing_in_progress(typing->ble->bt)) {
            typing->link_ready_tick = 0;
            typing->next_tick = now;
            return;
        }
        if(typing->link_ready_tick == 0) {
            typing->link_ready_tick = furi_get_tick();
        }
        if(furi_get_tick() - typing->link_ready_tick < BLE_TYPE_LINK_SETTLE_MS) {
            typing->next_tick = furi_get_tick();
            return;
        }
    }
    if(!airbridge_tick_reached(furi_get_tick(), typing->next_tick)) return;

    const uint32_t press_delay = typing->transport == AirbridgeTypingTransportBle ?
                                     BLE_TYPE_PRESS_DELAY_MS :
                                     TYPE_PRESS_DELAY_MS;
    const uint32_t release_delay = typing->transport == AirbridgeTypingTransportBle ?
                                       BLE_TYPE_RELEASE_DELAY_MS :
                                       TYPE_RELEASE_DELAY_MS;

    if(typing->key_down) {
        const uint32_t step_generation = typing->generation;
        if(!airbridge_typing_release(typing, typing->key)) {
            /* A false return caused by user abort or by queued input starting a
             * NEW generation must neither error nor abort the new session — so
             * this staleness check runs BEFORE app_abort_typing. */
            if(*typing->screen != AirbridgeScreenTyping || typing->generation != step_generation) {
                return;
            }
            airbridge_typing_abort(typing);
            typing->show_error(typing->app, "KEYBOARD SEND ERROR");
            return;
        }
        typing->key_down = false;
        if(typing->enter_pending) {
            typing->enter_pending = false;
            typing->enter_done = true;
        } else {
            typing->position++;
        }
        uint32_t actual_release_delay = release_delay;
        if(typing->transport == AirbridgeTypingTransportBle && (typing->key >> 8)) {
            actual_release_delay += BLE_TYPE_MODIFIED_SETTLE_MS;
        }
        actual_release_delay += airbridge_typing_jitter_ms(typing);
        typing->next_tick = furi_get_tick() + actual_release_delay;
        return;
    }

    if(typing->position < typing->bootstrap_len) {
        uint16_t key = HID_ASCII_TO_KEY(typing->bootstrap[typing->position]);
        if(key == HID_KEYBOARD_NONE) {
            typing->show_error(typing->app, "bootstrap.js NOT US ASCII");
            return;
        }
        const uint32_t step_generation = typing->generation;
        if(!airbridge_typing_press(typing, key)) {
            /* Stale-step false return (abort / generation change): no error
             * screen for the session that superseded us. */
            if(*typing->screen != AirbridgeScreenTyping || typing->generation != step_generation) {
                return;
            }
            typing->show_error(typing->app, "KEYBOARD SEND ERROR");
            return;
        }
        typing->key = key;
        typing->key_down = true;
        typing->next_tick =
            furi_get_tick() + press_delay + airbridge_typing_jitter_ms(typing);
        return;
    }

    if(!typing->enter_done) {
        const uint32_t step_generation = typing->generation;
        if(!airbridge_typing_press(typing, HID_KEYBOARD_RETURN)) {
            /* Stale-step false return (abort / generation change): no error
             * screen for the session that superseded us. */
            if(*typing->screen != AirbridgeScreenTyping || typing->generation != step_generation) {
                return;
            }
            typing->show_error(typing->app, "KEYBOARD SEND ERROR");
            return;
        }
        typing->key = HID_KEYBOARD_RETURN;
        typing->key_down = true;
        typing->enter_pending = true;
        typing->next_tick =
            furi_get_tick() + press_delay + airbridge_typing_jitter_ms(typing);
        return;
    }

    *typing->screen = AirbridgeScreenWaiting;
    if(typing->transport == AirbridgeTypingTransportBle) {
        typing->ble->ble_waiting_last_pump_tick = furi_get_tick();
        /* NO bond wipe here (round-8 wiped; reverted): with ONE identity the
         * typing bond doubles as the browser's data bond — wiping the
         * Flipper-side keys leaves the Mac's bond record pointing at dead
         * keys, and every subsequent connect fails with "Connection attempt
         * failed" (hardware-proven). The daemon's squatting is handled by the
         * squatter watchdog instead. */
        airbridge_ble_force_reconnect(typing->ble);
    }
}

bool airbridge_typing_service_release(AirbridgeTyping* typing) {
    if(!typing->release_all_pending) return true;

    uint8_t report[8] = {0};
    if(typing->ble->ble_profile_installed &&
       !bt_airbridge_kb_report(typing->ble->bt, report, 8)) {
        typing->release_all_pending = false;
        typing->release_all_attempts = 0;
        return true;
    }

    typing->release_all_attempts++;
    if(typing->release_all_attempts >= 3) {
        typing->release_all_pending = false;
        typing->release_all_attempts = 0;
        FURI_LOG_E(TAG, "release-all FAILED - tap a key on target");
        typing->show_error(typing->app, "STUCK KEY - TAP A KEY");
    }
    return false;
}

void airbridge_typing_drain_release(AirbridgeTyping* typing) {
    if(typing->release_all_pending && typing->ble->ble_profile_installed) {
        uint8_t report[8] = {0};
        for(uint8_t attempt = 0; attempt < 3; attempt++) {
            if(!bt_airbridge_kb_report(typing->ble->bt, report, 8)) break;
            if(attempt + 1 < 3) furi_delay_ms(20);
        }
        typing->release_all_pending = false;
    }
}

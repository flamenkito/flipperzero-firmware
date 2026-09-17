#include "airbridge_typing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <furi.h>
#include <furi_hal_usb_hid.h>

#include <bt/bt_service/bt.h>

#include "airbridge_assets.h"
#include "airbridge_assets_digest.h"
#include "airbridge_time.h"
#include "airbridge_passwords.h"

#define TAG "AirBridge"

static void airbridge_typing_clear_password(AirbridgeTyping* typing) {
    if(!typing->password) return;
    if(typing->bootstrap) {
        volatile char* bytes = typing->bootstrap;
        for(size_t i = 0; i < typing->bootstrap_len; i++)
            bytes[i] = 0;
        free(typing->bootstrap);
    }
    typing->bootstrap = NULL;
    typing->bootstrap_len = 0;
    typing->password = false;
}

void airbridge_typing_init(
    AirbridgeTyping* typing,
    Storage* storage,
    AirbridgeBle* ble,
    AirbridgeTypingShowError show_error,
    void* error_context) {
    typing->io_file = storage_file_alloc(storage);
    typing->ble = ble;
    typing->show_error = show_error;
    typing->error_context = error_context;
    typing->transport = AirbridgeTypingTransportUsb;
}

void airbridge_typing_release_file(AirbridgeTyping* typing) {
    storage_file_free(typing->io_file);
}

void airbridge_typing_deinit(AirbridgeTyping* typing) {
    airbridge_typing_clear_password(typing);
    free(typing->bootstrap);
}

static bool airbridge_typing_ble_kb_report_with_retry(AirbridgeTyping* typing, uint16_t key) {
    const uint32_t generation = typing->generation;
    for(uint8_t attempt = 0; attempt < BLE_TYPING_RETRY_MAX; attempt++) {
        /* An abort (BACK, error path, teardown) bumps the generation: a report
         * from the aborted session must never reach the host. */
        if(generation != typing->generation) {
            return false;
        }
        if(airbridge_ble_kb_report(typing->ble, key)) {
            if(attempt > 0) {
                FURI_LOG_W(TAG, "BLE kb report succeeded after %u retries", attempt);
            }
            return true;
        }
        FURI_LOG_W(TAG, "BLE kb report failed (attempt %u/%u)", attempt + 1, BLE_TYPING_RETRY_MAX);
        if(attempt + 1 < BLE_TYPING_RETRY_MAX) furi_delay_ms(BLE_TYPING_RETRY_DELAY_MS);
    }
    FURI_LOG_E(TAG, "BLE kb report exhausted retries");
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
        /* Release-all is DEFERRED to the main loop: a synchronous send here
         * could block the abort path on GATT congestion. The flag is latched
         * while the aborted session's transport is known; the loop services it
         * through one direct attempt per iteration, never the retry wrapper. */
        if(key_was_down) {
            typing->release_all_pending = true;
        }
    } else if(key_was_down) {
        /* USB release-all stays synchronous: ISR-driven, non-blocking. */
        release_ok = airbridge_usb_kb_release_all();
        if(!release_ok) {
            FURI_LOG_E(TAG, "release-all FAILED - tap a key on target");
        }
    }
    airbridge_typing_clear_password(typing);
    return release_ok;
}

bool airbridge_typing_service_release(AirbridgeTyping* typing) {
    if(!typing->release_all_pending) return true;

    if(typing->ble->ble_profile_installed &&
       airbridge_ble_kb_report(typing->ble, HID_KEYBOARD_NONE)) {
        typing->release_all_pending = false;
        typing->release_all_attempts = 0;
        return true;
    }

    typing->release_all_attempts++;
    if(typing->release_all_attempts >= 3) {
        typing->release_all_pending = false;
        typing->release_all_attempts = 0;
        FURI_LOG_E(TAG, "release-all FAILED - tap a key on target");
        typing->show_error(typing->error_context, "STUCK KEY - TAP A KEY");
    }
    return false;
}

static uint32_t airbridge_typing_jitter_ms(AirbridgeTyping* typing) {
    typing->jitter_state =
        typing->jitter_state * 1103515245U + 12345U + (uint32_t)typing->position;
    return (typing->jitter_state >> 16U) % (TYPE_JITTER_MAX_MS + 1U);
}

static bool airbridge_typing_load_bootstrap(AirbridgeTyping* typing) {
    const bool use_ble = typing->transport == AirbridgeTypingTransportBle;
    const char* filename = use_ble ? "bootstrap-ble.js" : "bootstrap.js";
    const char* missing_error = use_ble ? "NO bootstrap-ble.js ON SD" : "NO bootstrap.js ON SD";
    const char* empty_error = use_ble ? "EMPTY bootstrap-ble.js" : "EMPTY bootstrap.js";
    const char* large_error = use_ble ? "bootstrap-ble.js TOO LARGE" : "bootstrap.js TOO LARGE";
    const char* malloc_error = use_ble ? "bootstrap-ble.js MALLOC ERR" : "bootstrap.js MALLOC ERR";
    const char* read_error = use_ble ? "bootstrap-ble.js READ ERROR" : "bootstrap.js READ ERROR";
    const char* hash_error = use_ble ? "bootstrap-ble.js HASH MISMATCH" :
                                       "bootstrap.js HASH MISMATCH";
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_APP_DATA_PATH_PREFIX, filename);

    if(!storage_file_open(typing->io_file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, missing_error);
        return false;
    }

    uint64_t file_size = storage_file_size(typing->io_file);
    if(file_size == 0) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, empty_error);
        return false;
    }
    if(file_size > BOOTSTRAP_MAX_SIZE) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, large_error);
        return false;
    }

    if(typing->bootstrap) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
    }
    typing->bootstrap = malloc(file_size + 1);
    if(typing->bootstrap == NULL) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, malloc_error);
        return false;
    }

    typing->bootstrap_len = storage_file_read(typing->io_file, typing->bootstrap, file_size);
    storage_file_close(typing->io_file);
    if(typing->bootstrap_len != file_size) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
        typing->show_error(typing->error_context, read_error);
        return false;
    }
    typing->bootstrap[typing->bootstrap_len] = '\0';

    AirbridgeSha256 sha256;
    uint8_t digest[AIRBRIDGE_ASSET_SHA256_SIZE];
    airbridge_sha256_init(&sha256);
    airbridge_sha256_update(&sha256, (const uint8_t*)typing->bootstrap, typing->bootstrap_len);
    airbridge_sha256_final(&sha256, digest);
    const uint8_t* expected = use_ble ? AIRBRIDGE_BOOTSTRAP_BLE_SHA256 :
                                        AIRBRIDGE_BOOTSTRAP_SHA256;
    if(!airbridge_digest_matches(digest, expected)) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
        typing->bootstrap_len = 0;
        typing->show_error(typing->error_context, hash_error);
        return false;
    }

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
            typing->show_error(typing->error_context, message);
            return false;
        }
    }

    return true;
}

bool airbridge_typing_start(AirbridgeTyping* typing, AirbridgeTypingTransport transport) {
    airbridge_typing_clear_password(typing);
    typing->generation++;
    typing->transport = transport;
    typing->release_all_pending = false;
    typing->release_all_attempts = 0;
    if(!airbridge_typing_load_bootstrap(typing)) return false;
    typing->position = 0;
    typing->key = HID_KEYBOARD_NONE;
    typing->key_down = false;
    typing->enter_pending = false;
    typing->enter_done = false;
    typing->jitter_state = furi_get_tick() ^ ((uint32_t)typing->bootstrap_len << 16U) ^
                           typing->generation;
    if(typing->jitter_state == 0) {
        typing->jitter_state = 0xA53C5A5AU;
    }
    typing->next_tick = furi_get_tick();
    typing->link_ready_tick = 0;
    typing->disconnected_since = 0;
    return true;
}

bool airbridge_typing_password_start(AirbridgeTyping* typing, const char* value) {
    const size_t length = strlen(value);
    if(!length || length > AIRBRIDGE_PASSWORD_SIZE || typing->key_down ||
       typing->release_all_pending || !airbridge_usb_vendor_is_connected())
        return false;
    for(size_t i = 0; i < length; i++)
        if((uint8_t)value[i] < 0x20 || (uint8_t)value[i] > 0x7e) return false;
    char* copy = malloc(length + 1);
    if(!copy) return false;
    memcpy(copy, value, length + 1);
    airbridge_typing_clear_password(typing);
    free(typing->bootstrap);
    typing->bootstrap = copy;
    typing->bootstrap_len = length;
    typing->password = true;
    typing->generation++;
    typing->transport = AirbridgeTypingTransportUsb;
    typing->position = 0;
    typing->key = HID_KEYBOARD_NONE;
    typing->key_down = false;
    typing->enter_pending = false;
    typing->enter_done = true;
    typing->jitter_state = furi_get_tick();
    typing->next_tick = furi_get_tick();
    return true;
}

static bool airbridge_typing_press(AirbridgeTyping* typing, uint16_t key) {
    if(typing->transport == AirbridgeTypingTransportBle) {
        if(!typing->ble->ble_profile_installed) return false;
        return airbridge_typing_ble_kb_report_with_retry(typing, key);
    }
    return airbridge_usb_kb_press(key);
}

static bool airbridge_typing_release(AirbridgeTyping* typing, uint16_t key) {
    if(typing->transport == AirbridgeTypingTransportBle) {
        if(!typing->ble->ble_profile_installed) return false;
        UNUSED(key);
        return airbridge_typing_ble_kb_report_with_retry(typing, HID_KEYBOARD_NONE);
    }
    return airbridge_usb_kb_release(key);
}

static void airbridge_typing_finish_key(AirbridgeTyping* typing) {
    /* Never carry a held key into a potentially blocking BLE operation. Issue
     * key-up after the 12-19 ms press delay on this same worker. */
    furi_delay_ms(TYPE_PRESS_DELAY_MS + airbridge_typing_jitter_ms(typing));
    if(!airbridge_typing_release(typing, typing->key)) {
        airbridge_typing_abort(typing);
        typing->show_error(typing->error_context, "KEYBOARD SEND ERROR");
        return;
    }
    typing->key_down = false;
    if(typing->enter_pending) {
        typing->enter_pending = false;
        typing->enter_done = true;
    } else {
        typing->position++;
    }
    typing->next_tick =
        furi_get_tick() + TYPE_RELEASE_DELAY_MS + airbridge_typing_jitter_ms(typing);
}

bool airbridge_typing_step(AirbridgeTyping* typing) {
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
                typing->show_error(typing->error_context, "BLE LINK TIMEOUT");
            }
            return false;
        }
        typing->disconnected_since = 0;
        if(typing->ble->bt != NULL && bt_pairing_in_progress(typing->ble->bt)) {
            typing->link_ready_tick = 0;
            typing->next_tick = now;
            return false;
        }
        if(typing->link_ready_tick == 0) {
            typing->link_ready_tick = furi_get_tick();
        }
        if(furi_get_tick() - typing->link_ready_tick < BLE_TYPE_LINK_SETTLE_MS) {
            typing->next_tick = furi_get_tick();
            return false;
        }
    }
    if(!airbridge_tick_reached(furi_get_tick(), typing->next_tick)) return false;

    /* A cross-thread abort (teardown) mid-step must not paint an error for the
     * session that superseded us. */
    const uint32_t step_generation = typing->generation;

    if(typing->position < typing->bootstrap_len) {
        uint16_t key = HID_ASCII_TO_KEY(typing->bootstrap[typing->position]);
        if(key == HID_KEYBOARD_NONE) {
            typing->show_error(typing->error_context, "bootstrap.js NOT US ASCII");
            return false;
        }
        if(!airbridge_typing_press(typing, key)) {
            if(typing->generation != step_generation) return false;
            typing->show_error(typing->error_context, "KEYBOARD SEND ERROR");
            return false;
        }
        typing->key = key;
        typing->key_down = true;
        airbridge_typing_finish_key(typing);
        return false;
    }

    if(!typing->enter_done) {
        if(!airbridge_typing_press(typing, HID_KEYBOARD_RETURN)) {
            if(typing->generation != step_generation) return false;
            typing->show_error(typing->error_context, "KEYBOARD SEND ERROR");
            return false;
        }
        typing->key = HID_KEYBOARD_RETURN;
        typing->key_down = true;
        typing->enter_pending = true;
        airbridge_typing_finish_key(typing);
        return false;
    }

    airbridge_typing_clear_password(typing);
    return true;
}

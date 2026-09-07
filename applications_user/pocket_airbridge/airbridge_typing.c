#include "airbridge_typing.h"

#include <stdio.h>
#include <stdlib.h>

#include <furi.h>
#include <furi_hal_usb_hid.h>

#include "airbridge_assets.h"
#include "airbridge_assets_digest.h"
#include "airbridge_time.h"

#define TAG "AirBridge"

void airbridge_typing_init(
    AirbridgeTyping* typing,
    Storage* storage,
    AirbridgeTypingShowError show_error,
    void* error_context) {
    typing->io_file = storage_file_alloc(storage);
    typing->show_error = show_error;
    typing->error_context = error_context;
}

void airbridge_typing_release_file(AirbridgeTyping* typing) {
    storage_file_free(typing->io_file);
}

void airbridge_typing_deinit(AirbridgeTyping* typing) {
    free(typing->bootstrap);
}

bool airbridge_typing_abort(AirbridgeTyping* typing) {
    const bool key_was_down = typing->key_down;
    typing->key_down = false;
    typing->enter_pending = false;
    typing->enter_done = false;
    if(key_was_down) {
        const bool release_ok = airbridge_usb_kb_release_all();
        if(!release_ok) {
            FURI_LOG_E(TAG, "release-all FAILED - tap a key on target");
        }
        return release_ok;
    }
    return true;
}

static uint32_t airbridge_typing_jitter_ms(AirbridgeTyping* typing) {
    typing->jitter_state =
        typing->jitter_state * 1103515245U + 12345U + (uint32_t)typing->position;
    return (typing->jitter_state >> 16U) % (TYPE_JITTER_MAX_MS + 1U);
}

static bool airbridge_typing_load_bootstrap(AirbridgeTyping* typing) {
    char path[64];
    snprintf(path, sizeof(path), "%s/bootstrap.js", STORAGE_APP_DATA_PATH_PREFIX);

    if(!storage_file_open(typing->io_file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, "NO bootstrap.js ON SD");
        return false;
    }

    uint64_t file_size = storage_file_size(typing->io_file);
    if(file_size == 0) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, "EMPTY bootstrap.js");
        return false;
    }
    if(file_size > BOOTSTRAP_MAX_SIZE) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, "bootstrap.js TOO LARGE");
        return false;
    }

    if(typing->bootstrap) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
    }
    typing->bootstrap = malloc(file_size + 1);
    if(typing->bootstrap == NULL) {
        storage_file_close(typing->io_file);
        typing->show_error(typing->error_context, "bootstrap.js MALLOC ERR");
        return false;
    }

    typing->bootstrap_len = storage_file_read(typing->io_file, typing->bootstrap, file_size);
    storage_file_close(typing->io_file);
    if(typing->bootstrap_len != file_size) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
        typing->show_error(typing->error_context, "bootstrap.js READ ERROR");
        return false;
    }
    typing->bootstrap[typing->bootstrap_len] = '\0';

    AirbridgeSha256 sha256;
    uint8_t digest[AIRBRIDGE_ASSET_SHA256_SIZE];
    airbridge_sha256_init(&sha256);
    airbridge_sha256_update(&sha256, (const uint8_t*)typing->bootstrap, typing->bootstrap_len);
    airbridge_sha256_final(&sha256, digest);
    if(!airbridge_digest_matches(digest, AIRBRIDGE_BOOTSTRAP_SHA256)) {
        free(typing->bootstrap);
        typing->bootstrap = NULL;
        typing->bootstrap_len = 0;
        typing->show_error(typing->error_context, "bootstrap.js HASH MISMATCH");
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

bool airbridge_typing_start(AirbridgeTyping* typing) {
    if(!airbridge_typing_load_bootstrap(typing)) return false;
    typing->position = 0;
    typing->key = HID_KEYBOARD_NONE;
    typing->key_down = false;
    typing->enter_pending = false;
    typing->enter_done = false;
    typing->jitter_state = furi_get_tick() ^ ((uint32_t)typing->bootstrap_len << 16U);
    if(typing->jitter_state == 0) {
        typing->jitter_state = 0xA53C5A5AU;
    }
    typing->next_tick = furi_get_tick();
    return true;
}

static bool airbridge_typing_press(AirbridgeTyping* typing, uint16_t key) {
    UNUSED(typing);
    return airbridge_usb_kb_press(key);
}

static bool airbridge_typing_release(AirbridgeTyping* typing, uint16_t key) {
    UNUSED(typing);
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
    if(!airbridge_tick_reached(furi_get_tick(), typing->next_tick)) return false;

    if(typing->position < typing->bootstrap_len) {
        uint16_t key = HID_ASCII_TO_KEY(typing->bootstrap[typing->position]);
        if(key == HID_KEYBOARD_NONE) {
            typing->show_error(typing->error_context, "bootstrap.js NOT US ASCII");
            return false;
        }
        if(!airbridge_typing_press(typing, key)) {
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
            typing->show_error(typing->error_context, "KEYBOARD SEND ERROR");
            return false;
        }
        typing->key = HID_KEYBOARD_RETURN;
        typing->key_down = true;
        typing->enter_pending = true;
        airbridge_typing_finish_key(typing);
        return false;
    }

    return true;
}

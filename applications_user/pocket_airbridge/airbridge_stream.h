#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <storage/storage.h>

#include "airbridge_types.h"

typedef struct AirbridgeApp AirbridgeApp;
typedef void (*AirbridgeStreamShowError)(AirbridgeApp* app, const char* message);

typedef struct {
    File* file;
    AirbridgeTypingTransport* transport;
    AirbridgeScreen* screen;
    AirbridgeApp* app;
    AirbridgeStreamShowError show_error;
    bool open;
    bool header_pending;
    uint64_t total_len;
    uint32_t checksum;
    uint64_t sent;
    uint8_t tx_strikes;
    uint8_t pending_chunk[HID_VENDOR_PACKET_LEN];
    size_t pending_len;
    /* Stamp of the last real 0x42 deploy stream start; zero means no stream
     * this session. Used by the Done-screen zombie-kick. */
    uint32_t started_tick;
    /* Tick of the last Done-screen entry. The zombie-kick waits
     * BLE_DONE_ZOMBIE_GRACE_MS past it: Done is entered when the final chunk
     * is QUEUED in the BLE stack, and an immediate bt_disconnect drops the
     * unsent tail (hardware: bootstrap saw [gattserverdisconnected] +
     * 'Transfer failed' right after a fully-sent stream). The grace lets
     * in-flight notifications flush; the bootstrap's own confirmed
     * disconnect (2500 ms) usually lands first anyway. */
    uint32_t done_since;
} AirbridgeStream;

void airbridge_stream_init(
    AirbridgeStream* stream,
    Storage* storage,
    AirbridgeTypingTransport* transport,
    AirbridgeScreen* screen,
    AirbridgeStreamShowError show_error,
    AirbridgeApp* app);
void airbridge_stream_deinit(AirbridgeStream* stream);
void airbridge_stream_close(AirbridgeStream* stream);
bool airbridge_stream_start(AirbridgeStream* stream);
void airbridge_stream_step_usb(AirbridgeStream* stream);
void airbridge_stream_step_ble(AirbridgeStream* stream);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <storage/storage.h>

#include "airbridge_ble.h"
#include "airbridge_types.h"

typedef void (*AirbridgeStreamShowError)(void* context, const char* message);

typedef struct {
    File* file;
    AirbridgeBle* ble;
    void* error_context;
    AirbridgeStreamShowError show_error;
    AirbridgeStreamTransport transport;
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
     * is queued, and an immediate disconnect could drop the unsent tail. */
    uint32_t done_since;
} AirbridgeStream;

void airbridge_stream_init(
    AirbridgeStream* stream,
    Storage* storage,
    AirbridgeBle* ble,
    AirbridgeStreamShowError show_error,
    void* error_context);
void airbridge_stream_deinit(AirbridgeStream* stream);
void airbridge_stream_close(AirbridgeStream* stream);
bool airbridge_stream_start(AirbridgeStream* stream, AirbridgeStreamTransport transport);
bool airbridge_stream_step_usb(AirbridgeStream* stream);
bool airbridge_stream_step_ble(AirbridgeStream* stream);

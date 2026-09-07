#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <storage/storage.h>

#include "airbridge_types.h"

typedef void (*AirbridgeStreamShowError)(void* context, const char* message);

typedef struct {
    File* file;
    void* error_context;
    AirbridgeStreamShowError show_error;
    bool open;
    bool header_pending;
    uint64_t total_len;
    uint32_t checksum;
    uint64_t sent;
} AirbridgeStream;

void airbridge_stream_init(
    AirbridgeStream* stream,
    Storage* storage,
    AirbridgeStreamShowError show_error,
    void* error_context);
void airbridge_stream_deinit(AirbridgeStream* stream);
void airbridge_stream_close(AirbridgeStream* stream);
bool airbridge_stream_start(AirbridgeStream* stream);
bool airbridge_stream_step_usb(AirbridgeStream* stream);

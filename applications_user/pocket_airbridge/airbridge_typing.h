#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <storage/storage.h>

#include "airbridge_types.h"

typedef void (*AirbridgeTypingShowError)(void* context, const char* message);

typedef struct {
    File* io_file;
    void* error_context;
    AirbridgeTypingShowError show_error;
    char* bootstrap;
    size_t bootstrap_len;
    size_t position;
    uint16_t key;
    bool key_down;
    bool enter_pending;
    bool enter_done;
    uint32_t next_tick;
    uint32_t jitter_state;
} AirbridgeTyping;

void airbridge_typing_init(
    AirbridgeTyping* typing,
    Storage* storage,
    AirbridgeTypingShowError show_error,
    void* error_context);
void airbridge_typing_release_file(AirbridgeTyping* typing);
void airbridge_typing_deinit(AirbridgeTyping* typing);
bool airbridge_typing_start(AirbridgeTyping* typing);
bool airbridge_typing_step(AirbridgeTyping* typing);
bool airbridge_typing_abort(AirbridgeTyping* typing);

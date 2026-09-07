#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <storage/storage.h>

#include "airbridge_profile.h"

typedef struct {
    uint8_t usb_profile_index;
    AirbridgeBleIdentityParams ble_identity;
    bool identity_warning;
} AirbridgeConfig;

void airbridge_config_set_defaults(AirbridgeConfig* config);
void airbridge_config_load(AirbridgeConfig* config, Storage* storage);

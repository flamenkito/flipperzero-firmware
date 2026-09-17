#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIRBRIDGE_BLE_PACKET_SIZE   192U
#define AIRBRIDGE_BLE_PACKET_COUNT  3U
#define AIRBRIDGE_BLE_PACKET_HEADER 16U

static inline bool airbridge_ble_packet_control(const uint8_t* data, size_t len) {
    if(len != 64 && len != 128 && len != AIRBRIDGE_BLE_PACKET_SIZE) return false;
    static const uint8_t prefix[] = {0xF0, 'A', 'B', 'P', 1};
    if(memcmp(data, prefix, sizeof(prefix)) || (data[6] != 2 && data[6] != 3) || data[7] != 0)
        return false;
    if(data[5] != (len > 64 ? 1 : 2)) return false;
    if(data[5] == 1 && len != data[6] * 64U) return false;
    for(size_t i = AIRBRIDGE_BLE_PACKET_HEADER; i < len; i++) {
        if(data[i] != 0xA5) return false;
    }
    return true;
}

#include "airbridge_config.h"

#include <furi.h>
#include "airbridge_usb.h"
#include <toolbox/stream/file_stream.h>
#include <toolbox/stream/stream.h>

#include "airbridge_types.h"

#define BLE_SCAN_RESPONSE_OVERHEAD 27U

static bool app_find_profile(const char* label, uint8_t* profile_index) {
    for(uint8_t index = 0; index < airbridge_usb_profile_count(); index++) {
        const char* candidate = airbridge_usb_profile_label(index);
        if(candidate != NULL && strcmp(label, candidate) == 0) {
            *profile_index = index;
            return true;
        }
    }
    return false;
}

static const uint8_t app_hp_ouis[][3] = {
    {0x3C, 0x52, 0x82},
    {0x48, 0x0F, 0xCF},
    {0x94, 0x57, 0xA5},
    {0x3C, 0xD9, 0x2B},
    {0xB4, 0xB6, 0x76},
    {0x2C, 0x44, 0xFD},
    {0xA0, 0xD3, 0xC1},
    {0x40, 0xB0, 0x34},
};

static bool app_config_key_matches(const char* key, size_t key_len, const char* expected) {
    return (strlen(expected) == key_len) && (strncmp(key, expected, key_len) == 0);
}

static int8_t app_hex_nibble(char value) {
    if(value >= '0' && value <= '9') return value - '0';
    if(value >= 'a' && value <= 'f') return value - 'a' + 10;
    if(value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool app_parse_hex_u16(const char* value, size_t value_len, uint16_t* result) {
    if((value_len >= 2) && (value[0] == '0') && ((value[1] == 'x') || (value[1] == 'X'))) {
        value += 2;
        value_len -= 2;
    }
    if((value_len == 0) || (value_len > 4)) return false;

    uint16_t parsed = 0;
    for(size_t index = 0; index < value_len; index++) {
        int8_t nibble = app_hex_nibble(value[index]);
        if(nibble < 0) return false;
        parsed = (parsed << 4) | nibble;
    }
    *result = parsed;
    return true;
}

static bool app_hp_oui_allowed(const uint8_t* mac_address) {
    for(size_t index = 0; index < COUNT_OF(app_hp_ouis); index++) {
        if(memcmp(mac_address, app_hp_ouis[index], sizeof(app_hp_ouis[index])) == 0) {
            return true;
        }
    }
    return false;
}

static bool app_parse_mac(const char* value, size_t value_len, uint8_t* mac_address) {
    if(value_len != 17) return false;

    for(size_t index = 0; index < AIRBRIDGE_BLE_MAC_ADDRESS_LEN; index++) {
        size_t offset = index * 3;
        int8_t high = app_hex_nibble(value[offset]);
        int8_t low = app_hex_nibble(value[offset + 1]);
        if((high < 0) || (low < 0)) return false;
        if((index < AIRBRIDGE_BLE_MAC_ADDRESS_LEN - 1) && (value[offset + 2] != ':')) {
            return false;
        }
        mac_address[index] = (high << 4) | low;
    }
    return app_hp_oui_allowed(mac_address);
}

static bool
    app_copy_config_value(char* target, size_t target_size, const char* value, size_t value_len) {
    if((value_len == 0) || (value_len >= target_size)) return false;
    memcpy(target, value, value_len);
    target[value_len] = '\0';
    return true;
}

static bool
    app_parse_mfg_hex(AirbridgeBleIdentityParams* identity, const char* value, size_t value_len) {
    if((value_len % 2) != 0) return false;

    size_t extra_len = value_len / 2;
    if(extra_len > sizeof(identity->manufacturer_data) - 2U) return false;

    for(size_t index = 0; index < extra_len; index++) {
        int8_t high = app_hex_nibble(value[index * 2]);
        int8_t low = app_hex_nibble(value[index * 2 + 1]);
        if((high < 0) || (low < 0)) return false;
        identity->manufacturer_data[index + 2] = (high << 4) | low;
    }
    identity->manufacturer_data_len = extra_len + 2U;
    return true;
}

static bool app_parse_config_line(AirbridgeConfig* config, const char* line) {
    const char* equals = strchr(line, '=');
    if(equals == NULL) return true;

    const char* key = line;
    while((*key == ' ') || (*key == '\t'))
        key++;
    const char* key_end = equals;
    while((key_end > key) && ((key_end[-1] == ' ') || (key_end[-1] == '\t')))
        key_end--;

    const char* value = equals + 1;
    while((*value == ' ') || (*value == '\t'))
        value++;
    const char* value_end = value;
    while((*value_end != '\0') && (*value_end != '\r') && (*value_end != '\n'))
        value_end++;
    while((value_end > value) && ((value_end[-1] == ' ') || (value_end[-1] == '\t'))) {
        value_end--;
    }

    size_t key_len = key_end - key;
    size_t value_len = value_end - value;
    if(app_config_key_matches(key, key_len, "profile")) {
        char label[32];
        if(!app_copy_config_value(label, sizeof(label), value, value_len)) return false;
        uint8_t profile_index;
        if(!app_find_profile(label, &profile_index)) return false;
        config->usb_profile_index = profile_index;
        return true;
    } else if(app_config_key_matches(key, key_len, "ble_name")) {
        return app_copy_config_value(
            config->ble_identity.device_name,
            sizeof(config->ble_identity.device_name),
            value,
            value_len);
    } else if(app_config_key_matches(key, key_len, "ble_mac")) {
        return app_parse_mac(value, value_len, config->ble_identity.mac_address);
    } else if(app_config_key_matches(key, key_len, "ble_appearance")) {
        return app_parse_hex_u16(value, value_len, &config->ble_identity.appearance);
    } else if(app_config_key_matches(key, key_len, "ble_mfg_company")) {
        uint16_t company;
        if(!app_parse_hex_u16(value, value_len, &company)) return false;
        config->ble_identity.manufacturer_data[0] = company & 0xFF;
        config->ble_identity.manufacturer_data[1] = company >> 8;
        return true;
    } else if(app_config_key_matches(key, key_len, "ble_mfg_hex")) {
        return app_parse_mfg_hex(&config->ble_identity, value, value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_mfr")) {
        return app_copy_config_value(
            config->ble_identity.dis_manufacturer,
            sizeof(config->ble_identity.dis_manufacturer),
            value,
            value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_model")) {
        return app_copy_config_value(
            config->ble_identity.dis_model,
            sizeof(config->ble_identity.dis_model),
            value,
            value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_serial")) {
        return app_copy_config_value(
            config->ble_identity.dis_serial,
            sizeof(config->ble_identity.dis_serial),
            value,
            value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_pnp")) {
        return app_parse_hex_u16(value, value_len, &config->ble_identity.dis_pnp_version);
    }
    return false;
}

void airbridge_config_load(AirbridgeConfig* config, Storage* storage) {
    AirbridgeConfig parsed;
    airbridge_config_set_defaults(&parsed);

    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    bool config_valid = true;
    if(file_stream_open(stream, APP_DATA_PATH("config"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(stream_size(stream) > CONFIG_MAX_SIZE) {
            config_valid = false;
        } else {
            while(stream_read_line(stream, line)) {
                if(!app_parse_config_line(&parsed, furi_string_get_cstr(line))) {
                    config_valid = false;
                }
            }
        }
    }
    file_stream_close(stream);
    furi_string_free(line);
    stream_free(stream);

    if(strlen(parsed.ble_identity.device_name) + parsed.ble_identity.manufacturer_data_len >
       BLE_SCAN_RESPONSE_OVERHEAD) {
        config_valid = false;
    }
    if(config_valid) {
        *config = parsed;
    } else {
        airbridge_config_set_defaults(config);
        config->identity_warning = true;
    }
}

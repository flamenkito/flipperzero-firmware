#include "airbridge_profile.h"

#include "airbridge_dev_info_service.h"
#include "airbridge_serial_service.h"
#include "airbridge_serial_uuid.h"
#include <services/battery_service.h>

#include <furi.h>
#include <ble/ble.h>

#define TAG "BleAirbridgeProfile"

#define AIRBRIDGE_SERIAL_IDENTITY_VERSION      (0x10U)
#define AIRBRIDGE_SERIAL_IDENTITY_VERSION_MASK (0xF0U)

typedef struct {
    FuriHalBleProfileBase base;

    BleServiceBattery* battery_svc;
    BleServiceAirbridgeDevInfo* dev_info_svc;
    BleServiceAirbridgeSerial* serial_svc;
} BleProfileAirbridge;
_Static_assert(offsetof(BleProfileAirbridge, base) == 0, "Wrong layout");

/*
 * LAST-RESORT FALLBACK ONLY: W5 must normally supply the config-file identity.
 * The HP-OUI address tail, manufacturer payload, and DIS values are conspicuous
 * compiled-in placeholders so a missing params block never exposes Flipper data.
 */
static const AirbridgeBleIdentityParams airbridge_default_identity = {
    .device_name = "HP 725 K+M",
    .mac_address = {0x3C, 0x52, 0x82, 0x00, 0x00, 0x01},
    .appearance = GAP_APPEARANCE_UNKNOWN,
    .manufacturer_data = {0x65, 0x00},
    .manufacturer_data_len = 2,
    .dis_manufacturer = "HP",
    .dis_model = "HP 725 K+M",
    .dis_serial = "HP000001",
    .dis_pnp_version = 0x0100,
};

static const AirbridgeBleIdentityParams*
    ble_profile_airbridge_get_identity(FuriHalBleProfileParams profile_params) {
    const AirbridgeBleProfileParams* params = profile_params;
    return params ? &params->identity : &airbridge_default_identity;
}

static void ble_profile_airbridge_free(BleProfileAirbridge* profile) {
    if(profile->serial_svc) {
        ble_svc_airbridge_serial_stop(profile->serial_svc);
    }
    if(profile->dev_info_svc) {
        ble_svc_airbridge_dev_info_stop(profile->dev_info_svc);
    }
    if(profile->battery_svc) {
        ble_svc_battery_stop(profile->battery_svc);
    }
    free(profile);
}

static FuriHalBleProfileBase* ble_profile_airbridge_start(FuriHalBleProfileParams profile_params) {
    const AirbridgeBleIdentityParams* identity =
        ble_profile_airbridge_get_identity(profile_params);
    const AirbridgeDisStrings dis_strings = {
        .manufacturer_name = identity->dis_manufacturer,
        .model_number = identity->dis_model,
        .serial_number = identity->dis_serial,
        .pnp_version = identity->dis_pnp_version,
    };

    BleProfileAirbridge* profile = malloc(sizeof(BleProfileAirbridge));
    if(!profile) {
        FURI_LOG_E(TAG, "Failed to allocate profile");
        return NULL;
    }
    memset(profile, 0, sizeof(*profile));
    profile->base.config = ble_profile_airbridge;

    // GATT budget: battery(8) + AirBridge DIS(9) + serial(12) = 29 <= 68.
    profile->battery_svc = ble_svc_battery_start(true);
    if(!profile->battery_svc) {
        FURI_LOG_E(TAG, "Failed to start battery service");
        goto error;
    }
    profile->dev_info_svc = ble_svc_airbridge_dev_info_start(&dis_strings);
    if(!profile->dev_info_svc) {
        FURI_LOG_E(TAG, "Failed to start device information service");
        goto error;
    }
    profile->serial_svc = ble_svc_airbridge_serial_start();
    if(!profile->serial_svc) {
        FURI_LOG_E(TAG, "Failed to start AirBridge serial service");
        goto error;
    }

    const AirbridgeBleProfileParams* params = profile_params;
    if(params) {
        ble_svc_airbridge_serial_set_callbacks(
            profile->serial_svc, 8U * 64U, params->callback, params->context);
    }

    return &profile->base;

error:
    ble_profile_airbridge_free(profile);
    return NULL;
}

static void ble_profile_airbridge_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile);
    furi_check(profile->config == ble_profile_airbridge);

    ble_profile_airbridge_free((BleProfileAirbridge*)profile);
}

bool airbridge_profile_send(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t len) {
    if(!profile || profile->config != ble_profile_airbridge) return false;
    return ble_svc_airbridge_serial_update_tx(
        ((BleProfileAirbridge*)profile)->serial_svc, data, len);
}

bool airbridge_profile_subscribed(FuriHalBleProfileBase* profile) {
    if(!profile || profile->config != ble_profile_airbridge) return false;
    return ble_svc_airbridge_serial_client_subscribed(((BleProfileAirbridge*)profile)->serial_svc);
}

// AN5289: 4.7, in order to use flash controller interval must be at least 25ms + advertisement, which is 30 ms
// Since we don't use flash controller anymore interval can be lowered to 7.5ms
#define CONNECTION_INTERVAL_MIN (0x0006)
// Up to 45 ms
#define CONNECTION_INTERVAL_MAX (0x24)

static const GapConfig template_config = {
    .adv_service =
        {
            .UUID_Type = UUID_TYPE_128,
            .Service_UUID_128 = BLE_SVC_AIRBRIDGE_SERIAL_SERVICE_UUID,
        },
    .appearance_char = GAP_APPEARANCE_UNKNOWN,
    .bonding_mode = true,
    .pairing_method = GapPairingPinCodeVerifyYesNo,
    .conn_param =
        {
            .conn_int_min = CONNECTION_INTERVAL_MIN,
            .conn_int_max = CONNECTION_INTERVAL_MAX,
            .slave_latency = 0,
            .supervisor_timeout = 0,
        },
};

static void
    ble_profile_airbridge_get_config(GapConfig* config, FuriHalBleProfileParams profile_params) {
    const AirbridgeBleIdentityParams* identity =
        ble_profile_airbridge_get_identity(profile_params);

    furi_check(config);
    furi_check(identity->manufacturer_data_len <= sizeof(identity->manufacturer_data));
    memcpy(config, &template_config, sizeof(GapConfig));

    memcpy(config->mac_address, identity->mac_address, sizeof(config->mac_address));
    config->mac_address[5] = (config->mac_address[5] & ~AIRBRIDGE_SERIAL_IDENTITY_VERSION_MASK) |
                             AIRBRIDGE_SERIAL_IDENTITY_VERSION;
    /* The BLE transport is serial-only on air. Never let an SD-configured HID
     * appearance recruit the host HID daemon onto the single peripheral link. */
    config->appearance_char = GAP_APPEARANCE_UNKNOWN;
    config->adv_name_in_scan_response = true;

    memset(config->adv_name, 0, sizeof(config->adv_name));
    config->adv_name[0] = AD_TYPE_COMPLETE_LOCAL_NAME;
    strlcpy(&config->adv_name[1], identity->device_name, sizeof(config->adv_name) - 1U);

    config->mfg_data_len = identity->manufacturer_data_len;
    memcpy(config->mfg_data, identity->manufacturer_data, config->mfg_data_len);
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = ble_profile_airbridge_start,
    .stop = ble_profile_airbridge_stop,
    .get_gap_config = ble_profile_airbridge_get_config,
};

const FuriHalBleProfileTemplate* const ble_profile_airbridge = &profile_callbacks;

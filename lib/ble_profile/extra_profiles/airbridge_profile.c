#include "airbridge_profile.h"

#include <services/airbridge_dev_info_service.h>
#include <services/airbridge_serial_service.h>
#include <services/airbridge_serial_uuid.h>
#include <services/battery_service.h>
#include <extra_services/hid_service.h>

#include <furi.h>
#include <furi_hal_usb_hid.h>
#include <usb_hid.h>
#include <ble/ble.h>

#define TAG "BleAirbridgeProfile"

#define HID_INFO_BASE_USB_SPECIFICATION                    (0x0101)
#define HID_INFO_COUNTRY_CODE                              (0x00)
#define BLE_PROFILE_HID_INFO_FLAG_REMOTE_WAKE_MSK          (0x01)
#define BLE_PROFILE_HID_INFO_FLAG_NORMALLY_CONNECTABLE_MSK (0x02)

#define BLE_PROFILE_HID_KB_MAX_KEYS   (6)
#define BLE_PROFILE_CONSUMER_MAX_KEYS (1)

enum AirbridgeHidReportId {
    AirbridgeHidReportIdKeyboard = 1,
    AirbridgeHidReportIdMouse = 2,
    AirbridgeHidReportIdConsumer = 3,
};

// Keyboard + mouse + consumer HID report map, matching the stock HID profile.
static const uint8_t ble_profile_airbridge_report_map_data[] = {
    // Keyboard Report
    HID_USAGE_PAGE(HID_PAGE_DESKTOP),
    HID_USAGE(HID_DESKTOP_KEYBOARD),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_REPORT_ID(AirbridgeHidReportIdKeyboard),
    HID_USAGE_PAGE(HID_DESKTOP_KEYPAD),
    HID_USAGE_MINIMUM(HID_KEYBOARD_L_CTRL),
    HID_USAGE_MAXIMUM(HID_KEYBOARD_R_GUI),
    HID_LOGICAL_MINIMUM(0),
    HID_LOGICAL_MAXIMUM(1),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(8),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(1),
    HID_REPORT_SIZE(8),
    HID_INPUT(HID_IOF_CONSTANT | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_USAGE_PAGE(HID_PAGE_LED),
    HID_REPORT_COUNT(8),
    HID_REPORT_SIZE(1),
    HID_USAGE_MINIMUM(1),
    HID_USAGE_MAXIMUM(8),
    HID_OUTPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_COUNT(BLE_PROFILE_HID_KB_MAX_KEYS),
    HID_REPORT_SIZE(8),
    HID_LOGICAL_MINIMUM(0),
    HID_LOGICAL_MAXIMUM(101),
    HID_USAGE_PAGE(HID_DESKTOP_KEYPAD),
    HID_USAGE_MINIMUM(0),
    HID_USAGE_MAXIMUM(101),
    HID_INPUT(HID_IOF_DATA | HID_IOF_ARRAY | HID_IOF_ABSOLUTE),
    HID_END_COLLECTION,
    // Mouse Report
    HID_USAGE_PAGE(HID_PAGE_DESKTOP),
    HID_USAGE(HID_DESKTOP_MOUSE),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_USAGE(HID_DESKTOP_POINTER),
    HID_COLLECTION(HID_PHYSICAL_COLLECTION),
    HID_REPORT_ID(AirbridgeHidReportIdMouse),
    HID_USAGE_PAGE(HID_PAGE_BUTTON),
    HID_USAGE_MINIMUM(1),
    HID_USAGE_MAXIMUM(3),
    HID_LOGICAL_MINIMUM(0),
    HID_LOGICAL_MAXIMUM(1),
    HID_REPORT_COUNT(3),
    HID_REPORT_SIZE(1),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(5),
    HID_INPUT(HID_IOF_CONSTANT | HID_IOF_VARIABLE | HID_IOF_ABSOLUTE),
    HID_USAGE_PAGE(HID_PAGE_DESKTOP),
    HID_USAGE(HID_DESKTOP_X),
    HID_USAGE(HID_DESKTOP_Y),
    HID_USAGE(HID_DESKTOP_WHEEL),
    HID_LOGICAL_MINIMUM(-127),
    HID_LOGICAL_MAXIMUM(127),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(3),
    HID_INPUT(HID_IOF_DATA | HID_IOF_VARIABLE | HID_IOF_RELATIVE),
    HID_END_COLLECTION,
    HID_END_COLLECTION,
    // Consumer Report
    HID_USAGE_PAGE(HID_PAGE_CONSUMER),
    HID_USAGE(HID_CONSUMER_CONTROL),
    HID_COLLECTION(HID_APPLICATION_COLLECTION),
    HID_REPORT_ID(AirbridgeHidReportIdConsumer),
    HID_LOGICAL_MINIMUM(0),
    HID_RI_LOGICAL_MAXIMUM(16, 0x3FF),
    HID_USAGE_MINIMUM(0),
    HID_RI_USAGE_MAXIMUM(16, 0x3FF),
    HID_REPORT_COUNT(BLE_PROFILE_CONSUMER_MAX_KEYS),
    HID_REPORT_SIZE(16),
    HID_INPUT(HID_IOF_DATA | HID_IOF_ARRAY | HID_IOF_ABSOLUTE),
    HID_END_COLLECTION,
};

typedef struct {
    FuriHalBleProfileBase base;

    BleServiceBattery* battery_svc;
    BleServiceAirbridgeDevInfo* dev_info_svc;
    BleServiceHid* hid_svc;
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
    .appearance = GAP_APPEARANCE_KEYBOARD,
    .manufacturer_data = {0x65, 0x00},
    .manufacturer_data_len = 2,
    .dis_manufacturer = "HP",
    .dis_model = "HP 725 K+M",
    .dis_serial = "HP000001",
    .dis_pnp_version = 0x0100,
};

static const AirbridgeBleIdentityParams*
    ble_profile_airbridge_get_identity(FuriHalBleProfileParams profile_params) {
    const AirbridgeBleIdentityParams* identity = (const AirbridgeBleIdentityParams*)profile_params;
    return identity ? identity : &airbridge_default_identity;
}

static void ble_profile_airbridge_free(BleProfileAirbridge* profile) {
    if(profile->serial_svc) {
        ble_svc_airbridge_serial_stop(profile->serial_svc);
    }
    if(profile->hid_svc) {
        ble_svc_hid_stop(profile->hid_svc);
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

    // GATT budget: battery(8) + AirBridge DIS(9) + HID(23) + serial(12) = 52 <= 68.
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
    profile->hid_svc = ble_svc_hid_start();
    if(!profile->hid_svc) {
        FURI_LOG_E(TAG, "Failed to start HID service");
        goto error;
    }

    if(ble_svc_hid_update_report_map(
           profile->hid_svc,
           ble_profile_airbridge_report_map_data,
           sizeof(ble_profile_airbridge_report_map_data))) {
        FURI_LOG_E(TAG, "Failed to initialize HID report map");
        goto error;
    }
    uint8_t hid_info_val[4] = {
        HID_INFO_BASE_USB_SPECIFICATION & 0x00FF,
        (HID_INFO_BASE_USB_SPECIFICATION & 0xFF00) >> 8,
        HID_INFO_COUNTRY_CODE,
        BLE_PROFILE_HID_INFO_FLAG_REMOTE_WAKE_MSK |
            BLE_PROFILE_HID_INFO_FLAG_NORMALLY_CONNECTABLE_MSK,
    };
    if(ble_svc_hid_update_info(profile->hid_svc, hid_info_val)) {
        FURI_LOG_E(TAG, "Failed to initialize HID information");
        goto error;
    }

    profile->serial_svc = ble_svc_airbridge_serial_start();
    if(!profile->serial_svc) {
        FURI_LOG_E(TAG, "Failed to start AirBridge serial service");
        goto error;
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

static bool ble_profile_airbridge_report(
    FuriHalBleProfileBase* profile,
    uint8_t report_id,
    uint8_t* data,
    uint16_t len) {
    furi_check(profile && (profile->config == ble_profile_airbridge));
    furi_check(report_id >= AirbridgeHidReportIdKeyboard);
    furi_check(report_id <= AirbridgeHidReportIdConsumer);

    BleProfileAirbridge* airbridge_profile = (BleProfileAirbridge*)profile;
    // The HID map uses IDs 1..3; the frozen service API selects its report array by index 0..2.
    return ble_svc_hid_update_input_report(airbridge_profile->hid_svc, report_id - 1U, data, len);
}

bool ble_profile_airbridge_kb_report(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t len) {
    return ble_profile_airbridge_report(profile, AirbridgeHidReportIdKeyboard, data, len);
}

bool ble_profile_airbridge_mouse_report(
    FuriHalBleProfileBase* profile,
    uint8_t* data,
    uint16_t len) {
    return ble_profile_airbridge_report(profile, AirbridgeHidReportIdMouse, data, len);
}

bool ble_profile_airbridge_consumer_report(
    FuriHalBleProfileBase* profile,
    uint8_t* data,
    uint16_t len) {
    return ble_profile_airbridge_report(profile, AirbridgeHidReportIdConsumer, data, len);
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
    .appearance_char = GAP_APPEARANCE_KEYBOARD,
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
    config->appearance_char = identity->appearance;
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

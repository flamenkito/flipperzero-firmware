#include "airbridge_dev_info_service.h"
#include "app_common.h"
#include <furi_ble/gatt.h>

#include <ble/ble.h>

#include <furi.h>

#define TAG "BtAirbridgeDevInfoSvc"

typedef enum {
    AirbridgeDevInfoSvcGattCharacteristicMfgName = 0,
    AirbridgeDevInfoSvcGattCharacteristicModel,
    AirbridgeDevInfoSvcGattCharacteristicSerial,
    AirbridgeDevInfoSvcGattCharacteristicPnpId,
    AirbridgeDevInfoSvcGattCharacteristicCount,
} AirbridgeDevInfoSvcGattCharacteristicId;

struct BleServiceAirbridgeDevInfo {
    uint16_t service_handle;
    BleGattCharacteristicInstance characteristics[AirbridgeDevInfoSvcGattCharacteristicCount];
    char* manufacturer_name;
    char* model_number;
    char* serial_number;
    uint8_t pnp_id[7];
};

static void
    airbridge_dev_info_set_pnp_id(BleServiceAirbridgeDevInfo* service, uint16_t pnp_version) {
    service->pnp_id[0] = 0x01;
    service->pnp_id[1] = 0xF0;
    service->pnp_id[2] = 0x03;
    service->pnp_id[3] = 0x41;
    service->pnp_id[4] = 0x53;
    service->pnp_id[5] = pnp_version & 0xFF;
    service->pnp_id[6] = pnp_version >> 8;
}

static void ble_svc_airbridge_dev_info_start_cleanup(
    BleServiceAirbridgeDevInfo* service,
    size_t characteristics_initialized) {
    for(size_t i = 0; i < characteristics_initialized; i++) {
        ble_gatt_characteristic_delete(service->service_handle, &service->characteristics[i]);
    }
    ble_gatt_service_delete(service->service_handle);
    free(service->manufacturer_name);
    free(service->model_number);
    free(service->serial_number);
    free(service);
}

BleServiceAirbridgeDevInfo* ble_svc_airbridge_dev_info_start(const AirbridgeDisStrings* strings) {
    furi_check(strings);
    furi_check(strings->manufacturer_name);
    furi_check(strings->model_number);
    furi_check(strings->serial_number);

    BleServiceAirbridgeDevInfo* service = malloc(sizeof(BleServiceAirbridgeDevInfo));
    if(!service) {
        return NULL;
    }

    service->manufacturer_name = strdup(strings->manufacturer_name);
    service->model_number = strdup(strings->model_number);
    service->serial_number = strdup(strings->serial_number);
    if(!service->manufacturer_name || !service->model_number || !service->serial_number) {
        free(service->manufacturer_name);
        free(service->model_number);
        free(service->serial_number);
        free(service);
        return NULL;
    }

    airbridge_dev_info_set_pnp_id(service, strings->pnp_version);

    const BleGattCharacteristicParams characteristics[AirbridgeDevInfoSvcGattCharacteristicCount] =
        {[AirbridgeDevInfoSvcGattCharacteristicMfgName] =
             {.name = "Manufacturer Name",
              .data_prop_type = FlipperGattCharacteristicDataFixed,
              .data.fixed.length = strlen(service->manufacturer_name),
              .data.fixed.ptr = (const uint8_t*)service->manufacturer_name,
              .uuid.Char_UUID_16 = MANUFACTURER_NAME_UUID,
              .uuid_type = UUID_TYPE_16,
              .char_properties = CHAR_PROP_READ,
              .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
              .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
              .is_variable = CHAR_VALUE_LEN_CONSTANT},
         [AirbridgeDevInfoSvcGattCharacteristicModel] =
             {.name = "Model Number",
              .data_prop_type = FlipperGattCharacteristicDataFixed,
              .data.fixed.length = strlen(service->model_number),
              .data.fixed.ptr = (const uint8_t*)service->model_number,
              .uuid.Char_UUID_16 = MODEL_NUMBER_UUID,
              .uuid_type = UUID_TYPE_16,
              .char_properties = CHAR_PROP_READ,
              .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
              .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
              .is_variable = CHAR_VALUE_LEN_CONSTANT},
         [AirbridgeDevInfoSvcGattCharacteristicSerial] =
             {.name = "Serial Number",
              .data_prop_type = FlipperGattCharacteristicDataFixed,
              .data.fixed.length = strlen(service->serial_number),
              .data.fixed.ptr = (const uint8_t*)service->serial_number,
              .uuid.Char_UUID_16 = SERIAL_NUMBER_UUID,
              .uuid_type = UUID_TYPE_16,
              .char_properties = CHAR_PROP_READ,
              .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
              .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
              .is_variable = CHAR_VALUE_LEN_CONSTANT},
         [AirbridgeDevInfoSvcGattCharacteristicPnpId] = {
             .name = "PnP ID",
             .data_prop_type = FlipperGattCharacteristicDataFixed,
             .data.fixed.length = sizeof(service->pnp_id),
             .data.fixed.ptr = service->pnp_id,
             .uuid.Char_UUID_16 = PNP_ID_UUID,
             .uuid_type = UUID_TYPE_16,
             .char_properties = CHAR_PROP_READ,
             .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
             .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
             .is_variable = CHAR_VALUE_LEN_CONSTANT}};

    uint16_t uuid = DEVICE_INFORMATION_SERVICE_UUID;
    if(!ble_gatt_service_add(
           UUID_TYPE_16,
           (Service_UUID_t*)&uuid,
           PRIMARY_SERVICE,
           1 + 2 * AirbridgeDevInfoSvcGattCharacteristicCount,
           &service->service_handle)) {
        free(service->manufacturer_name);
        free(service->model_number);
        free(service->serial_number);
        free(service);
        return NULL;
    }

    for(size_t i = 0; i < AirbridgeDevInfoSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_init(
            service->service_handle, &characteristics[i], &service->characteristics[i]);
        if(!service->characteristics[i].characteristic) {
            FURI_LOG_E(TAG, "Failed to add characteristic %u", i);
            ble_svc_airbridge_dev_info_start_cleanup(service, i);
            return NULL;
        }
        if(ble_gatt_characteristic_update(
               service->service_handle, &service->characteristics[i], NULL)) {
            FURI_LOG_E(TAG, "Failed to initialize characteristic %u", i);
            ble_svc_airbridge_dev_info_start_cleanup(service, i + 1U);
            return NULL;
        }
    }

    return service;
}

void ble_svc_airbridge_dev_info_stop(BleServiceAirbridgeDevInfo* service) {
    furi_check(service);

    for(size_t i = 0; i < AirbridgeDevInfoSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_delete(service->service_handle, &service->characteristics[i]);
    }
    ble_gatt_service_delete(service->service_handle);

    free(service->manufacturer_name);
    free(service->model_number);
    free(service->serial_number);
    free(service);
}

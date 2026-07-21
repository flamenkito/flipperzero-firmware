#include "airbridge_serial_service.h"
#include "airbridge_serial_uuid.h"
#include "app_common.h"
#include <ble/ble.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>

#include <furi.h>

#include <stdint.h>

#define TAG "BtAirbridgeSerialSvc"

typedef enum {
    AirbridgeSerialSvcGattCharacteristicRx = 0,
    AirbridgeSerialSvcGattCharacteristicTx,
    AirbridgeSerialSvcGattCharacteristicFlowCtrl,
    AirbridgeSerialSvcGattCharacteristicStatus,
    AirbridgeSerialSvcGattCharacteristicCount,
} AirbridgeSerialSvcGattCharacteristicId;

static const BleGattCharacteristicParams
    ble_svc_airbridge_serial_chars[AirbridgeSerialSvcGattCharacteristicCount] = {
        [AirbridgeSerialSvcGattCharacteristicRx] =
            {.name = "RX",
             .data_prop_type = FlipperGattCharacteristicDataFixed,
             .data.fixed.length = BLE_SVC_AIRBRIDGE_SERIAL_DATA_LEN_MAX,
             .uuid.Char_UUID_128 = BLE_SVC_AIRBRIDGE_SERIAL_RX_CHAR_UUID,
             .uuid_type = UUID_TYPE_128,
             .char_properties = CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_WRITE | CHAR_PROP_READ,
             .security_permissions = ATTR_PERMISSION_AUTHEN_READ | ATTR_PERMISSION_AUTHEN_WRITE,
             .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
             .is_variable = CHAR_VALUE_LEN_VARIABLE},
        [AirbridgeSerialSvcGattCharacteristicTx] =
            {.name = "TX",
             .data_prop_type = FlipperGattCharacteristicDataFixed,
             .data.fixed.length = BLE_SVC_AIRBRIDGE_SERIAL_DATA_LEN_MAX,
             .uuid.Char_UUID_128 = BLE_SVC_AIRBRIDGE_SERIAL_TX_CHAR_UUID,
             .uuid_type = UUID_TYPE_128,
             .char_properties = CHAR_PROP_READ | CHAR_PROP_INDICATE,
             .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
             .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
             .is_variable = CHAR_VALUE_LEN_VARIABLE},
        [AirbridgeSerialSvcGattCharacteristicFlowCtrl] =
            {.name = "Flow control",
             .data_prop_type = FlipperGattCharacteristicDataFixed,
             .data.fixed.length = sizeof(uint32_t),
             .uuid.Char_UUID_128 = BLE_SVC_AIRBRIDGE_SERIAL_FLOW_CONTROL_UUID,
             .uuid_type = UUID_TYPE_128,
             .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
             .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
             .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
             .is_variable = CHAR_VALUE_LEN_CONSTANT},
        [AirbridgeSerialSvcGattCharacteristicStatus] = {
            .name = "RPC status",
            .data_prop_type = FlipperGattCharacteristicDataFixed,
            .data.fixed.length = sizeof(uint32_t),
            .uuid.Char_UUID_128 = BLE_SVC_AIRBRIDGE_SERIAL_STATUS_UUID,
            .uuid_type = UUID_TYPE_128,
            .char_properties = CHAR_PROP_READ | CHAR_PROP_WRITE | CHAR_PROP_NOTIFY,
            .security_permissions = ATTR_PERMISSION_AUTHEN_READ | ATTR_PERMISSION_AUTHEN_WRITE,
            .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
            .is_variable = CHAR_VALUE_LEN_CONSTANT}};

static const Service_UUID_t airbridge_serial_service_uuid = {
    .Service_UUID_128 = BLE_SVC_AIRBRIDGE_SERIAL_SERVICE_UUID,
};

struct BleServiceAirbridgeSerial {
    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[AirbridgeSerialSvcGattCharacteristicCount];
    FuriMutex* buff_size_mtx;
    uint32_t buff_size;
    uint16_t bytes_ready_to_receive;
    AirbridgeSerialServiceEventCallback callback;
    void* context;
    GapSvcEventHandler* event_handler;
};

static BleServiceAirbridgeSerial* active_airbridge_serial_service = NULL;

static BleEventAckStatus ble_svc_airbridge_serial_event_handler(void* event, void* context) {
    BleServiceAirbridgeSerial* serial_svc = (BleServiceAirbridgeSerial*)context;
    BleEventAckStatus ret = BleEventNotAck;
    hci_event_pckt* event_pckt = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);
    evt_blecore_aci* blecore_evt = (evt_blecore_aci*)event_pckt->data;
    aci_gatt_attribute_modified_event_rp0* attribute_modified;
    if(event_pckt->evt == HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        if(blecore_evt->ecode == ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) {
            attribute_modified = (aci_gatt_attribute_modified_event_rp0*)blecore_evt->data;
            if(attribute_modified->Attr_Handle ==
               serial_svc->chars[AirbridgeSerialSvcGattCharacteristicRx].handle + 2) {
                ret = BleEventAckFlowEnable;
                FURI_LOG_D(TAG, "RX descriptor event");
            } else if(
                attribute_modified->Attr_Handle ==
                serial_svc->chars[AirbridgeSerialSvcGattCharacteristicRx].handle + 1) {
                FURI_LOG_D(TAG, "Received %d bytes", attribute_modified->Attr_Data_Length);
                if(serial_svc->callback) {
                    furi_check(
                        furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) ==
                        FuriStatusOk);
                    if(attribute_modified->Attr_Data_Length > serial_svc->bytes_ready_to_receive) {
                        FURI_LOG_W(
                            TAG,
                            "Received %d, while was ready to receive %d bytes. Can lead to buffer overflow!",
                            attribute_modified->Attr_Data_Length,
                            serial_svc->bytes_ready_to_receive);
                    }
                    serial_svc->bytes_ready_to_receive -= MIN(
                        serial_svc->bytes_ready_to_receive, attribute_modified->Attr_Data_Length);
                    SerialServiceEvent event = {
                        .event = SerialServiceEventTypeDataReceived,
                        .data = {
                            .buffer = attribute_modified->Attr_Data,
                            .size = attribute_modified->Attr_Data_Length,
                        }};
                    uint32_t buff_free_size = serial_svc->callback(event, serial_svc->context);
                    FURI_LOG_D(TAG, "Available buff size: %ld", buff_free_size);
                    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
                }
                ret = BleEventAckFlowEnable;
            } else if(
                attribute_modified->Attr_Handle ==
                serial_svc->chars[AirbridgeSerialSvcGattCharacteristicStatus].handle + 1) {
                bool* rpc_status = (bool*)attribute_modified->Attr_Data;
                if(!*rpc_status) {
                    if(serial_svc->callback) {
                        SerialServiceEvent event = {
                            .event = SerialServiceEventTypesBleResetRequest,
                        };
                        serial_svc->callback(event, serial_svc->context);
                    }
                }
            }
        } else if(blecore_evt->ecode == ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE) {
            FURI_LOG_T(TAG, "Ack received");
            if(serial_svc->callback) {
                SerialServiceEvent event = {
                    .event = SerialServiceEventTypeDataSent,
                };
                serial_svc->callback(event, serial_svc->context);
            }
            ret = BleEventAckFlowEnable;
        }
    }
    return ret;
}

typedef enum {
    AirbridgeSerialServiceRpcStatusNotActive = 0UL,
    AirbridgeSerialServiceRpcStatusActive = 1UL,
} AirbridgeSerialServiceRpcStatus;

static void ble_svc_airbridge_serial_update_rpc_char(
    BleServiceAirbridgeSerial* serial_svc,
    AirbridgeSerialServiceRpcStatus status) {
    ble_gatt_characteristic_update(
        serial_svc->svc_handle,
        &serial_svc->chars[AirbridgeSerialSvcGattCharacteristicStatus],
        &status);
}

BleServiceAirbridgeSerial* ble_svc_airbridge_serial_start(void) {
    furi_check(!active_airbridge_serial_service);

    BleServiceAirbridgeSerial* serial_svc = malloc(sizeof(BleServiceAirbridgeSerial));

    serial_svc->event_handler = ble_event_dispatcher_register_svc_handler(
        ble_svc_airbridge_serial_event_handler, serial_svc);

    if(!ble_gatt_service_add(
           UUID_TYPE_128,
           &airbridge_serial_service_uuid,
           PRIMARY_SERVICE,
           12,
           &serial_svc->svc_handle)) {
        free(serial_svc);
        return NULL;
    }
    for(uint8_t i = 0; i < AirbridgeSerialSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_init(
            serial_svc->svc_handle, &ble_svc_airbridge_serial_chars[i], &serial_svc->chars[i]);
    }

    ble_svc_airbridge_serial_update_rpc_char(serial_svc, AirbridgeSerialServiceRpcStatusNotActive);
    serial_svc->buff_size_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
    active_airbridge_serial_service = serial_svc;

    return serial_svc;
}

void ble_svc_airbridge_serial_set_callbacks(
    BleServiceAirbridgeSerial* serial_svc,
    uint16_t buff_size,
    AirbridgeSerialServiceEventCallback callback,
    void* context) {
    furi_check(serial_svc);
    serial_svc->callback = callback;
    serial_svc->context = context;
    serial_svc->buff_size = buff_size;
    serial_svc->bytes_ready_to_receive = buff_size;

    uint32_t buff_size_reversed = REVERSE_BYTES_U32(serial_svc->buff_size);
    ble_gatt_characteristic_update(
        serial_svc->svc_handle,
        &serial_svc->chars[AirbridgeSerialSvcGattCharacteristicFlowCtrl],
        &buff_size_reversed);
}

void ble_svc_airbridge_serial_notify_buffer_is_empty(BleServiceAirbridgeSerial* serial_svc) {
    furi_check(serial_svc);
    furi_check(serial_svc->buff_size_mtx);

    furi_check(furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) == FuriStatusOk);
    if(serial_svc->bytes_ready_to_receive == 0) {
        FURI_LOG_D(TAG, "Buffer is empty. Notifying client");
        serial_svc->bytes_ready_to_receive = serial_svc->buff_size;

        uint32_t buff_size_reversed = REVERSE_BYTES_U32(serial_svc->buff_size);
        ble_gatt_characteristic_update(
            serial_svc->svc_handle,
            &serial_svc->chars[AirbridgeSerialSvcGattCharacteristicFlowCtrl],
            &buff_size_reversed);
    }
    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
}

void ble_svc_airbridge_serial_stop(BleServiceAirbridgeSerial* serial_svc) {
    furi_check(serial_svc);
    furi_check(serial_svc == active_airbridge_serial_service);

    active_airbridge_serial_service = NULL;
    ble_event_dispatcher_unregister_svc_handler(serial_svc->event_handler);

    for(uint8_t i = 0; i < AirbridgeSerialSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_delete(serial_svc->svc_handle, &serial_svc->chars[i]);
    }
    ble_gatt_service_delete(serial_svc->svc_handle);
    furi_mutex_free(serial_svc->buff_size_mtx);
    free(serial_svc);
}

bool ble_svc_airbridge_serial_update_tx(
    BleServiceAirbridgeSerial* serial_svc,
    uint8_t* data,
    uint16_t data_len) {
    if(data_len > BLE_SVC_AIRBRIDGE_SERIAL_DATA_LEN_MAX) {
        return false;
    }

    for(uint16_t remained = data_len; remained > 0;) {
        uint8_t value_len = MIN(BLE_SVC_AIRBRIDGE_SERIAL_CHAR_VALUE_LEN_MAX, remained);
        uint16_t value_offset = data_len - remained;
        remained -= value_len;

        tBleStatus result = aci_gatt_update_char_value_ext(
            0,
            serial_svc->svc_handle,
            serial_svc->chars[AirbridgeSerialSvcGattCharacteristicTx].handle,
            remained ? 0x00 : 0x02,
            data_len,
            value_offset,
            value_len,
            data + value_offset);

        if(result) {
            FURI_LOG_E(TAG, "Failed updating TX characteristic: %d", result);
            return false;
        }
    }

    return true;
}

void ble_svc_airbridge_serial_set_rpc_active(BleServiceAirbridgeSerial* serial_svc, bool active) {
    furi_check(serial_svc);
    ble_svc_airbridge_serial_update_rpc_char(
        serial_svc,
        active ? AirbridgeSerialServiceRpcStatusActive : AirbridgeSerialServiceRpcStatusNotActive);
}

BleServiceAirbridgeSerial* ble_svc_airbridge_serial_get_active(void) {
    return active_airbridge_serial_service;
}

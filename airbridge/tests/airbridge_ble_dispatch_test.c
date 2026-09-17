#include <furi.h>
#undef furi_check
#include <core/check.h>
#include <ble/ble.h>
#include <interface/patterns/ble_thread/tl/tl.h>

/* The firmware build checks the 32-bit GATT ABI; this executable uses host pointers. */
#define _Static_assert(condition, message) \
    _Static_assert(sizeof(void*) != 4 || (condition), message)
#include <furi_ble/gatt.h>
#undef _Static_assert

#undef furi_check
#undef furi_assert
#define furi_check(condition)  assert(condition)
#define furi_assert(condition) assert(condition)
#undef FURI_LOG_D
#define FURI_LOG_D(tag, ...) host_log(tag, __VA_ARGS__)
#define FURI_LOG_T(tag, ...) host_log(tag, __VA_ARGS__)

static void host_log(const char* tag, const char* format, ...) {
    UNUSED(tag);
    UNUSED(format);
}

typedef struct {
    bool locked;
} FuriMutex;
enum {
    FuriMutexTypeNormal,
    FuriWaitForever = UINT32_MAX
};

static FuriMutex* furi_mutex_alloc(unsigned type) {
    assert(type == FuriMutexTypeNormal);
    return calloc(1, sizeof(FuriMutex));
}

static void furi_mutex_free(FuriMutex* mutex) {
    assert(!mutex->locked);
    free(mutex);
}

static FuriStatus furi_mutex_acquire(FuriMutex* mutex, uint32_t timeout) {
    assert(timeout == FuriWaitForever);
    assert(!mutex->locked);
    mutex->locked = true;
    return FuriStatusOk;
}

static FuriStatus furi_mutex_release(FuriMutex* mutex) {
    assert(mutex->locked);
    mutex->locked = false;
    return FuriStatusOk;
}

#include "../../targets/f7/ble_glue/furi_ble/event_dispatcher.c"
#include "../../lib/ble_profile/extra_services/hid_service.c"
#undef TAG
#include "../../applications_user/pocket_airbridge/airbridge_serial_service.c"

static struct {
    uint16_t handle;
    uint16_t next;
    uint16_t end;
} services[2];
static size_t service_count;
static unsigned unclaimed;
static unsigned received;
static uint8_t received_data[244];
static uint16_t received_len;
static unsigned updates;
static unsigned resets;

bool ble_gatt_service_add(
    uint8_t uuid_type,
    const Service_UUID_t* uuid,
    uint8_t service_type,
    uint8_t attribute_count,
    uint16_t* handle) {
    UNUSED(uuid_type);
    UNUSED(uuid);
    assert(service_type == PRIMARY_SERVICE);
    assert(service_count < COUNT_OF(services));
    *handle = service_count ? services[service_count - 1].end : 0x20;
    services[service_count].handle = *handle;
    services[service_count].next = *handle + 1;
    services[service_count].end = *handle + attribute_count;
    service_count++;
    return true;
}

bool ble_gatt_service_delete(uint16_t handle) {
    UNUSED(handle);
    return true;
}

void ble_gatt_characteristic_init(
    uint16_t service,
    const BleGattCharacteristicParams* params,
    BleGattCharacteristicInstance* instance) {
    for(size_t i = 0; i < service_count; i++) {
        if(services[i].handle != service) continue;
        BleGattCharacteristicParams* copy = malloc(sizeof(*copy));
        assert(copy);
        *copy = *params;
        instance->characteristic = copy;
        instance->handle = services[i].next;
        services[i].next += 2;
        if(params->char_properties & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE)) {
            services[i].next++;
        }
        if(params->descriptor_params) {
            instance->descriptor_handle = services[i].next++;
        }
        assert(services[i].next <= services[i].end);
        return;
    }
    abort();
}

void ble_gatt_characteristic_delete(uint16_t service, BleGattCharacteristicInstance* instance) {
    UNUSED(service);
    free((void*)instance->characteristic);
}

bool ble_gatt_characteristic_update(
    uint16_t service,
    BleGattCharacteristicInstance* instance,
    const void* source) {
    UNUSED(service);
    UNUSED(instance);
    UNUSED(source);
    updates++;
    return false;
}

BleEventFlowStatus ble_event_app_notification(void* packet) {
    UNUSED(packet);
    unclaimed++;
    return BleEventFlowEnable;
}

static uint16_t on_serial(SerialServiceEvent event, void* context) {
    assert(context == &received);
    if(event.event == SerialServiceEventTypesBleResetRequest) resets++;
    if(event.event == SerialServiceEventTypeDataReceived) {
        assert(event.data.size <= sizeof(received_data));
        received++;
        received_len = event.data.size;
        memcpy(received_data, event.data.buffer, event.data.size);
    }
    return 8 * 64;
}

static void write_attribute(uint16_t handle, const uint8_t* data, uint16_t length) {
    uint8_t packet[300] = {0};
    hci_uart_pckt* uart = (void*)packet;
    uart->type = TL_BLEEVT_PKT_TYPE;
    hci_event_pckt* hci = (void*)uart->data;
    hci->evt = HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE;
    evt_blecore_aci* aci = (void*)hci->data;
    aci->ecode = ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE;
    aci_gatt_attribute_modified_event_rp0* modified = (void*)aci->data;
    modified->Attr_Handle = handle;
    modified->Attr_Data_Length = length;
    memcpy(modified->Attr_Data, data, length);
    hci->plen = 2 + offsetof(aci_gatt_attribute_modified_event_rp0, Attr_Data) + length;
    assert(ble_event_dispatcher_process_event(packet) == BleEventFlowEnable);
}

static void test_composite(bool hid_first) {
    memset(services, 0, sizeof(services));
    service_count = unclaimed = received = updates = resets = 0;
    BleServiceHid* hid = hid_first ? ble_svc_hid_start() : NULL;
    BleServiceAirbridgeSerial* serial = ble_svc_airbridge_serial_start();
    assert(serial);
    ble_svc_airbridge_serial_set_callbacks(serial, 8 * 64, on_serial, &received);
    if(!hid_first) hid = ble_svc_hid_start();
    assert(hid);

    uint8_t frame[64] = {0x0a};
    assert(!ble_svc_airbridge_serial_update_tx(serial, frame, sizeof(frame)));
    const uint16_t cccd = serial->chars[AirbridgeSerialSvcGattCharacteristicTx].handle + 2;
    const uint8_t enable[] = {1, 0};
    write_attribute(cccd, enable, sizeof(enable));
    assert(ble_svc_airbridge_serial_client_subscribed(serial));
    assert(resets == 1);
    unsigned before = updates;
    assert(ble_svc_airbridge_serial_update_tx(serial, frame, sizeof(frame)));
    assert(updates == before + 1);

    const uint16_t rx = serial->chars[AirbridgeSerialSvcGattCharacteristicRx].handle + 1;
    const uint8_t trigger = 0x42;
    write_attribute(rx, &trigger, 1);
    assert(received == 1 && received_len == 1 && received_data[0] == trigger);
    write_attribute(rx, frame, sizeof(frame));
    assert(received == 2 && received_len == sizeof(frame));
    assert(memcmp(received_data, frame, sizeof(frame)) == 0);

    const uint8_t zero[] = {0, 0};
    write_attribute(hid->chars[HidSvcGattCharacteristicProtocolMode].handle + 1, zero, 1);
    write_attribute(hid->chars[HidSvcGattCharacteristicCtrlPoint].handle + 1, zero, 1);
    write_attribute(hid->input_report_chars[0].handle + 2, enable, sizeof(enable));
    assert(received == 2 && unclaimed == 0);

    uint8_t packet[192];
    memset(packet, 0x5A, sizeof(packet));
    assert(ble_svc_airbridge_serial_update_tx(serial, packet, sizeof(packet)));
    write_attribute(rx, packet, sizeof(packet));
    assert(received == 3 && received_len == sizeof(packet));
    assert(memcmp(received_data, packet, sizeof(packet)) == 0);

    write_attribute(cccd, zero, sizeof(zero));
    assert(!ble_svc_airbridge_serial_client_subscribed(serial));
    assert(resets == 2);
    write_attribute(cccd, enable, sizeof(enable));
    uint8_t disconnect[] = {
        TL_BLEEVT_PKT_TYPE, HCI_DISCONNECTION_COMPLETE_EVT_CODE, 4, 0, 1, 0, 0x13};
    assert(ble_event_dispatcher_process_event(disconnect) == BleEventFlowEnable);
    assert(!ble_svc_airbridge_serial_client_subscribed(serial));
    assert(resets == 4);
    assert(!ble_svc_airbridge_serial_update_tx(serial, frame, sizeof(frame)));

    ble_svc_airbridge_serial_stop(serial);
    before = unclaimed;
    write_attribute(hid->svc_handle, zero, 1);
    write_attribute(hid->svc_handle - 1, zero, 1);
    write_attribute(services[hid_first ? 0 : 1].end, zero, 1);
    assert(unclaimed == before + 3);

    uint8_t confirmation[] = {
        TL_BLEEVT_PKT_TYPE,
        HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE,
        4,
        ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE & 0xff,
        ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE >> 8,
        1,
        0};
    assert(ble_event_dispatcher_process_event(confirmation) == BleEventFlowEnable);
    assert(unclaimed == before + 4);
    ble_svc_hid_stop(hid);
    ble_event_dispatcher_reset();
}

int main(void) {
    ble_event_dispatcher_init();
    test_composite(false);
    test_composite(true);
    puts("PASS: HID and AirBridge serial share events in either registration order");
    return 0;
}

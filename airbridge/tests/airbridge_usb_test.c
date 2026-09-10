#include "platform/furi.h"
#include "../../applications_user/pocket_airbridge/airbridge_usb.h"
#include <furi_hal_usb_spoof.h>

struct FuriSemaphore {
    bool live;
    uint32_t count;
};

static FuriSemaphore semaphores[4];
static unsigned allocations;
static unsigned frees;
static unsigned received_events;
static uint16_t written_size;
static void (*on_acquire)(void);
static usbd_device device;
static FuriHalUsbInterface* profile;

static const uint8_t fixture_vendor_report[FURI_HAL_USB_SPOOF_VENDOR_REPORT_DESC_LEN] = {0xA5};
static const uint8_t fixture_keyboard_report[FURI_HAL_USB_SPOOF_KEYBOARD_REPORT_DESC_LEN] = {
    0x5A};
static const HidCompositeConfigDescriptor fixture_composite = {
    .keyboard.hid_desc = {.bLength = sizeof(struct usb_hid_descriptor)},
    .vendor_hid_desc = {.bLength = sizeof(struct usb_hid_descriptor)},
};
static struct usb_device_descriptor fixture_devices[] = {
    {.idVendor = 0x046D, .idProduct = 0xC31C},
    {.idVendor = 0x413C, .idProduct = 0x2113},
};
static const uint8_t fixture_manufacturers[][2] = {{0x11}, {0x22}};
static const uint8_t fixture_products[][2] = {{0x33}, {0x44}};
static FuriHalUsbInterface fixture_spoof_interfaces[2];
typedef struct {
    struct usb_config_descriptor config;
    HidKeyboardDescriptor keyboard;
    struct usb_interface_descriptor vendor;
    struct usb_hid_descriptor vendor_hid_desc;
    struct usb_endpoint_descriptor vendor_ep_in;
    struct usb_endpoint_descriptor vendor_ep_out;
} FURI_PACKED FixtureNoIadConfigDescriptor;
static const FuriHalUsbSpoofIdentity fixture_identities[] = {
    {
        .device_desc = &fixture_devices[0],
        .manuf_desc = &fixture_manufacturers[0],
        .prod_desc = &fixture_products[0],
        .composite_config_desc = &fixture_composite,
        .keyboard_hid_desc = &fixture_composite.keyboard.hid_desc,
        .keyboard_hid_desc_len = sizeof(fixture_composite.keyboard.hid_desc),
        .vendor_hid_desc = &fixture_composite.vendor_hid_desc,
        .vendor_hid_desc_len = sizeof(fixture_composite.vendor_hid_desc),
    },
    {
        .device_desc = &fixture_devices[1],
        .manuf_desc = &fixture_manufacturers[1],
        .prod_desc = &fixture_products[1],
        .composite_config_desc = &fixture_composite,
        .keyboard_hid_desc = &fixture_composite.keyboard.hid_desc,
        .keyboard_hid_desc_len = sizeof(fixture_composite.keyboard.hid_desc),
        .vendor_hid_desc = &fixture_composite.vendor_hid_desc,
        .vendor_hid_desc_len = sizeof(fixture_composite.vendor_hid_desc),
    },
};

FuriHalUsbInterface* furi_hal_usb_spoof_get_interface(FuriHalUsbSpoofProfile selected) {
    return &fixture_spoof_interfaces[selected == FuriHalUsbSpoofProfileDell];
}

const FuriHalUsbSpoofIdentity*
    furi_hal_usb_spoof_get_identity(FuriHalUsbSpoofProfile selected) {
    return &fixture_identities[selected == FuriHalUsbSpoofProfileDell];
}

const uint8_t* furi_hal_usb_spoof_vendor_report_desc(uint16_t* length) {
    *length = sizeof(fixture_vendor_report);
    return fixture_vendor_report;
}

const uint8_t* furi_hal_usb_spoof_keyboard_report_desc(uint16_t* length) {
    *length = sizeof(fixture_keyboard_report);
    return fixture_keyboard_report;
}

FuriSemaphore* furi_semaphore_alloc(uint32_t maximum, uint32_t initial) {
    assert(maximum == 1 && allocations < COUNT_OF(semaphores));
    UNUSED(maximum);
    FuriSemaphore* semaphore = &semaphores[allocations++];
    *semaphore = (FuriSemaphore){.live = true, .count = initial};
    return semaphore;
}

void furi_semaphore_free(FuriSemaphore* semaphore) {
    assert(semaphore->live);
    semaphore->live = false;
    frees++;
}

FuriStatus furi_semaphore_acquire(FuriSemaphore* semaphore, uint32_t timeout) {
    UNUSED(timeout);
    if(on_acquire) {
        void (*callback)(void) = on_acquire;
        on_acquire = NULL;
        callback();
    }
    assert(semaphore->live);
    if(!semaphore->count) return FuriStatusErrorTimeout;
    semaphore->count--;
    return FuriStatusOk;
}

FuriStatus furi_semaphore_release(FuriSemaphore* semaphore) {
    assert(semaphore->live);
    semaphore->count = 1;
    return FuriStatusOk;
}

static uint8_t connect_usb(bool connected) {
    UNUSED(connected);
    return 0;
}

static bool configure_endpoint(uint8_t endpoint, uint8_t type, uint16_t size) {
    UNUSED(endpoint);
    assert(type == USB_EPTYPE_INTERRUPT);
    assert(size == 8 || size == 64);
    UNUSED(type);
    UNUSED(size);
    return true;
}

static void deconfigure_endpoint(uint8_t endpoint) {
    UNUSED(endpoint);
}

static int32_t write_endpoint(uint8_t endpoint, const void* data, uint16_t size) {
    UNUSED(endpoint);
    UNUSED(data);
    written_size = size;
    return size;
}

static int32_t read_endpoint(uint8_t endpoint, void* data, uint16_t size) {
    UNUSED(endpoint);
    memset(data, 0xA5, size);
    return size;
}

static void receive_event(HidVendorEvent event, void* context) {
    UNUSED(context);
    if(event == HidVendorRequest) received_events++;
}

static void initialize_profile(void) {
    profile->init(&device, profile, NULL);
    assert(device.config_callback && device.control_callback);
    assert(device.config_callback(&device, 1) == usbd_ack);
    profile->wakeup(&device);
}

static void assert_control_descriptor(
    uint8_t descriptor_type,
    uint16_t interface,
    const void* expected,
    uint16_t expected_length) {
    usbd_ctlreq request = {
        .bmRequestType = USB_REQ_INTERFACE | USB_REQ_STANDARD,
        .bRequest = USB_STD_GET_DESCRIPTOR,
        .wValue = (uint16_t)descriptor_type << 8,
        .wIndex = interface,
    };
    assert(device.control_callback(&device, &request, NULL) == usbd_ack);
    assert(device.status.data_ptr == expected);
    assert(device.status.data_count == expected_length);
    UNUSED(request);
    UNUSED(expected);
    UNUSED(expected_length);
}

static void assert_profile_control(uint8_t index) {
    profile = airbridge_usb_get_profile(index);
    initialize_profile();
    const bool keyboard = airbridge_usb_profile_has_keyboard(index);
    if(keyboard) {
        const void* keyboard_hid = index == AirbridgeUsbProfileHpKbdVendor ?
                                       &((const FixtureNoIadConfigDescriptor*)profile->cfg_descr)
                                            ->keyboard.hid_desc :
                                       fixture_identities[0].keyboard_hid_desc;
        assert_control_descriptor(
            USB_DTYPE_HID, 0, keyboard_hid, sizeof(struct usb_hid_descriptor));
        assert_control_descriptor(
            USB_DTYPE_HID_REPORT, 0, fixture_keyboard_report, sizeof(fixture_keyboard_report));
    }
    const uint16_t vendor_interface = keyboard ? 1 : 0;
    const void* vendor_hid = NULL;
    if(index == AirbridgeUsbProfileMsftVendorOnly) {
        vendor_hid = &((const HidVendorConfigDescriptor*)profile->cfg_descr)->vendor.hid_desc;
    } else if(index == AirbridgeUsbProfileHpKbdVendor) {
        vendor_hid =
            &((const FixtureNoIadConfigDescriptor*)profile->cfg_descr)->vendor_hid_desc;
    } else {
        vendor_hid = fixture_identities[0].vendor_hid_desc;
    }
    assert_control_descriptor(
        USB_DTYPE_HID, vendor_interface, vendor_hid, sizeof(struct usb_hid_descriptor));
    assert_control_descriptor(
        USB_DTYPE_HID_REPORT,
        vendor_interface,
        fixture_vendor_report,
        sizeof(fixture_vendor_report));
    profile->suspend(&device);
    profile->deinit(&device);
}

static void reinitialize_during_send(void) {
    profile->suspend(&device);
    profile->deinit(&device);
    assert(frees == 0);
    initialize_profile();
    assert(allocations == 2);
}

int main(void) {
    assert(airbridge_usb_profile_count() == 5);
    const struct usbd_driver driver = {
        .connect = connect_usb,
        .ep_config = configure_endpoint,
        .ep_deconfig = deconfigure_endpoint,
        .ep_write = write_endpoint,
        .ep_read = read_endpoint,
    };
    device.driver = &driver;
    assert_profile_control(AirbridgeUsbProfileLogitechKbdVendor);
    assert(airbridge_usb_get_profile(AirbridgeUsbProfileLogitechKbdVendor)->dev_descr ==
           fixture_identities[0].device_desc);
    assert(airbridge_usb_get_profile(AirbridgeUsbProfileDellKbdVendor)->dev_descr ==
           fixture_identities[1].device_desc);
    assert(airbridge_usb_get_profile(AirbridgeUsbProfileMsftKbdVendor)->cfg_descr ==
           (const void*)&fixture_composite);
    for(uint8_t index = 0; index < airbridge_usb_profile_count(); index++) {
        assert(airbridge_usb_profile_vid(index) != 0x0483);
        assert(
            airbridge_usb_get_profile(index)->dev_descr->idVendor ==
            airbridge_usb_profile_vid(index));
        assert(
            airbridge_usb_get_profile(index)->dev_descr->idProduct ==
            airbridge_usb_profile_pid(index));
    }
    assert(airbridge_usb_profile_vid(AirbridgeUsbProfileHpKbdVendor) == 0x03F0);
    assert(airbridge_usb_profile_pid(AirbridgeUsbProfileHpKbdVendor) == 0x5341);
    for(uint8_t index = 1; index < airbridge_usb_profile_count(); index++) {
        assert_profile_control(index);
    }
    profile = airbridge_usb_get_profile(AirbridgeUsbProfileHpKbdVendor);
    initialize_profile();
    airbridge_usb_vendor_set_callback(receive_event, NULL);
    device.endpoint[3](&device, usbd_evt_eprx, 3);
    assert(received_events == 1);

    uint8_t frame[64] = {0};
    on_acquire = reinitialize_during_send;
    assert(airbridge_usb_vendor_send_response_blocking(frame, sizeof(frame), 100));
    assert(written_size == 64);
    UNUSED(frame);
    airbridge_usb_vendor_set_callback(NULL, NULL);
    device.endpoint[3](&device, usbd_evt_eprx, 3);
    assert(received_events == 1);
    profile->suspend(&device);
    profile->deinit(&device);
    assert(!device.config_callback && !device.control_callback);
    for(unsigned endpoint = 0; endpoint < COUNT_OF(device.endpoint); endpoint++) {
        assert(!device.endpoint[endpoint]);
    }
    assert(frees == 0);
    airbridge_usb_free();
    assert(frees == allocations);
    return 0;
}

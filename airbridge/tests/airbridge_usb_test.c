#include "platform/furi.h"
#include "../../applications_user/pocket_airbridge/airbridge_usb.h"

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

FuriSemaphore* furi_semaphore_alloc(uint32_t maximum, uint32_t initial) {
    assert(maximum == 1 && allocations < COUNT_OF(semaphores));
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

static void reinitialize_during_send(void) {
    profile->suspend(&device);
    profile->deinit(&device);
    assert(frees == 0);
    initialize_profile();
    assert(allocations == 2);
}

int main(void) {
    assert(airbridge_usb_profile_count() == 5);
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
    const struct usbd_driver driver = {
        .connect = connect_usb,
        .ep_config = configure_endpoint,
        .ep_deconfig = deconfigure_endpoint,
        .ep_write = write_endpoint,
        .ep_read = read_endpoint,
    };
    device.driver = &driver;
    profile = airbridge_usb_get_profile(AirbridgeUsbProfileHpKbdVendor);
    initialize_profile();
    airbridge_usb_vendor_set_callback(receive_event, NULL);
    device.endpoint[3](&device, usbd_evt_eprx, 3);
    assert(received_events == 1);

    uint8_t frame[64] = {0};
    on_acquire = reinitialize_during_send;
    assert(airbridge_usb_vendor_send_response_blocking(frame, sizeof(frame), 100));
    assert(written_size == 64);
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

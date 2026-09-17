#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../../applications_user/pocket_airbridge/airbridge_ui_refresh.h"

int main(void) {
    AirbridgeUiSnapshot shown = {0};
    AirbridgeUiSnapshot incoming = {0};
    assert(airbridge_ui_refresh_needed(&shown, &incoming, false, false));
    assert(!airbridge_ui_refresh_needed(&shown, &incoming, true, true));

    uint32_t last = UINT32_MAX - 100;
    const uint32_t start = last;
    unsigned redraws = 0;
    for(uint32_t elapsed = 1; elapsed <= 5000; elapsed++) {
        const uint32_t now = start + elapsed;
        incoming.metrics.chunks_usb_to_ble++;
        incoming.metrics.chunks_ble_to_usb++;
        if(airbridge_ui_refresh_needed(&shown, &incoming, true, now - last >= 200)) {
            shown = incoming;
            last = now;
            redraws++;
            assert(elapsed % 200 == 0);
        }
    }
    assert(redraws == 25);
    assert(shown.metrics.chunks_usb_to_ble == 5000);
    assert(!airbridge_ui_refresh_needed(&shown, &incoming, true, true));

    incoming.screen = AirbridgeScreenPasswordTyping;
    incoming.typing_generation = 1;
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    shown = incoming;
    incoming.typing_position = 1;
    assert(!airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, true));
    incoming.typing_generation++;
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));

    shown = incoming;
    incoming.metrics.usb_connected = true;
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    shown = incoming;
    incoming.ble_connected = true;
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    shown = incoming;
    incoming.mouse_enabled = true;
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    shown = incoming;
    incoming.password_selected++;
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    shown = incoming;
    strcpy(incoming.menu_status, "Could not save setting");
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    shown = incoming;
    strcpy(incoming.error.detail, "USB disconnected");
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    shown = incoming;
    incoming.metrics.tx_errors++;
    assert(!airbridge_ui_refresh_needed(&shown, &incoming, true, false));
    assert(airbridge_ui_refresh_needed(&shown, &incoming, true, true));
    return 0;
}

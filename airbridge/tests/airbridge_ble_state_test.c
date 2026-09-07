#include <stdbool.h>
#include <stdint.h>

#define REQUIRE(condition) \
    do {                   \
        if(!(condition)) __builtin_trap(); \
    } while(false)

typedef struct {
    bool ble_connected;
    uint32_t ble_connected_since;
    uint32_t ble_last_rx_tick;
    uint32_t ble_desync_since;
    uint32_t link_generation;
} AirbridgeBle;

#include "../../applications_user/pocket_airbridge/airbridge_ble_state.h"

static AirbridgeBleLinkStateRefs link_state(AirbridgeBle* ble) {
    return (AirbridgeBleLinkStateRefs){
        .connected = &ble->ble_connected,
        .connected_since = &ble->ble_connected_since,
        .last_rx_tick = &ble->ble_last_rx_tick,
        .desync_since = &ble->ble_desync_since,
        .generation = &ble->link_generation,
    };
}

static void connected_initializes_link_state(void) {
    AirbridgeBle ble = {0};

    REQUIRE(airbridge_ble_link_apply_connected(link_state(&ble), 100, 1));

    REQUIRE(ble.ble_connected);
    REQUIRE(ble.link_generation == 1);
    REQUIRE(ble.ble_connected_since == 100);
    REQUIRE(ble.ble_last_rx_tick == 0);
    REQUIRE(ble.ble_desync_since == 0);
}

static void coalesced_reconnect_resets_link_state(void) {
    AirbridgeBle ble = {
        .ble_connected = true,
        .ble_connected_since = 10,
        .ble_last_rx_tick = 20,
        .ble_desync_since = 30,
        .link_generation = 4,
    };

    REQUIRE(airbridge_ble_link_apply_connected(link_state(&ble), 100, 1));

    REQUIRE(ble.ble_connected);
    REQUIRE(ble.link_generation == 5);
    REQUIRE(ble.ble_connected_since == 100);
    REQUIRE(ble.ble_last_rx_tick == 0);
    REQUIRE(ble.ble_desync_since == 0);
}

static void rx_before_status_preserves_established_link_state(void) {
    AirbridgeBle ble = {0};

    REQUIRE(airbridge_ble_link_apply_connected(link_state(&ble), 100, 1));
    ble.ble_last_rx_tick = 100;
    REQUIRE(!airbridge_ble_link_apply_connected(link_state(&ble), 110, 0));

    REQUIRE(ble.ble_connected);
    REQUIRE(ble.link_generation == 1);
    REQUIRE(ble.ble_connected_since == 100);
    REQUIRE(ble.ble_last_rx_tick == 100);
}

int main(void) {
    connected_initializes_link_state();
    coalesced_reconnect_resets_link_state();
    rx_before_status_preserves_established_link_state();
    return 0;
}

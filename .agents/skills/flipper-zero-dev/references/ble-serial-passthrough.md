# BLE Serial Passthrough

Pocket AirBridge uses a **custom AirBridge Serial-over-BLE GATT service**, not the stock Flipper Serial service.

## AirBridge UUID Family

| Role | UUID |
|---|---|
| Service | `7b871228-baf0-c5b4-5f46-9c2613d627a3` |
| TX (Notify) | `87825ec0-7398-8cb7-3242-b083eaa34f27` |
| RX (Write) | `152f7eeb-e3b7-5898-ba41-7ff66121c98d` |
| Flow control (Notify) | `d2d968bf-cbd8-568f-d24c-5bbddb824f25` |
| Status (Notify/Read/Write) | `bebb7113-63db-bbae-bb45-37dbbf73b6b3` |

## TX Uses NOTIFY, Not INDICATE

TX uses **Notify** (`GATT_CHAR_UPDATE_SEND_NOTIFICATION`, 0x01). Using Indicate (0x02) on a Notify characteristic causes zero notifications — a root-caused GATT stack behavior bug where the peer silently ignores the wrong notification type.

RX accepts Write commands.

Keep the browser UUID alternatives in `web/airbridge-transports.js` because firmware byte ordering and browser canonicalization have differed across builds.

## bt_service Raw Callback Hook

The custom BLE profile is `airbridge_profile.c` in `lib/ble_profile/extra_profiles/`. The `bt_service` raw serial hook routes to the AirBridge serial service (not the stock Serial service):

```c
typedef uint16_t (*BtRawSerialCallback)(
    const uint8_t* data,
    uint16_t len,
    void* context);

void bt_set_raw_serial_callback(BtRawSerialCallback cb, void* context);
bool bt_serial_tx(const uint8_t* data, uint16_t len);
```

When a raw callback is installed, invoke it before `rpc_session_feed`. TX must use the active AirBridge Serial profile owned by `bt_service`. Unregister the callback on FAP exit so normal RPC behavior returns.

## Bridge Event Queue Pattern

The bridge copies callback data into its event queue and performs transport sends only from the main loop. GAP/USB callbacks must not block or call the opposite transport directly.

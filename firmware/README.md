# Pocket AirBridge — Firmware Files

**Regenerated 2026-07-24:** This bundle captures the firmware tree through
`8ba53421` (`fix: block BLE identity downgrade via RPC-status char; correct
fallback mfg data to HP`) plus the bonded advertising working-tree changes,
relative to pristine upstream `dev` base `c9ab2b68`. The bundle advertises the
AirBridge serial UUID continuously, advertises HIDS only for the BLE Deploy
prompt and typing window, and enables persistent numeric-comparison bonding.

This directory contains everything needed to reproduce the Flipper Zero side of
Pocket AirBridge on a fresh checkout of the official firmware
(`flipperdevices/flipperzero-firmware`, `dev` branch, commit `c9ab2b68`).

## Contents

| Path | What it is |
|---|---|
| `airbridge-firmware.patch` | All firmware changes except `api_symbols.csv`: the USB composite profile, AirBridge BLE profile/services, raw serial routing, GATT capacity/error handling, and bonded windowed advertising fixes. |
| `api-symbols-additions.patch` | The matching `targets/f7/api_symbols.csv` changes, including `ble_profile_airbridge`, its keyboard/mouse/consumer-report exports, and the advertising-window API. |
| `pocket_airbridge/` | The FAP itself (`application.fam`, `pocket_airbridge.c`, `icon.png`). Copy to `applications_user/pocket_airbridge/` in the firmware tree. |

## Apply

```bash
cd flipperzero-firmware
git checkout -b pocket-airbridge c9ab2b68    # optional but recommended
git apply /path/to/airbridge-firmware.patch
git apply /path/to/api-symbols-additions.patch
cp -r /path/to/pocket_airbridge applications_user/
./fbt build APPSRC=applications_user/pocket_airbridge
```

Deploy and launch per `docs/firmware-guide.md` (Option A: `storage.py` + `runfap.py`).

## What the patches do (and why upstream doesn't provide it)

### Composite impersonation framework (`usb_airbridge`)

The `usb_airbridge` USB HID profile is not a single device — it is a **selectable
impersonation framework** with 5 profiles:

| Profile | VID | PID | Has keyboard | Identity string |
|---|---|---|---|---|
| Logitech Kbd+Vendor | `0x046D` | `0xB348` | Yes | Logitech Keyboard K380 |
| Dell Kbd+Vendor | `0x413C` | `0x2113` | Yes | Dell Keyboard KB716 |
| Microsoft Kbd+Vendor | `0x045E` | `0x07DC` | Yes | Microsoft Keyboard |
| Microsoft Vendor-Only | `0x045E` | `0x0828` | No | Microsoft USB Device |
| HP Kbd+Vendor | `0x03F0` | `0x5341` | Yes | HP Wireless Keyboard |

Each profile is a **composite HID device**: a standard keyboard collection
(used only by the explicit USB Deploy flow) plus a vendor-defined collection on
usage page `0xFF00` (the data channel in Bridge and Deploy modes). No keyboard
reports are emitted in Bridge mode.

The **always-on identity** model means the selected profile is applied once at
app start and held until the app exits. There is **no runtime switching**:
composite-to-composite reconfiguration is fatal on this USB stack (the device
disappears silently and requires a physical reset. The profile is selected from
`/ext/apps_data/pocket_airbridge/config` (default: `hp_kbd_vendor`).

The USB Deploy flow uses the keyboard collection to type `web/bootstrap.js`
into the target PC's browser DevTools console. The payload is ASCII-only, typed
with US scancodes, and requires a US keyboard layout on the target. The full
emission shows `TYPING...` on the Flipper screen; pressing BACK aborts
instantly.

### AirBridge BLE impersonation profile

The new `ble_profile_airbridge` composes Battery, Device Information Service
(DIS), Human Interface Device Service (HIDS), and the AirBridge serial service.
HIDS exposes keyboard, mouse, and consumer report maps so the explicit **DOWN
= BLE Deploy** flow can type its bootstrap; Bridge mode sends no HID keyboard
reports. The serial UUID is advertised continuously; HIDS is advertised only
while the BLE Deploy prompt or typing screen is active.

The FAP loads the BLE identity from
`/ext/apps_data/pocket_airbridge/config`. The supported keys are:

```ini
ble_name = HP 725 K+M
ble_mac = 3C:52:82:00:00:01
ble_appearance = 0x03C1
ble_mfg_company = 0x0065
ble_mfg_hex =
ble_dis_mfr = HP
ble_dis_model = HP 725 K+M
ble_dis_serial = HP5341KBD01
ble_dis_pnp = 0x0126
```

`ble_name`, `ble_mac`, GAP appearance, manufacturer data, and DIS values are
applied to the AirBridge profile. The configured MAC must use an allowlisted HP
OUI; invalid identity input falls back atomically to compiled HP defaults and
the FAP displays `WARN: BLE ID DEFAULT`. The profile has `bonding_mode = true`:
the first pairing uses numeric comparison and persists the bond for silent later
reconnects. HIDS (`0x1812`) stays in the GATT table but is advertised only in
the BLE Deploy prompt and typing window.

The browser-facing serial UUIDs are generated into
`web/airbridge-identity.js` in on-air byte order. The service is
`7b871228-baf0-c5b4-5f46-9c2613d627a3`; TX is the notify characteristic
`87825ec0-7398-8cb7-3242-b083eaa34f27`; RX is the write characteristic
`152f7eeb-e3b7-5898-ba41-7ff66121c98d`.

### BLE raw-serial routing

`bt_set_raw_serial_callback` / `bt_serial_tx` route the AirBridge serial
service's raw RX and TX through the FAP. Upstream routes its stock serial
service to RPC only. AirBridge TX uses GATT notifications, not indications.

### `gap.c` fix

Retains fast advertising and fixes AdvFast-to-AdvFast restart. It keeps the
AirBridge serial UUID in the advertising packet, moves name and manufacturer
data to the scan response, and adds or removes HIDS without changing the serial
identity. The FAP restarts advertising after central disconnects and disconnects
an idle BLE Deploy Waiting central after 15 seconds, preventing a bonded macOS
HID daemon from starving a new browser picker.

## If `api-symbols-additions.patch` doesn't apply cleanly

The `api_symbols.csv` file is version-specific. If the patch fails to apply:

```bash
./fbt build APPSRC=applications_user/pocket_airbridge
# build fails with "API version is still WIP" — expected
# edit targets/f7/api_symbols.csv: change any remaining ? to +
# re-run the build
```

## FAP behaviour

The FAP is a stateless byte relay: USB/GAP callbacks enqueue `BridgeEvent`s, the
main loop forwards USB→BLE via `bt_serial_tx` and BLE→USB via zero-padded
64-byte `furi_hal_hid_vendor_send_response`, with on-screen counters and a
500 ms LED heartbeat. From Bridge, **UP** starts USB Deploy
(`bootstrap.js` → `app-usb.html`) and **DOWN** starts BLE Deploy
(`bootstrap-ble.js` → `app-ble.html`), opening the HIDS advertising window for
the prompt and typing flow only.

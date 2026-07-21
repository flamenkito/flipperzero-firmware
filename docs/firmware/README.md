# Pocket AirBridge — Firmware Files

**Staleness note (2026-07-21 — regenerated):** The patch bundle in this directory
captures the CURRENT state of the firmware tree at `~/projects/flipperzero-firmware`.
It supersedes the original single-profile patch and reflects the composite
impersonation framework (5 profiles including HP `0x03F0:0x5341`, always-on,
no runtime switching). The patches apply cleanly on a pristine dev checkout
(verified 2026-07-21).

This directory contains everything needed to reproduce the Flipper Zero side of
Pocket AirBridge on a fresh checkout of the official firmware
(`flipperdevices/flipperzero-firmware`, `dev` branch, commit `c9ab2b68`).

## Contents

| Path | What it is |
|---|---|
| `airbridge-firmware.patch` | All firmware changes: `usb_airbridge` composite HID profile (2 new files), BLE raw-serial hook (`bt_set_raw_serial_callback`, `bt_serial_tx`) in `bt_service`, and the `gap.c` advertising-persistence fix. |
| `api-symbols-additions.patch` | `targets/f7/api_symbols.csv` additions and removals (version-specific — see below if it doesn't apply cleanly). |
| `pocket_airbridge/` | The FAP itself (`application.fam`, `pocket_airbridge.c`, `icon.png`). Copy to `applications_user/pocket_airbridge/` in the firmware tree. |

## Apply

```bash
cd flipperzero-firmware
git checkout -b pocket-airbridge            # optional but recommended
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
(used only in Deploy mode) plus a vendor-defined collection on usage page
`0xFF00` (used for the data channel in both Bridge and Deploy modes). The
keyboard collection is never present in Bridge mode — it activates only during
the Deploy flow.

The **always-on identity** model means the selected profile is applied once at
app start and held until the app exits. There is **no runtime switching**:
composite-to-composite reconfiguration is fatal on this USB stack (the device
disappears silently and requires a physical reset. The profile is selected from
`/ext/apps_data/pocket_airbridge/config` (default: `hp_kbd_vendor`).

The Deploy flow uses the keyboard collection to type `web/bootstrap.js` into the
target PC's browser DevTools console. The payload is ASCII-only, typed with US
scancodes, and requires a US keyboard layout on the target. The full emission
shows `TYPING...` on the Flipper screen; pressing BACK aborts instantly.

### BLE raw-serial hook

`bt_set_raw_serial_callback` / `bt_serial_tx`: intercepts Serial-service RX
before the RPC session and exposes raw TX. Upstream routes all serial data to
RPC only.

### `gap.c` fix

Keeps fast advertising instead of dropping to low-power after the advertise
timer, and fixes AdvFast→AdvFast restart. Needed so the browser can reliably
discover and stay connected to the Flipper during the demo.

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
64-byte `furi_hal_hid_vendor_send_response`, with on-screen counters and a 500 ms
LED heartbeat. In Deploy mode it additionally drives keyboard reports via
`furi_hal_hid_airbridge_kb_press` / `kb_release` / `kb_release_all`.

# Pocket AirBridge — Firmware Files

This directory contains everything needed to reproduce the Flipper Zero side of
Pocket AirBridge on a fresh checkout of the official firmware
(`flipperdevices/flipperzero-firmware`, `dev` branch).

## Contents

| Path | What it is |
|---|---|
| `airbridge-firmware.patch` | All firmware changes: `usb_airbridge` vendor HID profile (2 new files), BLE raw-serial hook (`bt_set_raw_serial_callback`, `bt_serial_tx`) in `bt_service`, and the `gap.c` advertising-persistence fix. Generated against commit `c9ab2b68`; the patched files are untouched by upstream since, so it applies on latest `dev`. |
| `api-symbols-additions.patch` | `targets/f7/api_symbols.csv` additions. Kept separate because this file is version-specific — if it doesn't apply cleanly, build once, let `fbt` regenerate the `?` entries, and change them to `+` (see `docs/firmware-guide.md`). |
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

- **`usb_airbridge`**: vendor-defined HID interface (usage page `0xFF00`, 64-byte
  bidirectional reports, VID/PID `0x0483/0x5742`). Upstream only ships
  `cdc_single/cdc_dual/hid/hid_u2f` — no custom HID mechanism.
- **BLE raw-serial hook**: intercepts Serial-service RX before the RPC session and
  exposes raw TX. Upstream routes all serial data to RPC only.
- **`gap.c` fix**: keeps fast advertising instead of dropping to low-power after the
  advertise timer, and fixes AdvFast→AdvFast restart. Needed so the browser can
  reliably discover and stay connected to the Flipper during the demo.

The FAP is a stateless byte relay: USB/GAP callbacks enqueue `BridgeEvent`s, the
main loop forwards USB→BLE via `bt_serial_tx` and BLE→USB via zero-padded
64-byte `furi_hal_hid_vendor_send_response`, with on-screen counters and a 500 ms
LED heartbeat.

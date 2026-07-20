# Pocket AirBridge — Flipper Zero Firmware Guide

## What We Built

You already have `~/projects/flipperzero-firmware`. We added:

1. **New USB HID profile** `usb_airbridge` (bidirectional 64-byte vendor HID, based on the U2F profile)
2. **BLE Serial passthrough patch** in `bt_service/bt.c` to let our FAP intercept raw serial data
3. **FAP** `pocket_airbridge` in `applications_user/pocket_airbridge/`, including `icon.png` wired through `fap_icon`

**Starting from a pristine firmware checkout instead?** Everything above is bundled in
[`docs/firmware/`](firmware/): `airbridge-firmware.patch` (HAL + BT + GAP changes),
`api-symbols-additions.patch`, and the FAP source. See `docs/firmware/README.md` for
apply instructions. The patches were audited against latest upstream `dev` on
2026-07-20: upstream still has no raw-serial API and no custom HID mechanism, and
has not touched the patched files since, so they apply cleanly.

The firmware is a dumb, stateless byte relay between the two transports. The
Flipper never parses protocol frames and never buffers more than the small
in-flight event queue.

### Verified on hardware (2026-07-20)

- Bidirectional text messages and 38-byte attachments, SHA-256 verified in both
  directions.
- Larger transfers completed: 5 KB and 20 KB attachments.
- Sender-side cancel and receiver-side cancel both verified mid-transfer.
- On-screen counters: `U->B` and `B->U` increment symmetrically with traffic;
  `DROP: 0` and `TXERR: 0` during healthy transfers. A single `TXERR` was
  observed once during a mid-flight cancel race and is benign (see
  Troubleshooting).

## Files Changed

```
flipperzero-firmware/
├── applications_user/
│   └── pocket_airbridge/
│       ├── application.fam
│       ├── icon.png
│       └── pocket_airbridge.c
├── targets/f7/furi_hal/
│   └── furi_hal_usb_airbridge.c          (NEW)
├── targets/furi_hal_include/
│   ├── furi_hal_usb.h                    (add usb_airbridge extern)
│   └── furi_hal_usb_airbridge.h        (NEW)
├── applications/services/bt/bt_service/
│   ├── bt.c                              (raw serial hook + bt_serial_tx)
│   ├── bt.h                              (public API declarations)
│   └── bt_i.h                            (note about public declarations)
└── targets/f7/api_symbols.csv          (export new symbols)
```

## Build

```bash
cd ~/projects/flipperzero-firmware
./fbt build APPSRC=applications_user/pocket_airbridge
```

If this is the first build after adding the USB profile, the firmware will detect new API symbols. The build script will update `api_symbols.csv` with `?` markers. Change them to `+` and re-run the build command (this was already done during setup).

## Deploy

### Option A: Build, Upload, and Launch with Official Scripts (Verified)

```bash
cd ~/projects/flipperzero-firmware
./fbt build APPSRC=applications_user/pocket_airbridge

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  build/f7-firmware-D/.extapps/pocket_airbridge.fap \
  /ext/apps/USB/pocket_airbridge.fap

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
  /ext/apps/USB/pocket_airbridge.fap

python3 scripts/runfap.py -p /dev/cu.usbmodemflip_Luwot1 \
  -s build/f7-firmware-D/.extapps/pocket_airbridge.fap \
  -t /ext/apps/USB/pocket_airbridge.fap
```

The `size` command should report the uploaded FAP size. In the verified build it reported `3776` bytes.

When `runfap.py` launches this app, it may end with:

```text
Error: read failed: [Errno 6] Device not configured
```

For Pocket AirBridge this is expected after launch: the app switches USB away from CDC serial into the custom HID device, so the serial transport used by `runfap.py` disappears.

**Redeploying while the app is running:** `storage.py` and `runfap.py` need
`/dev/cu.usbmodemflip_*`, which only exists when the AirBridge app is **not**
running. Exit the app first (BACK button on the Flipper) so the serial port
reappears, then redeploy. If the app is wedged and BACK does not exit, restart
the Flipper (hold LEFT + BACK); a restart always brings the port back.

Verify launch with macOS USB enumeration:

```bash
ioreg -p IOUSB -l | grep -A25 -i "Pocket AirBridge\|5742"
```

Expected values include `USB Product Name = "Pocket AirBridge"`, `idVendor = 1155` (`0x0483`), and `idProduct = 22338` (`0x5742`).

### Option B: `./fbt launch`

```bash
./fbt launch APPSRC=applications_user/pocket_airbridge
```

This compiles, uploads via USB, and runs the app automatically when the serial transport remains available long enough. For USB-mode-switching apps, the explicit `storage.py` + `runfap.py` flow above gives clearer failure/verification points.

### Option C: Build Only

```bash
./fbt build APPSRC=applications_user/pocket_airbridge
```

Then copy the `.fap` manually:
```bash
cp dist/f7/C/apps/USB/pocket_airbridge.fap /Volumes/Flipper/apps/USB/
```

## Revert Firmware Changes

```bash
cd ~/projects/flipperzero-firmware
git checkout targets/f7/furi_hal/furi_hal_usb_airbridge.c
git checkout targets/furi_hal_include/furi_hal_usb_airbridge.h
git checkout targets/furi_hal_include/furi_hal_usb.h
git checkout applications/services/bt/bt_service/bt.c
git checkout applications/services/bt/bt_service/bt.h
git checkout applications/services/bt/bt_service/bt_i.h
git checkout targets/f7/api_symbols.csv
rm -rf applications_user/pocket_airbridge
```

## How It Works

### USB Side

Our new `usb_airbridge` profile is a **vendor-defined HID device**:
- Usage Page: `0xFF00` (Vendor Defined)
- Usage: `0x01`
- IN endpoint `0x81` (Flipper → PC)
- OUT endpoint `0x01` (PC → Flipper)
- Packet size: **64 bytes**
- VID/PID: `0x0483 / 0x5742`

The sender page connects to it via WebHID using the VID/PID filter.

### BLE Side

We patched `bt_service` to expose two new functions:
- `bt_set_raw_serial_callback(cb, ctx)` — intercepts all BLE Serial RX data before the RPC system sees it
- `bt_serial_tx(data, len)` — sends raw bytes over BLE Serial

The chat page connects to the existing **Flipper Serial-over-BLE** service using these browser-canonical UUIDs:

| Role | UUID | Direction |
|------|------|-----------|
| Service | `8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000` | — |
| TX (Indicate) | `19ed82ae-ed21-4c9d-4145-228e61fe0000` | Flipper → Browser |
| RX (Write) | `19ed82ae-ed21-4c9d-4145-228e62fe0000` | Browser → Flipper |

The firmware source lists the same UUID bytes in controller byte order in
`targets/f7/ble_glue/services/serial_service_uuid.inc`; Web Bluetooth exposes
the browser-canonical UUID strings above.

### Bridge Logic (Inside the FAP)

The FAP is a queue-driven, stateless byte relay. All forwarding happens in the
FAP main loop; the USB and GAP callbacks never touch the other transport's HAL
directly.

1. **Event queue** — a `FuriMessageQueue` of 8 `BridgeEvent` structs
   (`type`, 64-byte `data`, `len`, `to_ble` direction flag).
2. **USB callback** (`usb_event_callback`) — on a HID OUT report, copies the
   bytes into a `BridgeEvent` and enqueues it with timeout `0` (callback
   context must not block). Connect/disconnect events are posted the same way.
3. **BLE callback** (`ble_raw_serial_callback`) — runs on the GAP thread,
   copies inbound bytes into a `BridgeEvent` and enqueues with timeout `0`.
   Returns the remaining accept capacity (`64` on enqueue, `0` when the queue
   was full and the frame was dropped).
4. **Main loop forwarding** — the only place transport HAL calls happen:
   - USB→BLE: `bt_serial_tx(data, len)`. On failure the `TXERR` counter
     increments; there is no spin-retry (protocol-level ACK backpressure
     handles loss).
   - BLE→USB: the payload is copied into a zero-filled 64-byte buffer and sent
     with `furi_hal_hid_vendor_send_response(report, 64)`. Host HID drivers
     may not deliver short IN packets, so every response is padded to the full
     report size.
5. **Drop accounting** — if either callback fails to enqueue (queue full), a
   `DROP` counter increments. With ACK-per-frame backpressure there is at most
   ~1 frame in flight per direction, so any non-zero `DROP` indicates a real
   problem.
6. **Status screen** — shows `USB: CONNECTED/--`, `BLE: ACTIVE/--`, and the
   counters `U->B`, `B->U`, `DROP`, `TXERR`. The green LED blinks every
   500 ms as a heartbeat while the app runs.
7. **Graceful USB failure** — if `furi_hal_usb_set_config` fails at startup,
   the app shows `ERR: CONFIG` on screen instead of crashing (a crash here
   would wedge USB until reboot).

The Flipper does **not** parse protocol frames, enforce half-duplex, or
implement the bridge state machine sketched in early docs. Half-duplex
discipline (one item in flight, lower `itemId` wins) is enforced entirely by
the browser endpoints.

## Browser Setup

Both pages must be served from a **secure origin** (`https://` or `localhost`):

```bash
cd ~/projects/flipper-hid/web
python3 -m http.server 8080
```

Then open:
- `http://localhost:8080/chat-usb.html` on PC-A (WebHID chat page)
- `http://localhost:8080/chat-ble.html` on PC-B (Web Bluetooth chat page)

Legacy file-transfer-only pages are still available:
- `http://localhost:8080/sender.html` on PC-A
- `http://localhost:8080/receiver.html` on PC-B

## Step-by-Step Chat Demo Script

1. **Build and deploy the FAP** (see Deploy section above).
2. **Launch the app** on Flipper Zero. The screen should show USB and BLE status.
3. **Connect USB** from Flipper Zero to PC-A.
4. **Open `chat-usb.html`** on PC-A in Chrome/Edge. Click **Connect USB** and select the Pocket AirBridge device.
5. **Pair Bluetooth** on PC-B with Flipper Zero.
6. **Open `chat-ble.html`** on PC-B in Chrome/Edge. Click **Connect BLE** and select the Flipper Zero device.
7. **Send a text message** from PC-A. Type "Hello from USB" and click **Send**. PC-B should display the message in the chat transcript.
8. **Reply from PC-B**. Type "Hello from BLE" and click **Send**. PC-A should display the reply.
9. **Send an attachment** from PC-A. Select a small text file or image and click **Send File**. PC-B should show a progress bar and then offer the file for download.
10. **Cancel a transfer**. Start sending a large attachment from either side, then click **Cancel**. The other side should show "Transfer cancelled" and return to idle.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `API version is still WIP` | Edit `targets/f7/api_symbols.csv` — change `?` to `+` for new entries, then rebuild |
| `app may not be runnable. Symbols not resolved` | Make sure new functions are declared in `bt.h` and added to `api_symbols.csv` |
| WebHID can't open device | Check that the chat-usb page uses `vendorId: 0x0483, productId: 0x5742` and is on `localhost` or `https` |
| Web Bluetooth can't find Flipper | Make sure the Flipper is advertising (app running) and Bluetooth is enabled on PC-B. Use Chrome/Edge |
| Web Bluetooth selects Flipper but says Serial service UUID is missing | Refresh `chat-ble.html`; the browser must use the canonical UUIDs (`8fe5...`, `19ed...`), not the firmware byte-order strings |
| Transfer is slow | Expected — 64-byte chunks at ~50-100 Hz is normal for BLE HID demo throughput |
| Cancel doesn't work | Make sure both sides run the latest chat pages that handle `MSG.CANCEL` and `AbortController` |
| Flipper reboots | Increase `stack_size` in `application.fam` (try `3 * 1024`) |
| `storage.py` hangs | Stop stale serial clients, then physically unplug/replug Flipper USB and retry after it returns to the desktop |
| `runfap.py` ends with `Device not configured` | Expected for Pocket AirBridge after launch because the app switches USB from CDC serial to custom HID |
| `storage.py` can't find `/dev/cu.usbmodemflip_*` | The AirBridge app is still running — exit it (BACK) or restart the Flipper so the serial port reappears |
| `TXERR` counter is non-zero | One `TXERR` can occur during a mid-flight cancel race (a frame reaches the main loop after the peer went away); benign if the transfer error is visible on both pages. Persistent `TXERR` growth means the BLE link is down — reconnect PC-B |
| App is missing an icon | Add `applications_user/pocket_airbridge/icon.png` and `fap_icon="icon.png"` in `application.fam`, then rebuild/redeploy |

## Important Notes

- **This modifies core firmware files.** After the hackathon, run the `git checkout` commands above to restore normal behavior.
- **The BLE Serial service is shared.** While Pocket AirBridge is running, qFlipper/mobile app RPC will NOT work because we intercept serial data. This is expected for the demo.
- **The WebHID vendor profile** (`usb_airbridge`) is registered as a separate USB mode. When the app exits, it restores the previous USB mode (usually `usb_cdc_dual`).

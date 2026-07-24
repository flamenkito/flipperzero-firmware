# Pocket AirBridge — Flipper Zero Firmware Guide

## What We Built

You already have `~/projects/flipperzero-firmware`. We added:

1. **New USB HID profile** `usb_airbridge` (bidirectional 64-byte vendor HID, based on the U2F profile)
2. **AirBridge BLE impersonation profile** (Battery + DIS + HIDS + AirBridge serial), config-driven identity, and `bt_service` routing so the FAP can intercept raw serial data
3. **FAP** `pocket_airbridge` in `applications_user/pocket_airbridge/`, including `icon.png` wired through `fap_icon`

**Starting from a pristine firmware checkout instead?** Everything above is bundled in
[`firmware/`](../firmware/): `airbridge-firmware.patch` (USB, BLE-profile, BT, and GAP changes),
`api-symbols-additions.patch`, and the FAP source. See `firmware/README.md` for
apply instructions. The bundle is regenerated from the firmware tree through
`8ba53421`, plus the bonded advertising working-tree changes, against pristine
upstream `dev` base `c9ab2b68`; apply the firmware patch, then the API-symbol
patch, before copying the FAP.

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

## Key Files Changed

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
│   ├── bt.c                              (AirBridge raw serial routing)
│   └── bt.h                              (public API declarations)
├── lib/ble_profile/extra_profiles/
│   └── airbridge_profile.c               (HIDS + DIS + serial BLE profile)
├── targets/f7/ble_glue/services/
│   ├── airbridge_dev_info_service.c      (config-driven DIS)
│   └── airbridge_serial_service.c        (AirBridge serial notify service)
└── targets/f7/api_symbols.csv          (export new symbols)
```

## Build

```bash
cd ~/projects/flipperzero-firmware
./fbt build APPSRC=applications_user/pocket_airbridge
```

If this is the first build after adding the USB profile, the firmware will detect new API symbols. The build script will update `api_symbols.csv` with `?` markers. Change them to `+` and re-run the build command (this was already done during setup).

## Flashing Firmware

On a normally-booted, USB-connected Flipper:

```bash
cd ~/projects/flipperzero-firmware
./fbt flash_usb
```

This bundles a self-update package, uploads it over the serial CLI to `/ext/update/f7-update-local/`, and the Flipper reboots into its own updater to flash itself. The serial port disappears for roughly 30 to 60 seconds during the self-update, then reappears on its own. No DFU button combo, no ST-Link, no qFlipper needed. Manual DFU is a recovery path for bricked devices only, not a prerequisite for normal flashing.

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

**Canonical location:** the FAP lives ONLY at `/ext/apps/USB/pocket_airbridge.fap` (the `fap_category="USB"` menu location). Never deploy a copy to `/ext/apps/` root or another category folder — a second copy goes stale silently and the menu can launch the old build. Before deploying, check for duplicates:
```bash
python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 list /ext/apps | grep -i airbridge
# must print exactly: /ext/apps/USB/pocket_airbridge.fap
# remove strays with: python3 scripts/storage.py -p <port> remove <path>
```

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

(These values describe the original development identity; the shipped FAP impersonates full device personalities and never exposes VID `0x0483` on the bus while running.)

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

## Deploy App Files to the SD Card

The Deploy flow reads its files from `/ext/apps_data/pocket_airbridge/` on the SD card:

| SD path | Source | Role |
|---|---|---|
| `/ext/apps_data/pocket_airbridge/bootstrap.js` | `web/bootstrap.js` | The typed snippet. ASCII-only, 1,104 characters. The snippet carries a WebHID filter list that enumerates every profile VID/PID (Logitech, Dell, MSFT, HP), so it matches whichever impersonation is active; on the target machine only the Flipper is present. |
| `/ext/apps_data/pocket_airbridge/bootstrap-ble.js` | `web/bootstrap-ble.js` | The BLE twin of `bootstrap.js` (1,701 characters), typed character-by-character through BLE HIDS during a BLE Deploy run. It opens Web Bluetooth, subscribes to the AirBridge serial TX **notify** characteristic, writes the `0x42` request, and boots the streamed app. |
| `/ext/apps_data/pocket_airbridge/app-usb.html` | `dist/app-usb.html` | The single-file WebHID app bundle streamed by **UP = USB Deploy**. |
| `/ext/apps_data/pocket_airbridge/app-ble.html` | `dist/app-ble.html` | The single-file Web Bluetooth app bundle streamed by **DOWN = BLE Deploy**. |

Build both bundles, then send all four deploy artifacts. The AirBridge app must
NOT be running while you do this (the serial port only exists when the app is
exited):

```bash
cd ~/projects/flipper-hid
python3 tools/build_bundle.py

cd ~/projects/flipperzero-firmware
python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  ~/projects/flipper-hid/web/bootstrap.js \
  /ext/apps_data/pocket_airbridge/bootstrap.js

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  ~/projects/flipper-hid/web/bootstrap-ble.js \
  /ext/apps_data/pocket_airbridge/bootstrap-ble.js

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  ~/projects/flipper-hid/dist/app-usb.html \
  /ext/apps_data/pocket_airbridge/app-usb.html

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
  /ext/apps_data/pocket_airbridge/app-usb.html

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  ~/projects/flipper-hid/dist/app-ble.html \
  /ext/apps_data/pocket_airbridge/app-ble.html

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
  /ext/apps_data/pocket_airbridge/app-ble.html
```

The `size` commands confirm that both transport-matched bundles were uploaded.

**Timing matters:** while a composite profile is active (Bridge or Deploy mode) the Flipper has NO serial port at all, and `storage.py` fails with `Failed to resolve port`. Deploy these files BEFORE entering Bridge/Deploy, or exit the app to restore the CLI, then send them.

## USB Personalities and the Deploy Flow

While the app runs, the Flipper never exposes its real USB identity (STM32 VID `0x0483`) on the bus. Every personality is a full impersonation of a legitimate device: VID/PID, strings, and descriptor shape. The primary profile is `hp_kbd_vendor`, an HP "Wireless Keyboard and Mouse" dongle (VID `0x03F0`, PID `0x5341`): a composite with a keyboard collection (used only by the Deploy flow) and a vendor-defined collection on usage page `0xFF00` (the AirBridge data channel).

The active profile is selected once, by `/ext/apps_data/pocket_airbridge/config`:

```ini
# /ext/apps_data/pocket_airbridge/config — active profile label
profile=hp_kbd_vendor
```

`hp_kbd_vendor` is the default; the app uses it when the config file is absent. Writing the file explicitly is optional:

```bash
printf 'profile=hp_kbd_vendor\n' > /tmp/airbridge-config
python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  /tmp/airbridge-config /ext/apps_data/pocket_airbridge/config
```

**There is no runtime profile switching.** The profile line in the FAP menu is display-only. This was removed by design on 2026-07-20 after hardware testing showed that CDC→composite apply works, but composite→composite reconfiguration (switching from one impersonation profile to another) is fatal on this USB stack: the device dies silently and needs a physical reset. Analysis pinned a definite endpoint-number collision (vendor IN `0x82` / OUT `0x02` shared endpoint index 2) plus risky manual endpoint teardown in deinit. The configured profile is applied automatically a short moment after app start and is held until app exit (always-on); the previous USB mode is restored only on app exit.

### The Deploy Flow

1. From the Bridge screen, press **UP** for USB Deploy or **DOWN** for BLE Deploy.
2. For BLE Deploy, the HIDS advertising window opens for the entire prompt and typing interaction. On the target PC, pair `HP 725 K+M` if this is the first use, then open a tab at `https://blank.org` (not `about:blank` — some Chrome builds report `window.isSecureContext === false` there; blank.org loads from browser cache offline).
3. On OK, the FAP types `bootstrap.js` over USB or `bootstrap-ble.js` over BLE HIDS. The `TYPING via USB/BLE` screen shows a determinate progress bar (chars typed / total, plus %) for the entire emission; BACK aborts instantly. HIDS stops advertising when BLE typing ends or the deploy flow is aborted; the AirBridge serial UUID remains advertised throughout.
4. The executed bootstrap paints a landing page. While it waits, the FAP shows `Waiting for browser...` with an indeterminate marquee (a block bouncing across the bar frame) and the hint `Click Connect in the browser`. Clicking **Connect** supplies the browser user gesture, opens the matching WebHID or Web Bluetooth transport, and sends `0x42`. During BLE Deploy Waiting, a central that has not requested the bundle is disconnected after 15 seconds and advertising resumes, preventing a bonded macOS HID connection from starving a new browser picker.
5. The FAP streams the length+checksum header and transport-matched `app-usb.html` or `app-ble.html` bundle (see [protocol.md](protocol.md), "Bootstrap Stream Protocol"). The `Serving app via USB/BLE` screen shows a determinate progress bar (KB sent / total, plus %); BACK aborts.
6. The screen shows `Done` and returns to Bridge.

USB Deploy requires a kbd+vendor USB profile. BLE Deploy uses the AirBridge HIDS
keyboard report and does not depend on the USB keyboard collection.

## Revert Firmware Changes

```bash
cd ~/projects/flipperzero-firmware
rm targets/f7/furi_hal/furi_hal_usb_airbridge.c
rm targets/furi_hal_include/furi_hal_usb_airbridge.h
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

**Note:** the values above describe the original development identity. The shipped FAP impersonates full device personalities and never exposes VID `0x0483` on the bus while running; see "USB Personalities and the Deploy Flow" above. The `0x0483/0x5742` identity is retired and not used by the live firmware.

### BLE Side

We patched `bt_service` to expose two new functions:
- `bt_set_raw_serial_callback(cb, ctx)` — intercepts all BLE Serial RX data before the RPC system sees it
- `bt_serial_tx(data, len)` — sends raw bytes over BLE Serial

The chat page connects to the AirBridge serial service using the on-air UUIDs
generated in `web/airbridge-identity.js`:

| Role | UUID | Direction |
|------|------|-----------|
| Service | `7b871228-baf0-c5b4-5f46-9c2613d627a3` | — |
| TX (Notify) | `87825ec0-7398-8cb7-3242-b083eaa34f27` | Flipper → Browser |
| RX (Write) | `152f7eeb-e3b7-5898-ba41-7ff66121c98d` | Browser → Flipper |
| Flow control (Notify) | `d2d968bf-cbd8-568f-d24c-5bbddb824f25` | Flipper → Browser |
| Status (Notify/Read/Write) | `bebb7113-63db-bbae-bb45-37dbbf73b6b3` | Both |

The firmware source keeps the controller-order values in
`targets/f7/ble_glue/services/airbridge_serial_uuid.h`; the browser uses the
byte-reversed on-air strings above. The serial service UUID is advertised at all
times. HIDS is advertised only for the BLE Deploy prompt and typing window, and
the profile includes DIS values from the FAP config. Bonding is enabled: the
first pairing uses MITM numeric comparison and stores a bond for silent later
reconnects.

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
6. **Status screen** — title `Pocket AirBridge` (y=10) with the active USB
   identity line `HID: <profile>` beneath it (y=19). An icon header row
   (y=24) shows two horizontal direction composites — USB plug → arrow → BT
   rune (x=0/9/16) and its mirror BT rune → arrow → USB plug (x=32/39/46) —
   plus a trash-can icon (DROP, x=64) and an alert-triangle icon (TXERR,
   x=96). The four frame counters (`U->B`, `B->U`, `DROP`, `TXERR`) are
   digits left-aligned under each icon group (x=0/32/64/96, y=35). Link state
   is encoded in the glyphs: the USB plug is filled when a USB host is
   connected and an outline when down; the BT rune gains a solid pedestal
   when a BLE central is connected and is the bare rune when down. A deploy
   hint row (up-arrow `USB deploy`, down-arrow `BLE deploy`, y=47-53) sits
   above `BACK: exit` (y=63). The green LED blinks every 500 ms as a
   heartbeat while the app runs.
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

The superseded `sender.html` and `receiver.html` file-transfer pages were removed;
use the chat pages above for all transfers.

### Bonding and macOS

Bonding is on and persists across app sessions. The first pairing on each host
shows a numeric-comparison code; later bonded reconnects are silent. macOS may
auto-reconnect the bonded keyboard through its HID daemon. The BLE Deploy
Waiting screen disconnects a central that does not request the bundle within 15
seconds, then resumes advertising, so a browser picker gets another chance.

For chat, Chrome tries `navigator.bluetooth.getDevices()` before opening a
picker. Enable `chrome://flags/#enable-web-bluetooth-new-permissions-backend`
to retain granted devices across Chrome restarts. A bonded macOS HID connection
can still grip a Bridge-mode link and require an app restart before chat
connects; a Bridge-mode squatter-kick is a known follow-up, not part of the
Deploy timeout.

For passive advertising QA, `python3 scripts/ble_qa_scan.py scan` asserts the
serial UUID alone in Bridge mode. Run the same command with `--expect-hids`
while the BLE Deploy prompt or typing screen is active; it then requires the
serial UUID plus HIDS.

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
| WebHID can't open device | Check that the chat-usb page targets the active impersonation profile and is on `localhost` or `https` |
| Web Bluetooth can't find Flipper | Make sure the Flipper is advertising (app running) and Bluetooth is enabled on PC-B. Use Chrome/Edge |
| Web Bluetooth selects Flipper but says Serial service UUID is missing | Refresh `chat-ble.html`; it must use the generated AirBridge UUIDs (`7b871228...`, `87825ec0...`, `152f7eeb...`), not controller-order UUID bytes |
| Transfer is slow | Expected — 64-byte chunks at ~50-100 Hz is normal for BLE HID demo throughput |
| Cancel doesn't work | Make sure both sides run the latest chat pages that handle `MSG.CANCEL` and `AbortController` |
| Flipper reboots | Increase `stack_size` in `application.fam` (try `3 * 1024`) |
| `storage.py` hangs | Stop stale serial clients, then physically unplug/replug Flipper USB and retry after it returns to the desktop |
| `runfap.py` ends with `Device not configured` | Expected for Pocket AirBridge after launch because the app switches USB from CDC serial to custom HID |
| Flipper unresponsive after app launch or a USB mode switch | Probe with `python3 tools/flipper_alive.py --wait 30` (from this repo). A healthy CLI answers `\r` with a `>:` prompt within 5 s. Port present but silent means the firmware is hung (USB CDC still enumerated, firmware dead). Recovery is a physical reset; do not attempt a DTR-toggle reset from software |
| `storage.py` can't find `/dev/cu.usbmodemflip_*` | The AirBridge app is still running — exit it (BACK) or restart the Flipper so the serial port reappears |
| `TXERR` counter is non-zero | One `TXERR` can occur during a mid-flight cancel race (a frame reaches the main loop after the peer went away); benign if the transfer error is visible on both pages. Persistent `TXERR` growth means the BLE link is down — reconnect PC-B |
| Pairing code dialog vanishes during deploy Connect before it can be confirmed | Fixed 2026-07-23: the deploy Waiting pump now only restarts advertising from GAP-idle and never disconnects, so a pairing code shown during deploy Connect can be confirmed at leisure. Previously the pump force-disconnected every 2.5 s and killed in-progress pairings — redeploy the current FAP |
| App is missing an icon | Add `applications_user/pocket_airbridge/icon.png` and `fap_icon="icon.png"` in `application.fam`, then rebuild/redeploy |

## Important Notes

- **This modifies core firmware files.** After the hackathon, run the `git checkout` commands above to restore normal behavior.
- **The BLE Serial service is shared.** While Pocket AirBridge is running, qFlipper/mobile app RPC will NOT work because we intercept serial data. This is expected for the demo.
- **The WebHID vendor profile** (`usb_airbridge`) is registered as a separate USB mode. When the app exits, it restores the previous USB mode (usually `usb_cdc_dual`).

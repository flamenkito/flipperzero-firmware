# Pocket AirBridge — Flipper Zero Firmware Guide

## What We Built

This one local custom firmware and apps repository is rooted at
`/Users/asutov/projects/flipperzero-firmware`. Pocket AirBridge product assets
live under `airbridge/`; firmware changes and FAP sources live beside the rest of
the firmware tree. It includes:

1. **App-owned USB HID profiles** in `airbridge_usb.c` (bidirectional 64-byte vendor HID)
2. **App-owned BLE impersonation profile** (Battery + DIS + HIDS + AirBridge serial), config-driven identity, and direct FAP serial routing
3. **FAP** `pocket_airbridge` in `applications_user/pocket_airbridge/`, including `icon.png` wired through `fap_icon`

The firmware changes and FAP are canonical in-tree sources. Edit and build them
here. There is no separate product checkout, patch bundle, or patch-application
step.

The firmware is a blind, stateless byte relay between the two transports. The
Flipper never parses application protocol frames, never sees browser session
keys, never stores plaintext or decrypted files, and never buffers more than the
small in-flight event queue.

Chat confidentiality is browser-only. The browsers perform SAS-verified P-256,
HKDF, and AES-GCM, and the Flipper forwards only opaque frames. It never stores
plaintext, session keys, decrypted names, or decrypted files.

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
applications_user/pocket_airbridge/
    pocket_airbridge.c             supervisor and I/O worker lifetime
    airbridge_operation.h          independent progress monitor
    airbridge_usb.c                USB descriptors, endpoints, keyboard reports
    airbridge_profile.c            Battery + DIS + HIDS + serial BLE profile
    airbridge_dev_info_service.c   config-driven DIS
    airbridge_serial_service.c     serial GATT notifications and receive events
    airbridge_serial_uuid.h        canonical UUID values
    airbridge_ble.c                connection and recovery policy
    airbridge_relay.c              eight-slot frame queue and forwarding
```

## Build

```bash
cd /Users/asutov/projects/flipperzero-firmware
./fbt
./fbt fap_pocket_airbridge
python3 airbridge/tests/run_tests.py
```

Firmware and FAP must be rebuilt together at API 87.15. The earlier API 87.14
ownership refactor removed obsolete AirBridge exports without changing the API
major, by explicit local policy. The BLE Deploy restoration adds the HIDS
advertising control; only minor API bumps are permitted. Do not
deploy an older AirBridge FAP against this firmware. Ordinary loader checks remain
enabled. The remaining shared changes are documented in
[firmware-boundary.md](firmware-boundary.md).

## Flashing Firmware

On a normally-booted, USB-connected Flipper:

Before a physical action, play the attention signal and use one `question` tool
gate with a confirmation and cancel option. Do not assume a button press,
unplug, unlock, or picker action occurred. See the root `AGENTS.md` for the
required browser-window and artifact-path rules.

```bash
cd /Users/asutov/projects/flipperzero-firmware
./fbt flash_usb
```

This bundles a self-update package, uploads it over the serial CLI to `/ext/update/f7-update-local/`, and the Flipper reboots into its own updater to flash itself. The serial port disappears for roughly 30 to 60 seconds during the self-update, then reappears on its own. No DFU button combo, no ST-Link, no qFlipper needed. Manual DFU is a recovery path for bricked devices only, not a prerequisite for normal flashing.

## Deploy

### Option A: Build, Upload, and Launch with Official Scripts (Verified)

```bash
cd /Users/asutov/projects/flipperzero-firmware
./fbt build APPSRC=applications_user/pocket_airbridge

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  build/f7-firmware-D/.extapps/pocket_airbridge.fap \
  /ext/apps/Tools/pocket_airbridge.fap

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
  /ext/apps/Tools/pocket_airbridge.fap

python3 scripts/runfap.py -p /dev/cu.usbmodemflip_Luwot1 \
  -s build/f7-firmware-D/.extapps/pocket_airbridge.fap \
  -t /ext/apps/Tools/pocket_airbridge.fap
```

The `size` command must match the local FAP's current byte size; do not compare
against a historical build's size.

**Canonical location:** the FAP lives ONLY at `/ext/apps/Tools/pocket_airbridge.fap` (the `fap_category="Tools"` menu location). Never deploy a copy to `/ext/apps/` root or another category folder — a second copy goes stale silently and the menu can launch the old build. Before deploying, check for duplicates:
```bash
python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 list /ext/apps | grep -i airbridge
# must print exactly: /ext/apps/Tools/pocket_airbridge.fap
# remove strays with: python3 scripts/storage.py -p <port> remove <path>
```

When `runfap.py` launches this app, it may end with:

```text
Error: read failed: [Errno 6] Device not configured
```

For Pocket AirBridge this is expected after launch: the app switches USB away from CDC serial into the custom HID device, so the serial transport used by `runfap.py` disappears.

**Redeploying while the app is running:** `storage.py` and `runfap.py` need
`/dev/cu.usbmodemflip_*`, which only exists when the AirBridge app is **not**
running. Exit the app first (long BACK on the Flipper) so the serial port
reappears, then redeploy. If the app is wedged and long BACK does not exit, restart
the Flipper (hold LEFT + BACK); a restart always brings the port back.

Verify the running app with macOS USB enumeration. The shipped FAP exposes the
selected impersonation profile, not the Flipper's STM32 development identity:

```bash
ioreg -p IOUSB -l | grep -A25 -i "HP Wireless Keyboard and Mouse\|03f0\|5341"
```

For the default `hp_kbd_vendor` profile, expected values include manufacturer
`HP`, product `HP Wireless Keyboard and Mouse`, `idVendor = 1008` (`0x03F0`),
and `idProduct = 21313` (`0x5341`). If you select another profile in
`/ext/apps_data/pocket_airbridge/config`, verify that configured profile's
VID/PID and strings instead.

Historical note: early development builds enumerated as `Pocket AirBridge` with
STM32 VID `0x0483` and PID `0x5742`. That identity is retired for shipped
impersonation builds and must not appear on the bus while the app runs.

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
cp dist/f7/C/apps/Tools/pocket_airbridge.fap /Volumes/Flipper/apps/Tools/
```

## Deploy App Files to the SD Card

The Deploy flow reads its files from `/ext/apps_data/pocket_airbridge/` on the SD card:

| SD path | Source | Role |
|---|---|---|
| `/ext/apps_data/pocket_airbridge/bootstrap.js` | `airbridge/web/bootstrap.js` | The typed USB snippet. ASCII-only and build-time SHA-256 pinned. It requires `DecompressionStream("gzip")`, carries a WebHID filter list for every supported profile VID/PID, requests `0x42`, verifies compressed-byte checksum, inflates gzip, and boots the app. |
| `/ext/apps_data/pocket_airbridge/bootstrap-ble.js` | `airbridge/web/bootstrap-ble.js` | The BLE twin of `bootstrap.js`, typed over BLE HIDS during a BLE Deploy run. ASCII-only and build-time SHA-256 pinned. It opens Web Bluetooth, subscribes to the AirBridge serial TX **notify** characteristic, writes `0x42`, verifies and inflates gzip, and boots the streamed app. |
| `/ext/apps_data/pocket_airbridge/app-usb.html.gz` | `airbridge/dist/app-usb.html.gz` | A build-time SHA-256-pinned `ABND` v1 container holding the deterministic gzip WebHID app payload. The FAP validates and strips the container header before streaming. |
| `/ext/apps_data/pocket_airbridge/app-ble.html.gz` | `airbridge/dist/app-ble.html.gz` | A build-time SHA-256-pinned `ABND` v1 container holding the deterministic gzip Web Bluetooth app payload. The FAP validates and strips the container header before streaming. |

Build both bundles, then send all four deploy artifacts. The AirBridge app must
NOT be running while you do this (the serial port only exists when the app is
exited):

```bash
cd /Users/asutov/projects/flipperzero-firmware
python3 airbridge/tools/build_bundle.py

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  airbridge/web/bootstrap.js \
  /ext/apps_data/pocket_airbridge/bootstrap.js

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  airbridge/web/bootstrap-ble.js \
  /ext/apps_data/pocket_airbridge/bootstrap-ble.js

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  airbridge/dist/app-usb.html.gz \
  /ext/apps_data/pocket_airbridge/app-usb.html.gz

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
  /ext/apps_data/pocket_airbridge/app-usb.html.gz

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  airbridge/dist/app-ble.html.gz \
  /ext/apps_data/pocket_airbridge/app-ble.html.gz

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
  /ext/apps_data/pocket_airbridge/app-ble.html.gz

```

The `size` commands confirm that both transport-matched bundles were uploaded.

`build_bundle.py` also regenerates the tracked FAP header
`applications_user/pocket_airbridge/airbridge_assets_digest.h`. Rebuild the FAP
after changing either deploy asset; an older FAP intentionally rejects newer SD
assets whose pinned digest or bundle metadata does not match.

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

The config is capped at 4 KiB and parsed atomically. Any invalid or unknown key,
including removed development-only BLE toggles, rejects the whole temporary parse;
the app restores both the default USB profile and default BLE identity and displays
`WARN: BLE ID DEFAULT`.

**There is no runtime profile switching.** The profile line in the FAP menu is display-only. This was removed by design on 2026-07-20 after hardware testing showed that CDC→composite apply works, but composite→composite reconfiguration (switching from one impersonation profile to another) is fatal on this USB stack: the device dies silently and needs a physical reset. Analysis pinned a definite endpoint-number collision (vendor IN `0x82` / OUT `0x02` shared endpoint index 2) plus risky manual endpoint teardown in deinit. The configured profile is applied automatically a short moment after app start and is held until app exit (always-on); the previous USB mode is restored only on app exit.

When `ble_dis_serial` is omitted, the FAP derives a stable HP-shaped DIS serial from the Flipper hardware UID hash (`HP` plus eight hex digits). Supplying `ble_dis_serial` in the config still overrides that derived default.

### On-Device Controls

The app opens on the **Bridge** screen, the default relay view. **LEFT** and
**RIGHT** rotate a screen carousel: Bridge → USB Deploy prompt → BLE Deploy
prompt → Bridge. **OK** on a deploy prompt starts that deploy; OK on the Bridge
screen does nothing. A short **BACK** press on a prompt returns to Bridge; a
long **BACK** press exits the app. The USB ↔ BLE relay keeps forwarding in the
background on every screen, so cycling the carousel never interrupts an
in-flight transfer.

### The Deploy Flow

1. From the Bridge screen, press **RIGHT** (or **LEFT**) to reach the USB Deploy prompt or the BLE Deploy prompt. Leave the prompt open while preparing the target computer.
2. On the target PC, open a tab at `https://blank.org` (not `about:blank` — some Chrome builds report `window.isSecureContext === false` there; blank.org loads from browser cache offline), open DevTools, and place the cursor in the console. For BLE Deploy, pair the target to the Flipper BLE identity first if this is the first use. HIDS stays enabled in the advertising policy for the complete AirBridge profile lifetime and is disabled only during teardown; the Bridge watchdog reasserts that idempotent HAL policy within its 2.5-second cadence and restarts advertising only when GAP is idle, without disconnecting an active link. Advertising HIDS alone does not emit keyboard reports.
3. On OK, the FAP authenticates and types `bootstrap.js` over the USB keyboard collection or `bootstrap-ble.js` over BLE HIDS with a small bounded per-key timing jitter. The `TYPING via USB` / `TYPING via BLE` screen shows a determinate progress bar (chars typed / total, plus %) for the entire emission; BACK aborts instantly. No keyboard report is emitted in Bridge mode or on a bridge data path; typing is available only after the explicit Deploy menu action and confirmation.
4. The executed bootstrap paints a landing page. While it waits, the FAP shows `Waiting for browser...` with an indeterminate marquee and the hint `Click Connect in the browser`. Clicking **Connect** supplies the browser user gesture, opens the matching WebHID or Web Bluetooth transport, and sends `0x42` (over the USB vendor collection, or as a BLE RX write). During BLE Deploy Waiting, an unsubscribed central is disconnected after 15 seconds and advertising resumes; active numeric-comparison pairing restarts that window. A subscribed central that never requests the bundle has a separate 90-second Waiting limit.
5. The FAP authenticates and streams the length+checksum header and transport-matched `app-usb.html.gz` or `app-ble.html.gz` bundle (see [protocol.md](protocol.md), "Bootstrap Stream Protocol"). The bootstrap inflates it with `DecompressionStream("gzip")`; unsupported browsers show `Transfer unsupported - retry`. The `Serving app via USB` / `Serving app via BLE` screen shows a determinate progress bar (KB sent / total, plus %); BACK aborts.
6. The screen shows `Done` and returns to Bridge.

The FAP source of truth is
`/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c`.

Static BLE radio evidence: firmware configures and supports a local ATT MTU maximum
of 414, enables DLE, prefers 2M PHY, and requests a 7.5 to 45 ms interval. Those
are configured values only; negotiated MTU is peer-driven. Negotiated runtime MTU,
PHY, DLE, interval, and throughput remain unclaimed without a physical run.

USB Deploy requires a kbd+vendor USB profile. BLE Deploy uses the AirBridge HIDS
keyboard report and streams over BLE serial; it does not depend on the USB keyboard collection.

## How It Works

### USB Side

The running app exposes a vendor-defined HID collection inside the selected USB
personality:
- Usage Page: `0xFF00` (Vendor Defined)
- Usage: `0x01`
- IN endpoint `0x81` (Flipper → PC)
- OUT endpoint `0x01` (PC → Flipper)
- Packet size: **64 bytes**
- Default VID/PID: `0x03F0 / 0x5341` from `hp_kbd_vendor`

The sender page connects to the configured impersonation profile via WebHID
using that profile's VID/PID filter.

Historical development builds used a standalone `Pocket AirBridge` identity with
STM32 VID `0x0483` and PID `0x5742`. The shipped FAP impersonates full device
personalities and never exposes VID `0x0483` on the bus while running.

### BLE Side

The app installs its own profile through `bt_profile_start` and receives serial
events directly, without firmware RPC interception. Direct GATT operations borrow
the current profile through `bt_current_profile_acquire/release`, preventing its
destruction during a send. Firmware never retains an external profile for retries.

The chat page connects to the AirBridge serial service using the on-air UUIDs
generated in `airbridge/web/airbridge-identity.js`:

| Role | UUID | Direction |
|------|------|-----------|
| Service | `7b871228-baf0-c5b4-5f46-9c2613d627a3` | — |
| TX (Notify) | `87825ec0-7398-8cb7-3242-b083eaa34f27` | Flipper → Browser |
| RX (Write) | `152f7eeb-e3b7-5898-ba41-7ff66121c98d` | Browser → Flipper |
| Flow control (Notify) | `d2d968bf-cbd8-568f-d24c-5bbddb824f25` | Flipper → Browser |
| Status (Notify/Read/Write) | `bebb7113-63db-bbae-bb45-37dbbf73b6b3` | Both |

The firmware source keeps the controller-order values in
`applications_user/pocket_airbridge/airbridge_serial_uuid.h`; the browser uses the
byte-reversed on-air strings above. The advertising watchdog runs every 2.5
seconds across all screens, waits for queued GAP work to complete, and restarts
advertising only when GAP is idle, without disconnecting an active link. The
profile also includes Battery, DIS, and HIDS
values from the FAP config. Advertising HIDS
does not authorize keyboard emission: Bridge mode and all bridge data paths emit no
keyboard reports; reports are limited to the explicit, confirmed Deploy typing prompts.
Bonding is enabled: the
first pairing uses MITM numeric comparison and stores a bond for silent later
reconnects.

### Bridge Logic (Inside the FAP)

The FAP is a queue-driven, stateless byte relay. All forwarding happens in the
FAP I/O worker; the USB and GAP callbacks never touch the other transport's HAL
directly.

1. **Event queue** — a `FuriMessageQueue` of 8 `BridgeEvent` structs
   (`type`, 64-byte `data`, `len`, `to_ble` direction flag).
2. **USB callback** (`usb_event_callback`) — on a HID OUT report, copies the
   bytes into a `BridgeEvent` and enqueues it with timeout `0` (callback
   context must not block). Connect/disconnect events are posted the same way.
3. **BLE callback** (`airbridge_relay_ble_event`) — runs on the BLE event worker,
   copies inbound bytes into a `BridgeEvent` and enqueues with timeout `0`.
   Returns the remaining accept capacity (`64` on enqueue, `0` when the queue
   was full and the frame was dropped).
4. **I/O worker forwarding** — the only place transport HAL calls happen:
   - USB→BLE: `airbridge_ble_send`. On failure the `TXERR` counter
     increments; there is no spin-retry (protocol-level ACK backpressure
     handles loss).
   - BLE→USB: the payload is copied into a zero-filled 64-byte buffer and sent
     with `airbridge_usb_vendor_send_response(report, 64)`. Host HID drivers
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
    when a BLE central is connected and is the bare rune when down. A hint
    row (y=47-53) advertises the LEFT/RIGHT carousel to the deploy prompts
    and sits above the exit hint (long BACK exits, y=63). The green LED
    blinks every 500 ms as a
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
python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
```

Then open:
- `http://127.0.0.1:8081/chat-usb.html` on PC-A (WebHID chat page)
- `http://127.0.0.1:8081/chat-ble.html` on PC-B (Web Bluetooth chat page)

The superseded `sender.html` and `receiver.html` file-transfer pages were removed;
use the chat pages above for all transfers.

### Bonding and macOS

Bonding is on and persists across app sessions. The first pairing on each host
shows a numeric-comparison code; later bonded reconnects are silent. macOS may
auto-reconnect the bonded keyboard through its HID daemon. The BLE Deploy
Waiting screen disconnects an unsubscribed central after 15 seconds, then resumes
advertising, so a browser picker gets another chance. Active pairing pauses that
eviction by restarting the window.

For first-time BLE Deploy use on macOS, pair through **System Settings → Bluetooth**
before Web Bluetooth so the OS binds the keyboard service. The effective GAP
appearance stays `GAP_APPEARANCE_UNKNOWN` (`0x0000`), even when the config contains
keyboard appearance `0x03C1`; that override avoids keyboard-style PIN entry.
HIDS remains advertised. A four-second eviction window was rejected on hardware
because it interrupted discovery before serial subscription.

For chat, Chrome tries `navigator.bluetooth.getDevices()` before opening a
picker. Enable `chrome://flags/#enable-web-bluetooth-new-permissions-backend`
to retain granted devices across Chrome restarts. A bonded macOS HID connection
can still grip a Bridge-mode link. The same 15-second unsubscribed-link watchdog
applies there; a subscribed link with no deploy `0x42` request is a 90-second
zombie only while the app is in Deploy Waiting.

For passive advertising QA, `python3 airbridge/scripts/ble_qa_scan.py scan` asserts
the AirBridge serial UUID plus HIDS after active-state watchdog recovery in Bridge
and Deploy states. This advertisement contract is distinct from keyboard emission:
only the confirmed Deploy typing prompts send keyboard reports.

## Step-by-Step Chat Demo Script

1. **Build and deploy the FAP** (see Deploy section above).
2. **Launch the app** on Flipper Zero. The screen should show USB and BLE status.
3. **Connect USB** from Flipper Zero to PC-A.
4. **Open `chat-usb.html`** on PC-A in Chrome/Edge. Run `:c` and select the device running Pocket AirBridge.
5. **Pair Bluetooth** on PC-B with Flipper Zero.
6. **Open `chat-ble.html`** on PC-B in Chrome/Edge. Run `:c` and select the active Bluetooth impersonation profile.
7. **Compare SAS**. Both browsers show a six-digit code. Run `:a` on both sides only if the codes match. Send controls stay locked before this step.
8. **Send a text message** from PC-A. Press `i`, type "Hello from USB", and press Enter. PC-B should display the message in the chat transcript.
9. **Reply from PC-B**. Press `i`, type "Hello from BLE", and press Enter. PC-A should display the reply.
10. **Send an attachment** from PC-A. Press Esc, run `:f` to select a small text file or image, then run `:s` to send it. PC-B should show a progress bar, verify SHA-256, and then offer the file for download.
11. **Cancel a transfer**. Start sending a large attachment from either side, then press Esc and run `:x`. The other side should show "Transfer cancelled" and return to idle.

## Troubleshooting

See [troubleshooting](troubleshooting.md) for the 2026-09-15 root cause, diagnostic
logs, watchdog timing, GATT return conventions, and regression checklist.

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
| Flipper unresponsive after app launch or a USB mode switch | Probe with `python3 airbridge/tools/flipper_alive.py --wait 30`. A healthy CLI answers `\r` with a `>:` prompt within 5 s. Port present but silent means the firmware is hung (USB CDC still enumerated, firmware dead). Recovery is a physical reset; do not attempt a DTR-toggle reset from software |
| `storage.py` can't find `/dev/cu.usbmodemflip_*` | The AirBridge app is still running — exit it (long BACK) or restart the Flipper so the serial port reappears |
| `TXERR` counter is non-zero | One `TXERR` can occur during a visible mid-flight cancel race. Persistent errors can mean a disconnected link or an unsubscribed serial service. If discovery succeeds but both forwarding counters stay zero, check HID event ownership; the fix requires a full firmware flash. |
| Pairing code dialog vanishes during deploy Connect before it can be confirmed | The 2.5 s advertising pump restarts only from GAP idle. The separate 15 s unsubscribed-link watchdog restarts its window during active pairing; a subscribed central with no accepted `0x42` has a 90 s Waiting limit. Use the current pairing-aware FAP; a four-second eviction variant was rejected. |
| App is missing an icon | Add `applications_user/pocket_airbridge/icon.png` and `fap_icon="icon.png"` in `application.fam`, then rebuild/redeploy |

## Important Notes

- **This is a custom firmware/apps repository.** Core firmware changes are in-tree and must be reviewed with the rest of this repository.
- **AirBridge owns its BLE serial service.** The FAP replaces the default Bluetooth profile and routes its custom serial UUIDs directly to the relay. Stock Bluetooth RPC is unavailable while this profile is active.
- **The WebHID data channel** is the vendor-defined collection inside the active impersonation profile. When the app exits, it restores the previous USB mode (usually `usb_cdc_dual`).

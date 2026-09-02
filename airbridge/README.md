# Pocket AirBridge

A browser-only, offline, end-to-end encrypted chat and attachment exchange system using a **Flipper Zero** as a physical bridge between two computers.

Pocket AirBridge lives in one local custom firmware and apps repository at
`/Users/asutov/projects/flipperzero-firmware`. Its product assets are under
`airbridge/`, and its canonical FAP source is
`applications_user/pocket_airbridge/`. Edit and build those in-tree sources.
There is no separate product repository, patch bundle, or patch-application step.

**Hackathon goal**: Exchange text messages and send small files or images between PC-A and PC-B with no network, no cloud, and nothing to install on either PC.

## Quickstart

1. **Flash the Pocket AirBridge firmware** (see [docs/firmware-guide.md](docs/firmware-guide.md)).
2. **Serve the web pages locally** (WebHID/Web Bluetooth need a secure origin):
   ```bash
   python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
   ```
3. **Connect Flipper Zero to PC-A** via USB.
4. **Pair Flipper Zero to PC-B** via Bluetooth (confirm the PIN shown on the Flipper screen).
5. **Open `http://127.0.0.1:8081/chat-usb.html`** on PC-A (Chrome/Edge), click **Connect USB**, and pick the Pocket AirBridge device in the browser prompt.
6. **Open `http://127.0.0.1:8081/chat-ble.html`** on PC-B (Chrome/Edge), click **Connect BLE**, and pick the Flipper Zero in the browser prompt.
7. Compare the six-digit SAS on both browsers, click **Accept SAS** on both sides, then type a message or select a file and hit **Send**.

For a browser-only protocol check, open the canonical harness URL:
`http://127.0.0.1:8081/protocol-harness.html`.

## Architecture

| Component | Technology | Role |
|-----------|-----------|------|
| PC-A Chat | WebHID (Chromium) | Sends and receives messages and attachments over USB HID |
| Flipper Zero | Custom USB HID + BLE Serial | Blind relay: forwards encrypted frames verbatim between USB and BLE |
| PC-B Chat | Web Bluetooth (Chromium) | Sends and receives messages and attachments over BLE, decrypts, reassembles, verifies hash |

Read the full architecture in [`docs/architecture.md`](docs/architecture.md).

## Protocol

Binary wire protocol over 64-byte frames. Message types include `HELLO`, `ITEM_META`, `ITEM_DATA`, `ACK`, `NACK`, `ITEM_DONE`, `ERROR`, `BUSY`, `CANCEL`, and crypto handshake frames. ACKs carry `[acked_type, acked_seq]`. NACKs carry `[type, seq_hi, seq_lo, reason]` and request only an exact current-item outer-frame retry. There is no byte-range or cross-session resume. The receiver verifies SHA-256 before ACKing `ITEM_DONE`.

The chat protocol is always encrypted end to end in the browsers. USB and BLE exchange a P-256 ECDH handshake, derive AES-GCM keys with HKDF, and unlock only after both users compare and accept the same six-digit SAS. The Flipper never sees plaintext, session keys, decrypted metadata, or decrypted files.

Read the full specification in [`docs/protocol.md`](docs/protocol.md).

## Locked-down PC bootstrap (Deploy app)

Some corporate PCs are locked down by device-control policy: no mass storage, no network transfer, no local files. HID is the only USB class that survives. The Deploy flow turns that policy to our advantage. The Flipper impersonates an HP "Wireless Keyboard and Mouse" dongle (VID `0x03F0`, PID `0x5341`), a composite device with a real keyboard collection and a vendor-defined collection on usage page `0xFF00`. It types a small bootstrap into the browser as if it were a keyboard, the bootstrap opens WebHID on the vendor collection, and the Flipper streams the complete chat app to the PC over the existing vendor HID channel. PC-B (the BLE side) is unchanged.

### Honest framing

This is BadUSB-shaped by design. Keyboard emulation is the whole point: it is the only delivery channel a HID-only policy cannot block. The guardrails are deliberate:

- Keystrokes are emitted only from an explicit deploy prompt on the Flipper (reached with LEFT/RIGHT from the Bridge screen), and only after you place the cursor and press OK to confirm.
- The Flipper screen shows `TYPING…` for the entire emission; pressing BACK aborts instantly.
- No keyboard report is ever sent in Bridge mode or on any data path. Typing exists only inside the Deploy flow.
- The typed payload is a fixed, reviewable, ASCII-only artifact: [`bootstrap.js`](web/bootstrap.js). It fetches a gzip-compressed app bundle and requires browser support for `DecompressionStream("gzip")`; unsupported browsers show `Transfer unsupported - retry` before WebHID selection.
- The whole thing requires physical possession of the Flipper plus explicit on-device actions. Task 8 adds small bounded typing jitter, but typing still exists only inside the explicit Deploy flow.

### Steps

Run these commands from the monorepo root,
`/Users/asutov/projects/flipperzero-firmware`.

1. **Build the app bundle:**
   ```bash
   python3 airbridge/tools/build_bundle.py
   ```
   This inlines the shared JS modules into self-contained app pages and writes deterministic gzip companions: `airbridge/dist/app-usb.html.gz` and `airbridge/dist/app-ble.html.gz`.
2. **Deploy the bootstrap and the bundle to the Flipper SD card** (exact commands in [docs/firmware-guide.md](docs/firmware-guide.md)).
3. **Launch Pocket AirBridge** on the Flipper. The app opens on the Bridge relay screen; press **RIGHT** to reach the USB Deploy prompt. (LEFT/RIGHT cycle Bridge → USB Deploy → BLE Deploy → Bridge; a short BACK returns to Bridge, a long BACK exits the app. The relay keeps running in the background on every screen.) The prompt asks you to place the cursor, then press OK.
4. **On the target PC**, open a browser tab at `https://blank.org`, open DevTools (F12), and click into the console. Any `https://` page works; `about:blank` is possible but verify first — on some Chrome builds `window.isSecureContext === false` there, which blocks WebHID. Run `console.log(window.isSecureContext)` to confirm before proceeding.
5. **Press OK on the Flipper.** The bootstrap types itself into the console while the screen shows `TYPING…` (BACK aborts). Once executed, it paints a minimal landing page with a Connect button.
6. **Click Connect.** Your real click supplies the user activation WebHID needs; pick the device in the browser prompt. The Flipper streams the compressed app from its SD card, the bootstrap inflates it with `DecompressionStream("gzip")`, and the page replaces itself with the full app. Unsupported browsers show `Transfer unsupported - retry` before a picker opens.

`data:` URLs are dead for this purpose (`window.isSecureContext === false`, so WebHID is unavailable there). `about:blank` is NOT reliably a secure context — on some Chrome builds `window.isSecureContext === false` and `navigator.hid` is undefined, while the same Chrome build passes on `https://blank.org`. The robust validated channel is ANY `https://` page plus the DevTools console. Both facts were confirmed on real Chrome on 2026-07-20 (about:blank insecure on the user's build; https://blank.org worked end-to-end on hardware).

The typed snippet carries a WebHID filter list that enumerates every profile VID/PID (Logitech, Dell, MSFT, HP). On the target machine only the Flipper should match; other devices with those IDs should be absent.

### Keyboard layout requirement

The typed payload is ASCII-only but includes symbols like `{}[]();:=>"'`. The Flipper types it using US keyboard scancodes, so **the target PC must use a US keyboard layout**. On any other layout the symbols mistype.

### Verified on hardware

- HP composite enumeration verified on macOS, 2026-07-20: VID `0x03F0` PID `0x5341`, exact HP product/manufacturer strings, keyboard collection claimed by the OS while WebHID opens the vendor `0xFF00` collection, bidirectional chat through the HP composite.
- The Windows `usbccgp` tree-compare against the real dongle capture is **deferred**. Run the same `Get-PnpDevice` capture on the target machine when one is available.
- One USB profile is active at a time, selected by `/ext/apps_data/pocket_airbridge/config` (default `hp_kbd_vendor`). The configured profile is applied automatically a short moment after app start and is held until the app exits (always-on). There is no runtime switching: composite-to-composite reconfiguration is fatal on this USB stack (the device dies silently and needs a physical reset).
- Task 8 added a per-device default BLE DIS serial derived from a hash of the Flipper UID and formatted as `HP` plus eight hex digits. Raw UID bytes are not exposed. BLE service UUID hiding and Windows `usbccgp` tree comparison remain deferred until physical Chrome and Windows evidence exists.

## Verified on hardware (2026-07-20)

Tested on a real Flipper Zero between `chat-usb.html` and `chat-ble.html`:

- Bidirectional text messages.
- Attachments in both directions with SHA-256 verified: 38 B, plus 5 KB and 20 KB transfers.
- Sender-side cancel and receiver-side cancel, both mid-transfer.
- Flipper debug aids working: green LED heartbeat every 500 ms; on-screen counters `U->B` / `B->U` / `DROP` / `TXERR`, with `DROP 0` and `TXERR 0` during healthy transfers.
- Rough throughput: ~0.5–2 KB/s over BLE (demo-acceptable, not fast).

## Current verification scope

Tasks 1 through 8 added browser-only E2E crypto, SAS unlock, deterministic gzip Deploy bundles, NACK exact-frame retry, BLE static tuning evidence, deploy typing jitter, and per-device default DIS serials. These are browser-harness and firmware-build verified in this roadmap, but physical USB/BLE proof after those changes is blocked unless a Flipper is connected and a gated run records hardware evidence. BLE service UUID hiding, Windows USB tree comparison, negotiated BLE MTU/PHY/DLE/runtime interval proof, and transfer resume remain deferred.

## Project Structure

```
flipperzero-firmware/
├── airbridge/
│   ├── docs/               # Architecture, protocol, and operations docs
│   ├── web/
│   │   ├── chat-usb.html       # WebHID chat page (send/receive over USB)
│   │   ├── chat-ble.html       # Web Bluetooth chat page (send/receive over BLE)
│   │   ├── airbridge-protocol.js
│   │   ├── airbridge-transports.js
│   │   └── protocol-harness.html
│   └── README.md               # This file
└── applications_user/pocket_airbridge/ # Canonical FAP source
```

The superseded `sender.html` and `receiver.html` file-transfer pages were removed;
use `chat-usb.html` and `chat-ble.html` instead.

## Task Breakdown for Implementation

### Phase 1 — Validate the Web Pages (No Flipper)
- [x] Host `chat-usb.html` and `chat-ble.html` on `localhost`.
- [x] Verify WebHID permission flow and device selection.
- [x] Verify Web Bluetooth pairing flow and GATT characteristic access.
- [x] Use two browser tabs + mocked Flipper (`?mock=1` pages and `protocol-harness.html`) to test message and attachment exchange.

### Phase 2 — Flipper Zero USB HID Vendor Interface
- [x] Define a custom HID report descriptor with Usage Page `0xFF00`.
- [x] Implement USB IN/OUT report handlers in the Flipper app.
- [x] Test that the sender page can open the device and exchange 64-byte reports.

### Phase 3 — Flipper Zero BLE GATT Service
- [x] Build custom AirBridge BLE serial service with its own UUID family (service `7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify `87825ec0-7398-8cb7-3242-b083eaa34f27`, RX write `152f7eeb-e3b7-5898-ba41-7ff66121c98d`).
- [x] TX characteristic (notify) and RX characteristic (write) working via a raw-serial hook in `bt_service`.
- [x] Test that the receiver page can pair, connect, and subscribe to notifications.

### Phase 4 — Bridge Logic
- [x] Implement the forwarding loop (stateless byte relay driven by an event queue; no single-item buffer on the Flipper).
- [x] Forward frames and ACKs transparently in both directions.
- [x] Timeout, retry, and cancel logic (lives in the browser endpoints, not the bridge).
- [x] Add status screen rendering (USB/BLE state, `U->B` / `B->U` / `DROP` / `TXERR` counters, LED heartbeat).

### Phase 5 — End-to-End Demo
- [x] Exchange text messages between PC-A and PC-B (both directions, hardware-verified 2026-07-20).
- [x] Send a small text file as an attachment (38 B, 5 KB, and 20 KB, both directions).
- [x] Send a small PNG/JPG image as an attachment (binary attachments up to 20 KB verified on hardware 2026-07-20).
- [x] Verify SHA-256 hash matches on the receiver (both directions).
- [x] Verify sender-side and receiver-side cancel mid-transfer.
- [x] Measure rough throughput and chunk latency (~0.5–2 KB/s over BLE).

### Known Gaps / Hackathon Hacks

| Gap | Mitigation for Demo |
|-----|---------------------|
| Custom BLE profile built | AirBridge composite profile with Battery, DIS, HIDS, and custom serial UUID family (`7b871228-baf0-c5b4-5f46-9c2613d627a3` / TX `87825ec0-7398-8cb7-3242-b083eaa34f27` / RX `152f7eeb-e3b7-5898-ba41-7ff66121c98d`); TX uses NOTIFY (not INDICATE). Static firmware config supports a local ATT MTU maximum of 414, enables DLE, prefers 2M PHY, and requests a 7.5 to 45 ms interval; negotiated runtime values remain peer-driven and need hardware evidence. |
| Custom HID descriptor registration | May need to patch `furi_hal_usb_hid` or use a community plugin template |
| BLE service UUID hiding | Deferred. The serial UUID is still advertised and the browser still uses the service-filtered picker until `acceptAllDevices:true` with `optionalServices` is proven on hardware. |
| 59-byte chunks | Slow but simple; works for files < 50 KB in reasonable time |
| Single-item half-duplex | Only one message or attachment in flight at a time; lower itemId wins collision. NACK retries exact current-item frames only; no resume. |
| Transfer resume | Deferred. NACK retries exact current-item outer frames only. |

## Browser Compatibility

- **Chrome / Edge / Brave** (Chromium 89+) on Windows, macOS, or Linux.
- WebHID and Web Bluetooth require a **secure origin** (`https://` or `localhost`).
- For persisted `getDevices()` grants across Chrome restarts, enable
  `chrome://flags/#enable-web-bluetooth-new-permissions-backend`; without it,
  Bluetooth reconnects fall back to the browser picker.
- On Linux, Web Bluetooth may need `chrome://flags/#enable-web-bluetooth` or kernel BLE permissions.

## Flipper Zero Firmware Notes

This custom firmware and apps repository contains two custom profiles:

- **USB HID**: `usb_airbridge` — a vendor-defined HID profile using usage page `0xFF00` for bidirectional 64-byte reports. USB identity is selected at app start from `/ext/apps_data/pocket_airbridge/config` (default `hp_kbd_vendor`, HP VID `0x03F0` PID `0x5341`) and held for the session lifetime. Composite-to-composite reconfiguration is not attempted.
- **BLE GATT**: `airbridge_profile` — a custom composite GATT profile (`lib/ble_profile/extra_profiles/airbridge_profile.c`) advertising the AirBridge serial UUID family: service `7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify `87825ec0-7398-8cb7-3242-b083eaa34f27` (NOTIFY, not INDICATE), and RX write `152f7eeb-e3b7-5898-ba41-7ff66121c98d`. Battery, DIS, and HIDS are also included in the composite.

Static BLE tuning evidence records configured ATT MTU 414, DLE enabled, 2M PHY preference, requested 7.5 to 45 ms connection interval, and 244-byte serial value capacity. Runtime negotiated MTU/PHY/DLE/interval and throughput remain unclaimed unless a hardware evidence run records them.

The custom FAP lives in `applications_user/pocket_airbridge/` and is built with this repository's full firmware build system.

The original `flipper/bridge_app.c` pseudocode skeleton now lives at [`docs/attic/bridge_app.c`](docs/attic/bridge_app.c) and is superseded. The live FAP is `applications_user/pocket_airbridge/pocket_airbridge.c`.

## License

MIT — Hackathon prototype. Use at your own risk.

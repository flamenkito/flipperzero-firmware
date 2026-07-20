# Pocket AirBridge

A browser-only, offline chat and attachment exchange system using a **Flipper Zero** as a physical bridge between two computers.

**Hackathon goal**: Exchange text messages and send small files or images between PC-A and PC-B with no network, no cloud, and no keyboard emulation.

## Quickstart

1. **Flash the Pocket AirBridge firmware** (see [docs/firmware-guide.md](docs/firmware-guide.md)).
2. **Serve the web pages locally** (WebHID/Web Bluetooth need a secure origin):
   ```bash
   cd web && python3 -m http.server 8080
   ```
3. **Connect Flipper Zero to PC-A** via USB.
4. **Pair Flipper Zero to PC-B** via Bluetooth (confirm the PIN shown on the Flipper screen).
5. **Open `http://localhost:8080/chat-usb.html`** on PC-A (Chrome/Edge), click **Connect USB**, and pick the Pocket AirBridge device in the browser prompt.
6. **Open `http://localhost:8080/chat-ble.html`** on PC-B (Chrome/Edge), click **Connect BLE**, and pick the Flipper Zero in the browser prompt.
7. Type a message or select a file and hit **Send**.

## Architecture

| Component | Technology | Role |
|-----------|-----------|------|
| PC-A Chat | WebHID (Chromium) | Sends and receives messages and attachments over USB HID |
| Flipper Zero | Custom USB HID + BLE Serial | Stateless byte pipe: forwards frames verbatim between USB and BLE |
| PC-B Chat | Web Bluetooth (Chromium) | Sends and receives messages and attachments over BLE, reassembles, verifies hash |

Read the full architecture in [`docs/architecture.md`](docs/architecture.md).

## Protocol

Binary wire protocol over 64-byte frames. Message types: `HELLO`, `ITEM_META`, `ITEM_DATA`, `ACK`, `NACK` (reserved, unused), `ITEM_DONE`, `ERROR`, `BUSY`, `CANCEL`. ACKs carry `[acked_type, acked_seq]`; the receiver verifies SHA-256 before ACKing `ITEM_DONE`.

Read the full specification in [`docs/protocol.md`](docs/protocol.md).

## Verified on hardware (2026-07-20)

Tested on a real Flipper Zero between `chat-usb.html` and `chat-ble.html`:

- Bidirectional text messages.
- Attachments in both directions with SHA-256 verified: 38 B, plus 5 KB and 20 KB transfers.
- Sender-side cancel and receiver-side cancel, both mid-transfer.
- Flipper debug aids working: green LED heartbeat every 500 ms; on-screen counters `U->B` / `B->U` / `DROP` / `TXERR`, with `DROP 0` and `TXERR 0` during healthy transfers.
- Rough throughput: ~0.5–2 KB/s over BLE (demo-acceptable, not fast).

## Project Structure

```
flipper-hid/
├── docs/
│   ├── architecture.md     # System design and data flow
│   ├── protocol.md         # Binary message protocol spec
│   ├── firmware-guide.md   # How to build & deploy the Flipper app
│   └── attic/
│       └── bridge_app.c    # Superseded pseudocode skeleton (historical)
├── web/
│   ├── chat-usb.html       # WebHID chat page (send/receive over USB)
│   ├── chat-ble.html       # Web Bluetooth chat page (send/receive over BLE)
│   ├── airbridge-protocol.js   # Shared wire protocol (frames, ACK, ItemSender/ItemReceiver)
│   ├── airbridge-transports.js # Shared WebHID/Web Bluetooth transport helpers
│   ├── protocol-harness.html   # In-browser protocol test harness
│   ├── sender.html         # Legacy WebHID file sender
│   └── receiver.html       # Legacy Web Bluetooth file receiver
└── README.md               # This file
```

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
- [x] Reuse the stock Flipper Serial-over-BLE service (browser-canonical UUIDs) instead of a custom service.
- [x] TX characteristic (indicate) and RX characteristic (write) working via a raw-serial hook in `bt_service`.
- [x] Test that the receiver page can pair, connect, and subscribe to indications.

### Phase 4 — Bridge Logic
- [x] Implement the forwarding loop (stateless byte relay driven by an event queue; no single-item buffer on the Flipper).
- [x] Forward frames and ACKs transparently in both directions.
- [x] Timeout, retry, and cancel logic (lives in the browser endpoints, not the bridge).
- [x] Add status screen rendering (USB/BLE state, `U->B` / `B->U` / `DROP` / `TXERR` counters, LED heartbeat).

### Phase 5 — End-to-End Demo
- [x] Exchange text messages between PC-A and PC-B (both directions, hardware-verified 2026-07-20).
- [x] Send a small text file as an attachment (38 B, 5 KB, and 20 KB, both directions).
- [ ] Send a small PNG/JPG image as an attachment (not yet tried on hardware; any binary file should work the same).
- [x] Verify SHA-256 hash matches on the receiver (both directions).
- [x] Verify sender-side and receiver-side cancel mid-transfer.
- [x] Measure rough throughput and chunk latency (~0.5–2 KB/s over BLE).

### Known Gaps / Hackathon Hacks

| Gap | Mitigation for Demo |
|-----|---------------------|
| Flipper firmware GATT flexibility | May need to fork `furi` or reuse existing UART-over-BLE profile |
| Custom HID descriptor registration | May need to patch `furi_hal_usb_hid` or use a community plugin template |
| No encryption | Acceptable for hackathon demo (local physical proximity) |
| 59-byte chunks | Slow but simple; works for files < 50 KB in reasonable time |
| Single-item half-duplex | Only one message or attachment in flight at a time; lower itemId wins collision |

## Browser Compatibility

- **Chrome / Edge / Brave** (Chromium 89+) on Windows, macOS, or Linux.
- WebHID and Web Bluetooth require a **secure origin** (`https://` or `localhost`).
- On Linux, Web Bluetooth may need `chrome://flags/#enable-web-bluetooth` or kernel BLE permissions.

## Flipper Zero Firmware Notes

This prototype targets the **official Flipper Zero firmware** (`flipperzero-firmware`).

The custom app lives in `applications_user/pocket_airbridge/` and is built with `ufbt` or the full firmware build system.

Because the stock firmware has limited support for custom USB HID descriptors and custom BLE GATT services, you may need to:

1. Use a **community firmware** (e.g., Momentum, Xtreme) with more flexible HID/BLE hooks.
2. Or base the HID side on the existing **BadUSB app infrastructure** but with a vendor usage page (not keyboard).
3. Or base the BLE side on the existing **Serial-over-BLE** profile and reinterpret the byte stream as our protocol.

The original `flipper/bridge_app.c` pseudocode skeleton now lives at [`docs/attic/bridge_app.c`](docs/attic/bridge_app.c) and is superseded — the live FAP is `applications_user/pocket_airbridge/pocket_airbridge.c` in the firmware repo.

## License

MIT — Hackathon prototype. Use at your own risk.

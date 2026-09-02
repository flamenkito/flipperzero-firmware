# Sisyphus Plan — Pocket AirBridge Chat with Attachments

## Objective

Pivot Pocket AirBridge from a one-way file-transfer demo into an offline two-party chat demo with optional attachments.

The new product story is: **two browser pages exchange text messages and small files through a Flipper Zero bridge, with no network, cloud, native app, or keyboard emulation.**

## Non-Negotiable Constraints

- PC-A uses WebHID over the existing vendor HID USB profile.
- PC-B uses Web Bluetooth over Flipper Serial-over-BLE.
- Flipper remains a dumb byte relay: USB HID ↔ BLE Serial.
- Flipper never stores a full attachment; at most one frame is in flight.
- 64-byte transport frames, 59-byte maximum protocol payload.
- No BadUSB, no keyboard emulation, no network/cloud dependency.
- Browser permissions remain explicit and user-driven.
- Hackathon reliability beats full-duplex sophistication.

## Recommended MVP Shape

Keep two role-specific pages instead of immediately merging to one universal page:

- `web/chat-usb.html`: PC-A, WebHID side.
- `web/chat-ble.html`: PC-B, Web Bluetooth side.

Both pages share the same chat protocol and UI concepts, but each owns only one browser transport. This avoids fighting WebHID/Web Bluetooth permission asymmetry while still proving bidirectional messaging.

## Protocol Model

Replace the file-specific session with a generic **chat item** transfer model.

### Frame Header

Keep the current 5-byte header:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `type` | Message type |
| 1 | 2 | `seq` | Fragment/chunk sequence |
| 3 | 2 | `len` | Payload length |
| 5 | N | `payload` | Max 59 bytes |

### Message Types

| Code | Name | Purpose |
|------|------|---------|
| `0x01` | `HELLO` | Peer/session handshake |
| `0x02` | `ITEM_META` | Fragmented JSON metadata for a chat item |
| `0x03` | `ITEM_DATA` | Text bytes or attachment bytes |
| `0x04` | `ACK` | ACK one fragment/chunk |
| `0x05` | `NACK` | Request retry |
| `0x06` | `ITEM_DONE` | End of one chat item |
| `0x07` | `ERROR` | Abort current item/session |
| `0x08` | `BUSY` | Peer is already sending/receiving an item |

### Item Metadata JSON

`ITEM_META` remains fragmented exactly like the current fixed `META` implementation: payload starts with 2-byte total-fragment count, followed by a JSON slice.

```json
{
  "itemId": "uuid-or-random-hex",
  "kind": "text" | "attachment",
  "name": "photo.jpg",
  "mimeType": "image/jpeg",
  "size": 15420,
  "chunks": 262,
  "hash": "sha256:a1b2c3...",
  "createdAt": 1783210000000
}
```

For text messages, `name` can be omitted and `mimeType` should be `text/plain;charset=utf-8`.

### Duplex Discipline

Use **application-level half-duplex** for the MVP:

- Either side may initiate a chat item when idle.
- While one item is in progress, the other side only ACKs/NACKs/BUSYs.
- If both sides send at once, deterministic tie-breaker: lower `itemId` wins; loser sends `BUSY` and retries after backoff.

This preserves the existing one-frame-at-a-time relay behavior and avoids Flipper-side queueing.

## Implementation Phases

### Phase 0 — Lock Existing Behavior with a Browser Protocol Harness

**Goal:** Create repeatable validation before changing UI shape.

**Work:**
- Add a small JS protocol module or test harness that can run in-browser without hardware.
- Cover frame build/parse, fragmented meta, ACK wait, text item transfer, attachment transfer, and BUSY collision handling.

**Expected result:** Mocked USB/BLE transports can complete a text item and a small attachment item with no hardware.

**Verification scenario:**

1. Serve the web directory:
   ```bash
   cd /Users/asutov/projects/flipper-hid/web
   python3 -m http.server 8080 --bind 127.0.0.1
   ```
2. Open the harness URL in Playwright:
   ```text
   http://127.0.0.1:8080/protocol-harness.html
   ```
3. Click or evaluate `runAllProtocolTests()`.
4. Expected page result:
   ```text
   PASS frame roundtrip
   PASS fragmented meta
   PASS text item usb-to-ble
   PASS text item ble-to-usb
   PASS attachment sha256
   PASS busy collision
   ```
5. Expected browser console: no `error` entries except optional missing `favicon.ico`.

### Phase 1 — Extract Shared Protocol Logic

**Goal:** Avoid duplicating sender/receiver logic across chat pages.

**Affected files:**
- New `web/airbridge-protocol.js`
- Possibly new `web/airbridge-transports.js`
- Existing `web/sender.html`, `web/receiver.html` only as reference or legacy pages.

**Work:**
- Move constants, frame build/parse, SHA-256, fragmented meta, ACK wait, retry, and item reassembly into shared code.
- Keep transport adapters thin:
  - WebHID adapter: send HID report, listen for input reports.
  - Web Bluetooth adapter: write RX char, listen to TX indications.

**Expected result:** Existing one-way flow can still be represented as an item transfer using shared protocol code.

**Verification scenario:**

1. Run Phase 0 harness again at:
   ```text
   http://127.0.0.1:8080/protocol-harness.html
   ```
2. Expected result: all six harness checks still show `PASS`.
3. Load any retained legacy pages:
   ```text
   http://127.0.0.1:8080/sender.html?v=chat-refactor
   http://127.0.0.1:8080/receiver.html?v=chat-refactor
   ```
4. Expected result: pages either load without script errors or visibly link users to the new chat pages. No silent broken legacy UI.

### Phase 2 — Build Chat UI Pages

**Goal:** Replace the one-way file transfer UX with a chat UX.

**Affected files:**
- New `web/chat-usb.html`
- New `web/chat-ble.html`
- Shared CSS inline or new `web/airbridge.css`

**UI:**
- Connection card with transport status.
- Chat transcript.
- Text input and Send button.
- Attachment picker.
- Per-item progress row.
- Error/status log collapsed below the chat.

**Expected result:** A user can type a message on either side and see it appear on the other side in the chat transcript.

**Verification scenario:**

1. Open both chat pages:
   ```text
   http://127.0.0.1:8080/chat-usb.html?mock=1
   http://127.0.0.1:8080/chat-ble.html?mock=1
   ```
2. In Playwright, send `hello from usb` from `chat-usb.html`.
3. Expected `chat-ble.html` transcript contains:
   ```text
   hello from usb
   ```
4. Send `hello from ble` from `chat-ble.html`.
5. Expected `chat-usb.html` transcript contains:
   ```text
   hello from ble
   ```
6. Expected status on both pages:
   ```text
   Idle
   ```
   after each message completes.

### Phase 3 — Attachment Support

**Goal:** Send small files as chat items.

**Work:**
- Attachments use the same `ITEM_META` → `ITEM_DATA[n]` → `ITEM_DONE` flow.
- Compute SHA-256 before sending.
- Verify SHA-256 after receiving.
- Render attachment cards:
  - text files: preview first N bytes,
  - images: inline thumbnail via Blob URL,
  - other files: download link.

**Limits:**
- No hard file-size cap. Transfers can be cancelled mid-flight via `MSG.CANCEL` (0x09) on either side.
- Cancel is surfaced as a Cancel button during active transfers and as a "Transfer cancelled" transcript message on the peer side.

**Expected result:** Send a `.txt` file and a `.png/.jpg`; receiver verifies hash, offers preview/download, and supports cancellation during transfer.

**Verification scenario:**

1. Open mock pages:
   ```text
   http://127.0.0.1:8080/chat-usb.html?mock=1
   http://127.0.0.1:8080/chat-ble.html?mock=1
   ```
2. Upload a generated text file from the USB page:
   ```bash
   printf 'Pocket AirBridge attachment test\n' > /Users/asutov/projects/flipper-hid/web/fixture-chat.txt
   ```
3. Expected BLE transcript shows an attachment card named `fixture-chat.txt` with:
   ```text
   Hash verified
   Download fixture-chat.txt
   ```
4. Repeat from BLE page to USB page.
5. Expected result: attachment card appears on USB page with `Hash verified`.
6. Cleanup required before completion:
   ```bash
   rm /Users/asutov/projects/flipper-hid/web/fixture-chat.txt
   ```

### Phase 4 — Firmware/UI Truthfulness Pass

**Goal:** Remove misleading “BLE connected” claims.

**Affected file:**
- `applications_user/pocket_airbridge/pocket_airbridge.c` in `~/projects/flipperzero-firmware`

**Work:**
- Change label from `BLE: CONNECTED` to `BLE: ACTIVE` unless a true connection status is available.
- Optionally show `USB: CONNECTED`, `BLE: ACTIVE`, `U->B`, `B->U` counters.
- Do not add Flipper-side protocol interpretation.

**Expected result:** Device UI no longer contradicts browser GATT errors.

**Verification scenario:**

1. Rebuild the FAP:
   ```bash
   cd /Users/asutov/projects/flipperzero-firmware
   ./fbt build APPSRC=applications_user/pocket_airbridge
   ```
2. Deploy and verify size:
   ```bash
   python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
     build/f7-firmware-D/.extapps/pocket_airbridge.fap \
     /ext/apps/USB/pocket_airbridge.fap

   python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
     /ext/apps/USB/pocket_airbridge.fap
   ```
3. Launch:
   ```bash
   python3 scripts/runfap.py -p /dev/cu.usbmodemflip_Luwot1 \
     -s build/f7-firmware-D/.extapps/pocket_airbridge.fap \
     -t /ext/apps/USB/pocket_airbridge.fap
   ```
4. Expected launch behavior: `Device not configured` may appear after launch because USB switches to HID.
5. Expected Flipper screen text:
   ```text
   Pocket AirBridge
   USB: CONNECTED
   BLE: ACTIVE
   U->B: <number>
   B->U: <number>
   ```

### Phase 5 — Documentation and Demo Script

**Goal:** Make the demo reproducible by a human.

**Affected files:**
- `README.md`
- `docs/architecture.md`
- `docs/protocol.md`
- `docs/firmware-guide.md`

**Work:**
- Update project story to “offline chat and attachment exchange.”
- Document browser-canonical BLE UUIDs.
- Document half-duplex rule and file size limits.
- Add a step-by-step demo script.

**Expected result:** A fresh operator can run the demo without knowing the debugging history.

**Verification scenario:**

1. Open `README.md`, `docs/architecture.md`, `docs/protocol.md`, and `docs/firmware-guide.md`.
2. Confirm each document uses the chat/attachment product story rather than one-way file transfer as the primary story.
3. Confirm the docs include these exact demo URLs:
   ```text
   http://127.0.0.1:8080/chat-usb.html
   http://127.0.0.1:8080/chat-ble.html
   ```
4. Confirm BLE docs include browser-canonical UUIDs:
   ```text
   8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000
   19ed82ae-ed21-4c9d-4145-228e61fe0000
   19ed82ae-ed21-4c9d-4145-228e62fe0000
   ```
5. Run the documented demo steps on localhost. Expected result: a new operator can reach the browser permission prompts without reading prior session notes.

## Verification Gates

1. - [x] **Static page load:** `chat-usb.html` and `chat-ble.html` load on `localhost` with zero script errors.
2. - [x] **Mock text:** USB mock sends text to BLE mock and back.
3. - [x] **Mock attachment:** Both directions transfer a small file; SHA-256 matches.
4. - [x] **Permission flow:** Chrome opens WebHID and Web Bluetooth pickers from user gestures.
5. - [x] **Hardware USB:** PC-A connects to `Pocket AirBridge` HID. Frames transmitted with fixed report ID 0x00.
6. - [x] **Hardware BLE:** PC-B connects to Flipper BLE. Fix: `namePrefix: 'Flipper'` filter + `optionalServices` with 128-bit UUIDs. BLE advertising persistent via GAP fix (gap.c timer → GapCommandAdvFast).
7. - [~] **Hardware chat:** BLE→USB partially works (HELLO received on BLE from USB). USB→BLE: ACK timeout. BLE→USB relay (`furi_hal_hid_vendor_send_response` → `usbd_ep_write(HID_EP_IN)`) may have endpoint issue — data not reaching browser.
8. - [~] **Hardware attachment:** One small `.txt` attachment. Blocked on gate 7.

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Web Bluetooth cannot see Serial service | Keep UUID fallback, document browser-canonical UUIDs, verify with Chrome after pairing. |
| Simultaneous sends corrupt flow | Half-duplex app state with `BUSY` and backoff. |
| BLE throughput too slow | Keep demo files tiny and show progress. |
| Flipper UI overclaims BLE state | Rename label to `BLE: ACTIVE`; use browser as source of GATT truth. |
| Shared JS modules blocked by local file serving | Serve from `localhost`; use classic scripts if module CORS gets annoying. |
| Firmware relay drops packets | Preserve ACK-per-frame backpressure and one item in flight. |

## Recommended Execution Order

1. Build protocol harness and shared protocol module.
2. Build chat pages with mock transports.
3. Wire WebHID/Web Bluetooth adapters.
4. Fix Flipper status label.
5. Run hardware text chat.
6. Add attachment cards and previews.
7. Update docs and demo script.

## Out of Scope for MVP

- True concurrent full-duplex sending.
- Multi-peer rooms.
- Encryption/authentication.
- Large file optimization.
- Flipper-side message parsing or storage.
- Native desktop/mobile apps.

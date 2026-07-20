# Pocket AirBridge — Technical Architecture

## Overview

Pocket AirBridge is a browser-only, offline chat and attachment exchange system. It uses a **Flipper Zero** as a physical bridge between two computers, with no network infrastructure required.

- **PC-A** (USB Chat) connects to Flipper Zero over **USB HID** (vendor-defined interface).
- **PC-B** (BLE Chat) connects to Flipper Zero over **Bluetooth Low Energy** (custom GATT service).
- Both PCs run **browser-based web apps** using WebHID and Web Bluetooth APIs.
- The Flipper Zero acts as a **stateless byte pipe**: it forwards frames between the two transports and never stores the full message or attachment.

## Component Diagram

```
┌─────────┐      USB HID       ┌───────────────┐      BLE GATT      ┌─────────┐
│  PC-A   │  <───────────────> │  Flipper Zero │  <───────────────> │  PC-B   │
│(USB Chat)│   messages + ACKs  │   (Bridge)    │   messages + ACKs  │(BLE Chat│
│ WebHID  │                    │  USB + BLE    │                    │Web BT   │
└─────────┘                     └───────────────┘                     └─────────┘
```

## Component Responsibilities

### PC-A — USB Chat Web App (WebHID)
- Discovers and connects to Flipper Zero via `navigator.hid`.
- Sends text messages and file attachments to PC-B.
- Splits attachments into fixed-size chunks.
- Computes SHA-256 hash of the entire attachment before transfer.
- Sends protocol messages: **HELLO → ITEM_META → ITEM_DATA[0…N] → ITEM_DONE**.
- Waits for **ACK** after each chunk before sending the next (backpressure).
- Receives incoming text messages and attachments from PC-B over USB.
- Displays a chat transcript, progress bar, and status log.

### Flipper Zero — Bridge Firmware
- Exposes a **vendor-defined USB HID interface** (Usage Page `0xFF00`) for PC-A communication.
- Advertises a **BLE peripheral** with a custom GATT service for PC-B communication.
- Is a **stateless byte pipe**: frames arriving on one transport are forwarded verbatim to the other. The bridge does not parse protocol messages, track items, or wait for ACKs.
- Buffers only a small in-flight event queue (8 slots of one 64-byte frame each); it never stores a full message or attachment.
- Does **not** enforce half-duplex: the one-item-in-flight discipline is enforced entirely by the browser endpoints (lower `itemId` wins a collision).
- Renders a status screen: USB connection state, BLE connection state, and the forwarding counters `U->B` / `B->U` / `DROP` / `TXERR`. A green LED heartbeat blinks every 500 ms while the app runs.

### PC-B — BLE Chat Web App (Web Bluetooth)
- Discovers and connects to Flipper Zero via `navigator.bluetooth`.
- Subscribes to the custom GATT characteristic for incoming data notifications.
- Receives protocol messages, reassembles attachment chunks into a local `Blob`.
- Verifies the final SHA-256 hash against the received metadata.
- Sends **ACK** after each successfully received chunk.
- Sends text messages and file attachments to PC-A over BLE.
- Offers reconstructed attachments as a browser download.
- Displays a chat transcript, progress bar, and status log.

## Data Flow (Streaming Bridge)

### PC-A sends a text message or attachment to PC-B

1. PC-A sends **HELLO** to Flipper Zero over USB HID.
2. Flipper Zero forwards **HELLO** to PC-B over BLE.
3. PC-A sends **ITEM_META** (item type, name, MIME type, size, SHA-256).
4. Flipper Zero forwards **ITEM_META** to PC-B.
5. **Loop for each chunk:**
   a. PC-A sends **ITEM_DATA** (chunk index + payload).
   b. Flipper Zero buffers the chunk and forwards it to PC-B.
   c. PC-B receives the chunk, stores it, and sends **ACK**.
   d. Flipper Zero forwards **ACK** to PC-A.
   e. PC-A advances to the next chunk.
6. PC-A sends **ITEM_DONE**.
7. PC-B verifies the SHA-256 hash and displays the message or enables the download button.

### PC-B sends a text message or attachment to PC-A

The same flow runs in reverse: PC-B originates **HELLO**, **ITEM_META**, **ITEM_DATA**, and **ITEM_DONE** over BLE; Flipper Zero forwards each message to PC-A over USB HID; PC-A ACKs each chunk back through the bridge.

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Vendor HID (not keyboard)** | Avoids BadUSB classification and keystroke injection risks. Explicit, consent-based communication. |
| **BLE GATT notifications** | Standard Web Bluetooth API support in Chromium browsers. Low power, suitable for small data. |
| **Small chunks (~60 bytes payload)** | Fits within a single 64-byte HID report, avoiding report-fragmentation complexity for the MVP. |
| **ACK-per-chunk backpressure** | Ensures Flipper Zero never buffers more than a couple of frames. Simple, reliable for demo. |
| **SHA-256 end-to-end verification** | Guarantees attachment integrity without relying on BLE or HID transport correctness. The receiver verifies the hash before ACKing `ITEM_DONE` and reports mismatches with an `ERROR` frame. |
| **Half-duplex, one item in flight** | Prevents collisions when both sides try to send simultaneously. Lower `itemId` wins. Enforced by the browser endpoints; the bridge stays a dumb pipe. |
| **Stateless bridge** | The Flipper only copies bytes between transports. Keeping all protocol state in the endpoints means the firmware cannot desync from either side, and a bridge restart never corrupts protocol state. |
| **No encryption** | MVP scope; transport is physically local and user-controlled. Documented as a known limitation. |

## Half-Duplex Rules

Because both PC-A and PC-B can initiate sends, the **browser endpoints** enforce strict half-duplex discipline. The Flipper bridge is stateless and enforces none of this itself; it forwards every frame in both directions unconditionally.

1. **Only one item in flight at a time.** An item is a text message or an attachment transfer.
2. **Collision resolution:** If both sides send a `HELLO` simultaneously, the side with the lower `itemId` wins. The other side receives `BUSY` and must retry after a backoff.
3. **No interleaving:** Once an `ITEM_META` is accepted, all subsequent `ITEM_DATA` chunks and the final `ITEM_DONE` must come from the same side before the other side may start a new item.
4. **Cancel:** Either side may send `CANCEL` to abort the current in-flight item, including the receiver. The bridge forwards `CANCEL` transparently; both endpoints discard partial data and return to idle.

## Browser Requirements

- **Chromium-based browser** (Chrome, Edge, Brave) on both PCs.
- WebHID requires a secure context (`https://` or `localhost`) and a user gesture.
- Web Bluetooth requires a secure context and a user gesture.
- Both APIs are behind permissions prompts and cannot be used silently.

## Flipper Zero Requirements

- Flipper Zero with firmware that supports custom USB HID descriptors and BLE peripheral mode.
- Custom application (not a keyboard emulator) to handle the bridge logic and status UI.

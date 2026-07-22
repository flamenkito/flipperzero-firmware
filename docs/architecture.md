# Pocket AirBridge — Technical Architecture

## Overview

Pocket AirBridge is a browser-only, offline chat and attachment exchange system. It uses a **Flipper Zero** as a physical bridge between two computers, with no network infrastructure required.

- **PC-A** (USB Chat) connects to Flipper Zero over **USB HID** (vendor-defined interface).
- **PC-B** (BLE Chat) connects to Flipper Zero over **Bluetooth Low Energy** (the AirBridge serial GATT service).
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
- Advertises the config-driven **AirBridge BLE impersonation profile** for PC-B: Battery, DIS, HIDS, and the AirBridge serial service.
- Is a **stateless byte pipe**: frames arriving on one transport are forwarded verbatim to the other. The bridge does not parse protocol messages, track items, or wait for ACKs.
- Buffers only a small in-flight event queue (8 slots of one 64-byte frame each); it never stores a full message or attachment.
- Does **not** enforce half-duplex: the one-item-in-flight discipline is enforced entirely by the browser endpoints (lower `itemId` wins a collision).
- Renders a status screen: USB connection state, BLE connection state, and the forwarding counters `U->B` / `B->U` / `DROP` / `TXERR`. A green LED heartbeat blinks every 500 ms while the app runs.

## BLE Identity and GATT Model

The FAP reads the BLE identity from `/ext/apps_data/pocket_airbridge/config`:
`ble_name`, `ble_mac`, `ble_appearance`, `ble_mfg_company`, `ble_mfg_hex`,
`ble_dis_mfr`, `ble_dis_model`, `ble_dis_serial`, and `ble_dis_pnp`. The
configured HP identity supplies the advertised name and public MAC, GAP
appearance and manufacturer data, and DIS manufacturer/model/serial/PnP values.
Invalid identity input falls back atomically to compiled HP defaults.

The AirBridge profile advertises HIDS (`0x1812`) and includes HIDS alongside
Battery, DIS, and the AirBridge serial service. HIDS is present for the explicit
BLE Deploy typing flow only; no keyboard reports are emitted while bridging chat
traffic. The profile uses numeric-comparison pairing with authenticated GATT
characteristics but disables persistent bonding. This avoids macOS retaining a
bonded keyboard connection and starving the browser's Web Bluetooth connection.

Browser discovery and data use only the generated AirBridge serial UUID family
from `web/airbridge-identity.js`, not HIDS. The on-air UUIDs are service
`7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify
`87825ec0-7398-8cb7-3242-b083eaa34f27`, and RX write
`152f7eeb-e3b7-5898-ba41-7ff66121c98d`.

### PC-B — BLE Chat Web App (Web Bluetooth)
- Discovers and connects to Flipper Zero via `navigator.bluetooth`.
- Subscribes to the AirBridge serial TX notify characteristic for incoming data.
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

## Deploy Transport Routing

The Bridge screen provides two distinct, user-confirmed deployment paths:

| FAP control | Typing transport | Bootstrap | Streamed bundle |
|---|---|---|---|
| **UP** | USB HID keyboard | `bootstrap.js` | `app-usb.html` |
| **DOWN** | BLE HIDS keyboard | `bootstrap-ble.js` | `app-ble.html` |

Both bootstraps request the bundle with `0x42` only after a user clicks their
landing-page Connect button. USB streams fixed 64-byte vendor-HID reports;
BLE streams the same length/checksum format over the AirBridge serial notify
characteristic. The bundle itself matches the bootstrap transport, so the
deployed app opens WebHID for USB Deploy and Web Bluetooth for BLE Deploy.

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Vendor HID (not keyboard)** | For the Bridge data path, vendor HID avoids exposing a keyboard interface to the host. The Deploy flow intentionally uses keyboard emulation and is BadUSB-shaped by design; see the "Honest framing" section in README.md. |
| **AirBridge BLE notifications** | The custom serial TX characteristic uses GATT notify, which Chromium exposes through `characteristicvaluechanged`; low overhead suits small data. |
| **Small chunks (~60 bytes payload)** | Fits within a single 64-byte HID report, avoiding report-fragmentation complexity for the MVP. |
| **ACK-per-chunk backpressure** | Ensures Flipper Zero never buffers more than a couple of frames. Simple, reliable for demo. |
| **SHA-256 end-to-end verification** | Guarantees attachment integrity without relying on BLE or HID transport correctness. The receiver verifies the hash before ACKing `ITEM_DONE` and reports mismatches with an `ERROR` frame. |
| **Half-duplex, one item in flight** | Prevents collisions when both sides try to send simultaneously. Lower `itemId` wins. Enforced by the browser endpoints; the bridge stays a dumb pipe. |
| **Stateless bridge** | The Flipper only copies bytes between transports. Keeping all protocol state in the endpoints means the firmware cannot desync from either side, and a bridge restart never corrupts protocol state. |
| **No application-layer encryption or persistent bond** | The authenticated BLE link is encrypted for each numeric-comparison session, but data has no end-to-end application encryption and no bonding is retained. |

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

# Pocket AirBridge — Technical Architecture

## Overview

Pocket AirBridge is a browser-only, offline, end-to-end encrypted chat and attachment exchange system. It uses a **Flipper Zero** as a physical bridge between two computers, with no network infrastructure required.

- **PC-A** (USB Chat) connects to Flipper Zero over **USB HID** (vendor-defined interface).
- **PC-B** (BLE Chat) connects to Flipper Zero over **Bluetooth Low Energy** (the AirBridge serial GATT service).
- Both PCs run **browser-based web apps** using WebHID and Web Bluetooth APIs.
- The Flipper Zero acts as a **blind stateless byte pipe**: it forwards frames between the two transports and never stores plaintext, session keys, decrypted metadata, or a full message or attachment.

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
- Uses v2 AB2S: hashes/counts one File.stream() pass, then encrypts a second pass in 65,536-byte plaintext segments. Private metadata and SHA-256 live in authenticated segment zero.
- Sends protocol messages: **HELLO → ITEM_META → ITEM_DATA[0…N] → ITEM_DONE**.
- Sends up to four DATA chunks per batch and waits for every **ACK** before sending the next batch (backpressure).
- Receives incoming text messages and attachments from PC-B over USB.
- Displays a chat transcript, transfer state, throughput, and status log.
- Starts the browser-only crypto handshake, shows the six-digit SAS, and keeps sending disabled until both browsers accept the same SAS.

### Flipper Zero — Bridge Firmware
- Exposes a **vendor-defined USB HID interface** (Usage Page `0xFF00`) for PC-A communication.
- Advertises the config-driven **AirBridge BLE impersonation profile** for PC-B: Battery, DIS, HIDS, and the AirBridge serial service.
- Is a **blind stateless byte pipe**: frames arriving on one transport are forwarded verbatim to the other. The bridge does not parse protocol messages, track items, decrypt data, or wait for ACKs.
- Buffers only a small in-flight event queue (8 slots of one 64-byte frame each); it never stores plaintext, session keys, decrypted files, or a full message or attachment.
- Never decrypts chat data. E2E keys and plaintext exist only in the two browser endpoints.
- Does **not** enforce half-duplex: browsers order collisions by `(itemId, role)`, with USB role 1 winning an equal-ID tie.
- Renders a status screen: USB connection state, BLE connection state, and the forwarding counters `U->B` / `B->U` / `DROP` / `TXERR`. A green LED heartbeat blinks every 500 ms while the app runs.

## BLE Identity and GATT Model

The FAP reads the BLE identity from `/ext/apps_data/pocket_airbridge/config`:
`ble_name`, `ble_mac`, `ble_appearance`, `ble_mfg_company`, `ble_mfg_hex`,
`ble_dis_mfr`, `ble_dis_model`, `ble_dis_serial`, and `ble_dis_pnp`. The
configured HP identity supplies the advertised name and public MAC, GAP
appearance and manufacturer data, and DIS manufacturer/model/serial/PnP values.
Invalid identity input falls back atomically to compiled HP defaults, with the
DIS serial derived from a stable hash of the Flipper hardware UID unless
`ble_dis_serial` is explicitly configured.

The AirBridge profile includes HIDS alongside Battery, DIS, and the AirBridge
serial service. In the normal active state, the Bridge watchdog re-arms serial
plus HIDS every 2.5 seconds and restarts advertising only when GAP is idle,
without disconnecting an active link. Advertising HIDS is distinct from keyboard
emission: no keyboard reports are emitted while bridging chat traffic or on any
bridge data path. Keyboard reports exist only in the explicit, user-confirmed
Deploy typing prompts (USB Deploy over USB HID, BLE Deploy over BLE HIDS). The
profile uses numeric-comparison pairing with persistent bonding, so first
pairing needs a code check and later reconnects can be silent.

Browser discovery and data use the generated AirBridge serial UUID family
from `web/airbridge-identity.js`. The on-air UUIDs are service
`7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify
`87825ec0-7398-8cb7-3242-b083eaa34f27`, and RX write
`152f7eeb-e3b7-5898-ba41-7ff66121c98d`.

Current evidence for BLE tuning is static unless a hardware run says otherwise:
the firmware configures and supports a local ATT MTU maximum of 414, enables DLE,
prefers 2M PHY, requests a 7.5 to 45 ms connection interval, and supports a
244-byte serial value capacity. Negotiated MTU is peer-driven; negotiated radio
values and BLE throughput require physical evidence.

### PC-B — BLE Chat Web App (Web Bluetooth)
- Discovers and connects to Flipper Zero via `navigator.bluetooth`.
- Subscribes to the AirBridge serial TX notify characteristic for incoming data.
- Completes SAS-gated crypto unlock with PC-A before sending or accepting items.
- Receives one ciphertext segment at a time, authenticates it before accumulating plaintext chunks, verifies final payload size/SHA-256, then creates a local `Blob` directly from chunks.
- Sends **ACK** after each successfully received chunk.
- Sends text messages and file attachments to PC-A over BLE.
- Offers reconstructed attachments as a browser download.
- Displays a chat transcript, transfer state, throughput, and status log.
- Starts the browser-only crypto handshake, shows the six-digit SAS, and keeps sending disabled until both browsers accept the same SAS.

## Data Flow (Streaming Bridge)

### PC-A sends a text message or attachment to PC-B

1. After mutual SAS unlock and the first hash/count pass, PC-A allocates a monotonic item ID and sends v2 **HELLO** over USB HID.
2. Flipper Zero forwards **HELLO** to PC-B over BLE.
3. PC-A requires the item-bound HELLO ACK before sending **ITEM_META** with transport-only fields. Chat requests the AB2W four-frame DATA window; the receiver must explicitly grant it. An incompatible peer fails with `Unsupported protocol version`.
4. Flipper Zero forwards **ITEM_META** to PC-B.
5. **Loop for each segment and bounded DATA batch in the second stream pass:**
   a. PC-A sends **ITEM_DATA** (header chunk index, item/segment prefix, at most 51 ciphertext bytes).
   b. Flipper Zero buffers the chunk and forwards it to PC-B.
   c. PC-B accepts the slice into one bounded ciphertext segment and sends a contextual **ACK**. The final slice waits for segment authentication before plaintext enters the private accumulator.
   d. Flipper Zero forwards **ACK** to PC-A.
   e. PC-A advances to the next batch after every frame in the current batch is acknowledged. Reordered slices and exact retries stay within that bounded batch.
6. PC-A sends **ITEM_DONE**.
7. PC-B verifies all segment/header/payload counts and incremental plaintext SHA-256, constructs a Blob from authenticated chunks, delivers the verified item, then ACKs DONE. No download exists before verification.

### PC-B sends a text message or attachment to PC-A

The same flow runs in reverse: PC-B originates **HELLO**, **ITEM_META**, **ITEM_DATA**, and **ITEM_DONE** over BLE; Flipper Zero forwards each message to PC-A over USB HID; PC-A ACKs each chunk back through the bridge.

## Deploy Transport Routing

The Bridge screen provides two distinct, user-confirmed deployment paths:

| FAP control | Typing transport | Bootstrap | Streamed bundle |
|---|---|---|---|
| **LEFT/RIGHT → USB Deploy, then OK** | USB HID keyboard | `bootstrap.js` | `app-usb.html.gz` |
| **LEFT/RIGHT → BLE Deploy, then OK** | BLE HIDS keyboard | `bootstrap-ble.js` | `app-ble.html.gz` |

Both bootstraps require `DecompressionStream("gzip")`, then request the compressed
bundle with `0x42` only after a user clicks their landing-page Connect button. USB
streams fixed 64-byte vendor-HID reports; BLE streams the same length/checksum
format over the AirBridge serial notify characteristic. The bundle is deterministic gzip;
unsupported browsers fail before the picker. The bundle itself matches the bootstrap
transport, so the deployed app opens WebHID for USB Deploy and Web Bluetooth for
BLE Deploy. The FAP authenticates the normalized bootstrap and versioned bundle container
against generated SHA-256 constants before typing or streaming; the container
also pins the decompressed HTML size and carries explicit `ABND` magic/version.

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Vendor HID Bridge data path** | The selected composite USB personality exposes keyboard and vendor collections to the host. Bridge frames use only the vendor HID collection on usage page `0xFF00`; no keyboard reports are emitted in Bridge mode or on bridge data paths. Keyboard reports are limited to the explicit, user-confirmed Deploy flow; see the "Honest framing" section in README.md. |
| **AirBridge BLE notifications** | The custom serial TX characteristic uses GATT notify, which Chromium exposes through `characteristicvaluechanged`; low overhead suits small data. |
| **51-byte encrypted DATA slices** | Eight payload bytes bind item and segment; the five-byte header and 59-byte outer payload still fit one 64-byte report. |
| **Bounded DATA windows** | Up to four outstanding DATA frames, with an ACK and exact retry copy per frame, reduce round-trip stalls while preserving the eight-slot Flipper queue. Control frames remain stop-and-wait. |
| **Browser E2E encryption and SHA-256 verification** | Unchanged cryptoVersion 1 SAS/P-256/HKDF protects v2 segmented AES-GCM. Authenticate each segment before retaining plaintext; final payload SHA-256 and counts precede Blob/DONE ACK. |
| **Half-duplex, one item in flight** | Lexicographic `(itemId, role)` ordering, USB winning ties; enforced in browsers, not firmware. |
| **Stateless bridge** | The Flipper only copies bytes between transports. Keeping all protocol state in the endpoints means the firmware cannot desync from either side, and a bridge restart never corrupts protocol state. |
| **Fail-closed SAS unlock** | Sending stays disabled until both browser endpoints accept the same six-digit SAS and receive a matching peer confirmation. Plaintext item frames before unlock are terminal protocol errors. |
| **Exact-frame NACK only** | Match type, sequence, AB2S, itemId and segment. Retry only the outstanding frame; no byte-range or cross-session resume. Conflicting duplicates and AEAD failures are terminal. |
| **Evidence-gated stealth work** | Typing jitter and per-device DIS serials are implemented. BLE service UUID hiding and Windows USB tree comparison remain deferred until hardware and Windows evidence exist. |

## Half-Duplex Rules

Because both PC-A and PC-B can initiate sends, the **browser endpoints** enforce strict half-duplex discipline. The Flipper bridge is stateless and enforces none of this itself; it forwards every frame in both directions unconditionally.

1. **Only one item in flight at a time.** An item is a text message or an attachment transfer.
2. **Collision resolution:** Order simultaneous HELLOs by `(itemId, role)`; lower ID wins and USB role 1 wins ties. The losing operation terminates; a fresh attempt burns a fresh ID.
3. **No interleaving:** Once an `ITEM_META` is accepted, all subsequent `ITEM_DATA` chunks and the final `ITEM_DONE` must come from the same side before the other side may start a new item.
4. **Cancel:** Either side may send `CANCEL` to abort the current in-flight item, including the receiver. The bridge forwards `CANCEL` transparently; both endpoints discard partial data and return to idle.

## Browser Requirements

### Memory, security and lifecycle contract

[Protocol v2](protocol.md) centralizes exact fields, segment math, IV/AAD,
monotonic ID allocation and retry semantics. See the root
[ADR 0001](../../docs/adr/0001-airbridge-protocol-v2-streaming.md) for alternatives.
The unchanged cryptoVersion 1 handshake does not imply v1 item compatibility.
An incompatible peer locks both user-facing paths with
`Unsupported protocol version`; load current pages, reconnect and confirm fresh
SAS. Ordinary corruption/authentication errors remain distinct and fail closed.

Ciphertext is segment-bounded; retained authenticated plaintext is intentionally
**O(file size)** in browser memory until final size/hash verification and Blob
creation. The final Blob is built directly from chunks, not a giant Uint8Array.
Completed cards own their object URLs; removal/replacement/page disposal revokes
exactly once without leaving a live revoked link. Active cancellation,
disconnect, crypto reset and errors invalidate generation tokens and release
partial buffers; stale asynchronous callbacks cannot mutate the next item.

V2 removes the historical **4 MiB/4096-chunk demo ceiling**, not practical bounds:
uint32 IDs/segment counts, uint64 sizes, browser heap/API limits, Blob/download/
storage capacity, BLE/USB throughput and transfer time. No whole-item sender or
ciphertext buffering, disk/OPFS/File System Access receive, save picker,
direct-to-disk path or mixed-version fallback is supported. Text is memory-backed.
SAS authenticates browser peers, not the relay hardware; endpoint compromise and
traffic analysis remain outside this confidentiality guarantee.

### Platform requirements

- **Chromium-based browser** (Chrome, Edge, Brave) on both PCs.
- WebHID requires a secure context (`https://` or `localhost`) and a user gesture.
- Web Bluetooth requires a secure context and a user gesture.
- Both APIs are behind permissions prompts and cannot be used silently.

## Flipper Zero Requirements

- Flipper Zero with firmware that supports custom USB HID descriptors and BLE peripheral mode.
- Custom application (not a keyboard emulator) to handle the bridge logic and status UI.

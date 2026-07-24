# Pocket AirBridge — Message Protocol Specification

## Transport Layer

- **USB HID**: Vendor-defined Usage Page `0xFF00`, Usage `0x01`. Report size: 64 bytes.
- **BLE GATT**: AirBridge serial service used as a byte pipe. The browser uses the on-air UUIDs generated in `web/airbridge-identity.js`: service `7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify `87825ec0-7398-8cb7-3242-b083eaa34f27`, and RX write `152f7eeb-e3b7-5898-ba41-7ff66121c98d`. MTU assumed ≥64 bytes.

Because a single HID report is 64 bytes, the protocol message fits within one report. The maximum payload per message is **59 bytes** (64 minus 5-byte header). For the MVP, chunk payload is capped at **59 bytes**. Larger files are simply split into more chunks.

### BLE identity and service composition

The FAP reads `ble_name`, `ble_mac`, `ble_appearance`, `ble_mfg_company`,
`ble_mfg_hex`, `ble_dis_mfr`, `ble_dis_model`, `ble_dis_serial`, and
`ble_dis_pnp` from `/ext/apps_data/pocket_airbridge/config`. These values set
the advertising identity and DIS values for the AirBridge BLE profile. The
profile includes Battery, DIS, HIDS, and AirBridge serial; Web Bluetooth uses
only the serial service. HIDS exists for explicit BLE Deploy typing and carries
no Bridge-mode keyboard traffic.

The serial service UUID is advertised continuously. HIDS is advertised only
during the BLE Deploy prompt and typing window, so macOS sees the keyboard only
when it is needed for explicit deploy typing. Bonding is enabled: each host's
first pairing uses MITM numeric comparison, and later reconnects use the stored
bond silently. The serial service's additional UUIDs are flow control notify
`d2d968bf-cbd8-568f-d24c-5bbddb824f25` and status notify/read/write
`bebb7113-63db-bbae-bb45-37dbbf73b6b3`.

## Wire Format

Every message is a binary blob with the following layout:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0      | 1    | `type` | Message type identifier |
| 1      | 2    | `seq`  | Sequence number (big-endian uint16) |
| 3      | 2    | `len`  | Payload length in bytes (big-endian uint16) |
| 5      | N    | `payload` | Variable-length payload (N = `len`) |

Total message size: `5 + len` bytes. `len` may be `0`.

## Message Types

| Code | Name   | Direction | Payload Description |
|------|--------|-----------|---------------------|
| `0x01` | **HELLO** | Any → Bridge → Other | Item initialization. Payload: 4-byte `itemId` (big-endian uint32). |
| `0x02` | **ITEM_META**  | Any → Bridge → Other | Item metadata fragment. Payload: 2-byte total-fragment count followed by a UTF-8 JSON slice. |
| `0x03` | **ITEM_DATA**  | Any → Bridge → Other | Item chunk. Payload: raw binary chunk. |
| `0x04` | **ACK**   | Any → Bridge → Other | Confirm receipt of a chunk or control frame. Payload: 3 bytes — `acked_type` (1 byte) + `acked_seq` (2-byte big-endian uint16). |
| `0x05` | **NACK**  | Any → Bridge → Other | Defined but currently unused (reserved). Payload: 2-byte chunk index (big-endian uint16). |
| `0x06` | **ITEM_DONE**  | Any → Bridge → Other | Item transfer complete. Payload: empty. |
| `0x07` | **ERROR** | Any → Any | Fatal error. Payload: UTF-8 error string. |
| `0x08` | **BUSY**  | Any → Bridge → Other | Receiver is busy with another item and rejected the HELLO. Payload: empty. |
| `0x09` | **CANCEL**| Any → Bridge → Other | Abort the current in-flight item. May originate from the sender **or** the receiver. Payload: empty. |

## ITEM_META Payload Schema (JSON)

Metadata is sent as one or more `ITEM_META` frames because the JSON usually exceeds the 59-byte frame payload limit. Each `ITEM_META` frame uses:

| Field | Location | Description |
|-------|----------|-------------|
| Fragment index | Message header `seq` | Zero-based metadata fragment index. |
| Total fragments | Payload bytes `0..1` | Big-endian uint16 count of metadata fragments. |
| JSON slice | Payload bytes `2..N` | Consecutive UTF-8 bytes from the metadata JSON document. |

The receiver ACKs every `ITEM_META` frame with the same fragment index in the ACK payload. After all metadata fragments arrive, the receiver concatenates the JSON slices in `seq` order and parses the result.

```json
{
  "kind": "text",
  "name": "message.txt",
  "mimeType": "text/plain",
  "size": 120,
  "chunks": 3,
  "hash": "sha256:a1b2c3..."
}
```

- `kind`: `"text"` for a plain text message, `"attachment"` for a file.
- `name`: display name or filename.
- `size`: total item size in bytes.
- `chunks`: total number of `ITEM_DATA` chunks expected.
- `hash`: lowercase hex SHA-256 hash of the entire item, prefixed with `sha256:`.

## State Machines

### Sender State Machine (Local ItemSender)

```
[IDLE] --(user composes message or selects file)--> [READY]
[READY] --(send HELLO with itemId)--> [WAITING_HELLO_ACK]
[WAITING_HELLO_ACK] --(receive ACK for HELLO)--> [SENDING_ITEM_META]
[SENDING_ITEM_META] --(receive ACK for each ITEM_META fragment)--> [SENDING_ITEM_DATA]
[SENDING_ITEM_DATA] --(send chunk, wait ACK)--> [SENDING_ITEM_DATA]
[SENDING_ITEM_DATA] --(all chunks ACKed)--> [SENDING_ITEM_DONE]
[SENDING_ITEM_DONE] --(receive ACK for ITEM_DONE)--> [COMPLETE]
[Any] --(receive ERROR)--> [FAILED]
[Any] --(receive CANCEL)--> [CANCELLED]
```

### Receiver State Machine (Local ItemReceiver)

```
[IDLE] --(user pairs device)--> [CONNECTING]
[CONNECTING] --(connected, wait HELLO)--> [WAITING_HELLO]
[WAITING_HELLO] --(receive HELLO)--> [WAITING_ITEM_META]
[WAITING_ITEM_META] --(receive all ITEM_META fragments)--> [RECEIVING_ITEM_DATA]
[RECEIVING_ITEM_DATA] --(receive ITEM_DATA chunk)--> (store, send ACK)
[RECEIVING_ITEM_DATA] --(receive ITEM_DONE)--> [VERIFYING]
[VERIFYING] --(hash matches)--> [COMPLETE]
[VERIFYING] --(hash mismatch)--> [FAILED]
[Any] --(receive ERROR)--> [FAILED]
[Any] --(receive CANCEL)--> [CANCELLED]
```

### Bridge State Machine (Flipper Zero)

**Not implemented.** The shipped Flipper firmware is a stateless byte pipe: it
holds no per-item state, does not parse these message types, and does not wait
for ACKs. Every frame that arrives on one transport is forwarded verbatim to
the other. All states below live in the browser endpoints (the sender and
receiver state machines above), which is also where half-duplex discipline is
enforced.

## Chunking Rules

1. **Chunk index** is the `seq` field in the `ITEM_DATA` message header.
2. Chunk indices start at `0` and increment by `1`.
3. The last chunk may be smaller than the maximum payload size.
4. Total chunks = `ceil(itemSize / MAX_PAYLOAD)`.

## Half-Duplex Discipline

Both sides can initiate transfers, so the **browser endpoints** enforce strict half-duplex rules (the bridge itself is stateless and enforces nothing):

1. **One item in flight:** Only one `HELLO` / `ITEM_META` / `ITEM_DATA` / `ITEM_DONE` sequence may be active at any time across the entire bridge.
2. **Collision resolution:** If both sides send `HELLO` simultaneously, the side with the lower `itemId` wins. The other side receives `BUSY` and must retry after a backoff.
3. **No interleaving:** Once an `ITEM_META` is accepted, all `ITEM_DATA` chunks and `ITEM_DONE` for that item must complete before a new `HELLO` from either side is accepted.
4. **Cancel:** Either side may send `CANCEL` at any time to abort the current in-flight item — the sender can cancel its own transfer, and the receiver can cancel a transfer it is receiving. The bridge forwards `CANCEL` transparently; the receiving side discards partial data, resets its receiving state, and returns to idle. The peer receiving a `CANCEL` ACKs it and also returns to idle.

## Consent Model

There is no application-level authentication or encryption (see Known Limitations). The explicit user consent for a transfer comes from the platform:

1. **First BLE pairing numeric comparison on the Flipper screen** — the AirBridge serial service requires MITM-authenticated pairing; the user confirms the code once for each host. Bonding is enabled, so later reconnects from that host reuse the stored bond silently.
2. **Browser permission pickers** — WebHID (PC-A) and Web Bluetooth (PC-B) require a user gesture and an explicit device-selection dialog before a browser receives its initial device grant. A previously granted browser can reconnect with `getDevices()` without reopening the picker.

A transfer therefore requires first-pairing physical consent on the Flipper plus
explicit initial per-browser consent on both PCs.

## ACK/NACK Semantics

- **ACK** payload is 3 bytes: `acked_type` (1 byte) followed by `acked_seq` (2-byte big-endian uint16). It names both the message type and the sequence being confirmed, so HELLO, ITEM_META, ITEM_DATA, ITEM_DONE, and CANCEL ACKs can never be confused with one another even though they share sequence numbers.
- The sender may proceed to the next frame only after receiving an ACK matching both the type and sequence of the current one.
- The receiver verifies the SHA-256 hash of the reassembled item **before** ACKing `ITEM_DONE`. On mismatch (or any validation failure such as oversized metadata or a missing chunk) it does **not** ACK; instead it sends **ERROR** with a UTF-8 reason string. A sender that receives **ERROR** while a send is in flight treats the send as failed and surfaces the peer's reason to the user.
- **NACK** (`0x05`) is defined but currently unused (reserved for future retransmission requests). Loss recovery today relies on sender-side timeouts and retries.
- If no ACK is received within a timeout (5 seconds), the sender retries the same frame up to 3 times before failing the transfer.
- Defensive receiver behavior worth knowing:
  - `ITEM_DATA` received before any `ITEM_META` is dropped silently (logged locally, no ERROR sent). This absorbs stale frames from a cancelled or yielded transfer without poisoning the next one.
  - Metadata declaring more than 4096 chunks or more than 4 MiB is rejected with **ERROR** (bounded demo memory).

## Example Transfer Flow (text message, 120 bytes, PC-A to PC-B)

```
PC-A (USB)      Bridge          PC-B (BLE)
----------      ------          ----------
  |               |                  |
  |--- HELLO ---->|                  |
  |               |--- HELLO ------->|
  |               |<-- ACK(HELLO) ---|
  |<-- ACK -------|                  |
  |               |                  |
  |--- ITEM_META(0) -->|            |
  |               |--- ITEM_META(0) ->|
  |               |<-- ACK(0) -------|
  |<-- ACK(0) ----|                  |
  |--- ITEM_META(1) -->|            |
  |               |--- ITEM_META(1) ->|
  |               |<-- ACK(1) -------|
  |<-- ACK(1) ----|                  |
  |               |                  |
  |--- ITEM_DATA(0) -->|            |
  |               |--- ITEM_DATA(0) ->|
  |               |<-- ACK(0) -------|
  |<-- ACK(0) ----|                  |
  |               |                  |
  |--- ITEM_DATA(1) -->|            |
  |               |--- ITEM_DATA(1) ->|
  |               |<-- ACK(1) -------|
  |<-- ACK(1) ----|                  |
  |               |                  |
  |--- ITEM_DONE ->|                |
  |               |--- ITEM_DONE --->|
  |               |<-- ACK(DONE) ----|
  |<-- ACK -------|                  |
  |               |                  |
```

## Error Handling

- Any side may send **ERROR** at any time to abort the current item.
- Any side may send **CANCEL** at any time to abort the current item gracefully.
- Upon receiving ERROR, both sides transition to `[FAILED]`, discard buffers, and display the error message to the user.
- Upon receiving CANCEL, both sides transition to `[CANCELLED]`, discard partial data, and display a cancellation notice.
- The bridge forwards ERROR and CANCEL messages transparently in both directions.

## Bootstrap Stream Protocol (Deploy Flow)

This is a separate, deliberately simpler protocol from the chat protocol above. It exists for the Deploy flow: a one-shot, download-only transfer of the app bundle from the Flipper to a just-typed bootstrap page on the target PC. The selected deploy transport routes each bootstrap to its matching bundle:

| Deploy control | Typed bootstrap | Bundle | Delivery channel |
|---|---|---|---|
| **UP** | `bootstrap.js` | `app-usb.html` | USB vendor HID |
| **DOWN** | `bootstrap-ble.js` | `app-ble.html` | AirBridge serial TX notify |

The FAP loads the matching pair from `/ext/apps_data/pocket_airbridge/`. No
per-chunk ACK is used for the bootstrap stream; USB interrupt IN reports are
hardware-reliable and BLE retries transient notification congestion.

1. **Request (bootstrap → FAP):** byte 0 = `0x42` (`'B'`, bundle request), sent as a 64-byte USB report or a BLE RX write.
2. **Header (FAP → bootstrap):** the first response carries a 4-byte little-endian `total_len` (bundle size in bytes) followed by a 4-byte little-endian `checksum` (the additive uint32 sum of all file bytes). USB pads the report to 64 bytes; BLE sends the eight header bytes directly on TX notify.
3. **Data (FAP → bootstrap):** the bundle bytes follow in order, in up-to-64-byte units. USB uses zero-padded 64-byte reports; BLE sends raw notification payloads.
4. **Verify and load:** the bootstrap accumulates exactly `total_len` bytes, recomputes the additive checksum, and boots the transferred HTML only when the values match.
5. **Failure:** on checksum mismatch the landing page shows `Transfer corrupt — retry` and the Connect button re-arms, so the user can click Connect again to restart the download.

The additive checksum exists to catch profile-switch race bytes at the start of the stream. It is not a security measure; the trust boundary is physical delivery from the user's own Flipper.

## Known Limitations (MVP)

- No end-to-end application encryption or authentication beyond first-pairing BLE numeric comparison and browser permission pickers (see Consent Model).
- Single item in flight at a time (half-duplex, enforced by the browser endpoints).
- Single receiver per session.
- No resume or partial transfer recovery.
- No compression.
- Fixed small chunk size optimized for HID report size, not throughput. Observed throughput on hardware is roughly **0.5–2 KB/s** — fine for text and small attachments, slow for anything larger.

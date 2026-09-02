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

If `ble_dis_serial` is omitted, the FAP derives a stable default DIS serial from
an FNV-1a hash of the local firmware UID and formats it as `HP` plus eight
uppercase hex digits. Explicit config still wins, and the raw Flipper UID is not
copied into the DIS string.

Setup briefly clears the HIDS advertising latch after profile installation. In the
normal active state, the Bridge watchdog re-arms serial plus HIDS every 2.5 seconds
and restarts advertising only when GAP is idle, without disconnecting an active link.
Advertising HIDS is not keyboard activity: Bridge mode and bridge data paths emit no
keyboard reports. Keyboard reports remain limited to the explicit, user-confirmed
Deploy typing flow. Bonding is enabled: each host's first pairing uses MITM numeric
comparison, and later reconnects use the stored bond silently. The serial service's
additional UUIDs are flow control notify
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
| `0x05` | **NACK**  | Any → Bridge → Other | Recoverable retransmission request. Payload: 4 bytes — `nacked_type` (1 byte) + `nacked_seq` (2-byte big-endian uint16) + `reason` (1 byte). |
| `0x06` | **ITEM_DONE**  | Any → Bridge → Other | Item transfer complete. Payload: empty. |
| `0x07` | **ERROR** | Any → Any | Fatal error. Payload: UTF-8 error string. |
| `0x08` | **BUSY**  | Any → Bridge → Other | Receiver is busy with another item and rejected the HELLO. Payload: empty. |
| `0x09` | **CANCEL**| Any → Bridge → Other | Abort the current in-flight item. May originate from the sender **or** the receiver. Payload: empty. |
| `0x0A` | **KEY_OFFER** | USB → Bridge → BLE | Crypto v1 public-key fragment from the USB endpoint. |
| `0x0B` | **KEY_REPLY** | BLE → Bridge → USB | Crypto v1 public-key fragment from the BLE endpoint. |
| `0x0C` | **KEY_CONFIRM** | Any → Bridge → Other | Post-SAS unlock confirmation for the current transcript. |
| `0x0D` | **KEY_ABORT** | Any → Bridge → Other | Abort crypto handshake/session. Payload: `version:u8`, `reason:u8`, optional UTF-8 detail. |

## Always-On End-to-End Crypto v1

The browser endpoints require crypto immediately after transport connect. The
Flipper remains a blind relay: it forwards `KEY_*`, `HELLO`, `ITEM_META`,
`ITEM_DATA`, `ITEM_DONE`, `ACK`, `NACK`, `ERROR`, `BUSY`, and `CANCEL` frames without
decrypting or storing user plaintext. There is no plaintext compatibility mode:
`HELLO`, `ITEM_META`, `ITEM_DATA`, or `ITEM_DONE` received before mutual unlock
is a terminal protocol error.

### Handshake

- `AIRBRIDGE_CRYPTO_V1 = 1`.
- Roles are fixed: USB is `1`, BLE is `2`.
- USB sends `KEY_OFFER` after transport connect. BLE sends `KEY_REPLY` only
  after validating the complete offer.
- Public keys are WebCrypto P-256 ECDH raw uncompressed points (`65` bytes,
  `0x04 || X32 || Y32`). Fragmentation uses normal frame `seq`:
  - `seq=0`: `version:u8`, `role:u8`, `total_len:u16be`, then up to 55 key bytes.
  - `seq>0`: remaining key bytes, up to 59 bytes.
- Every `KEY_OFFER`, `KEY_REPLY`, and `KEY_CONFIRM` fragment is ACKed with the
  existing ACK payload shape `[acked_type, acked_seq:u16be]`. Senders retry each
  handshake frame after a 1000 ms ACK timeout, up to 3 attempts, then send
  `KEY_ABORT` reason `3` (`timeout`). Duplicate identical key fragments are
  ACKed idempotently; conflicting duplicate payloads abort with reason `4`
  (`conflict`).
- The transcript is
  `"PocketAirBridge-crypto-v1" || 0x01 || usbPubRaw || 0x02 || blePubRaw`.
- Both browsers compute `sasInt = first20bits(SHA-256("PocketAirBridge SAS v1" || transcript))`
  and display `(sasInt % 1000000)` as exactly six decimal digits.
- `KEY_CONFIRM` is sent only after local SAS acceptance. Payload:
  `version:u8`, `status:u8` (`1` = accepted; `2` is reserved and does not
  unlock), `transcript_hash16` (first 16 bytes of `SHA-256(transcript)`). A
  browser enters `Unlocked` only after local SAS acceptance and a matching peer
  `KEY_CONFIRM` are both true. SAS mismatch sends `KEY_ABORT` reason `2` and
  clears keys.

### Key derivation

Each endpoint derives ECDH shared bits with
`deriveBits({name:"ECDH", public: remotePub}, localPriv, 256)`, imports those
bits as HKDF material, and uses HKDF-SHA256 with
`salt = SHA-256("PocketAirBridge salt v1" || transcript)`.

Derived outputs:

| Use | HKDF info |
|---|---|
| USB→BLE AES-GCM-256 key | `PocketAirBridge v1 USB->BLE item key` |
| BLE→USB AES-GCM-256 key | `PocketAirBridge v1 BLE->USB item key` |
| Key ID (first 16 bytes, base64url no padding) | `PocketAirBridge v1 key id` |
| USB→BLE nonce prefix (4 bytes) | `PocketAirBridge v1 usb-to-ble nonce prefix` |
| BLE→USB nonce prefix (4 bytes) | `PocketAirBridge v1 ble-to-usb nonce prefix` |

Directional IVs are `prefix32 || item_counter:u64be`. The counter starts at `0`
per handshake/direction and increments once per encrypted item. Receivers reject
replayed `(keyId, direction, itemCounter)` values within a session.

### Encrypted item envelope

After unlock, the application wraps the complete user-visible item into an
inner plaintext envelope and AES-GCM encrypts that envelope before chunking.
Outer `ITEM_DATA` frames carry only WebCrypto AES-GCM output bytes
(`ciphertext || 16-byte tag`).

Inner plaintext envelope:

| Field | Encoding |
|---|---|
| Magic | ASCII `AB1` |
| `itemId` | `u32be` |
| `kind` | `u8`: `1=text`, `2=attachment` |
| `nameLen`, `mimeLen` | `u16be`, `u16be` |
| `plainSize` | `u64be` |
| `plainSha256` | 32 raw digest bytes |
| `name`, `mime`, `data` | UTF-8 name, UTF-8 MIME, raw item bytes |

Outer metadata has `kind:"encrypted"` and crypto transport fields only:
`cryptoVersion`, `alg:"AES-GCM-256"`, `keyId`, `direction`, `iv` (base64),
`encryptedSize`, `encryptedSha256`, and original `itemId`. `encryptedSha256`
is lowercase hex over the exact WebCrypto output bytes and verifies ciphertext
reassembly before decrypt; the inner `plainSha256` is verified after decrypt for
user-visible integrity.

AES-GCM AAD is canonical bytes:
`"AB1-AAD" || cryptoVersion:u8 || itemId:u32be || direction:u8 || iv12 || encryptedSize:u64be`.
`direction` is `"usb-to-ble"` (`1`) for the USB→BLE key and
`"ble-to-usb"` (`2`) for the BLE→USB key. Receivers reject metadata whose
direction, key ID, IV prefix, selected key, sender role, or AAD does not match.

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

Transport and browser access still require explicit platform consent gates:

1. **First BLE pairing numeric comparison on the Flipper screen** — the AirBridge serial service requires MITM-authenticated pairing; the user confirms the code once for each host. Bonding is enabled, so later reconnects from that host reuse the stored bond silently.
2. **Browser permission pickers** — WebHID (PC-A) and Web Bluetooth (PC-B) require a user gesture and an explicit device-selection dialog before a browser receives its initial device grant. A previously granted browser can reconnect with `getDevices()` without reopening the picker.

A transfer therefore requires first-pairing physical consent on the Flipper plus
explicit initial per-browser consent on both PCs. Application-level
confidentiality and authenticity come from the SAS-verified browser-to-browser
E2E crypto v1 handshake described above; plaintext item frames before mutual
unlock fail closed, and there is no unencrypted compatibility mode.

## ACK/NACK Semantics

- **ACK** payload is 3 bytes: `acked_type` (1 byte) followed by `acked_seq` (2-byte big-endian uint16). It names both the message type and the sequence being confirmed, so HELLO, ITEM_META, ITEM_DATA, ITEM_DONE, and CANCEL ACKs can never be confused with one another even though they share sequence numbers.
- **NACK** payload is 4 bytes: `nacked_type` (1 byte), `nacked_seq` (2-byte big-endian uint16), and `reason` (1 byte). Reasons are `1=missing`, `2=malformed`, `3=auth-failed-retryable`, and `4=busy-window`.
- NACK references the outer protocol frame type and sequence only: `ITEM_META`, `ITEM_DATA`, or `ITEM_DONE`. It never requests byte offsets, plaintext offsets, or transfer resume.
- The sender may proceed to the next frame only after receiving an ACK matching both the type and sequence of the current one. A matching NACK retransmits only the exact referenced frame when retry budget remains.
- The receiver verifies the SHA-256 hash of the reassembled item **before** ACKing `ITEM_DONE`. Identifiable missing outer frames are NACKed. Hash mismatch, oversized metadata, AES-GCM authentication failure after full ciphertext reassembly, and non-identifiable validation failures send **ERROR** with a UTF-8 reason string. A sender that receives **ERROR** while a send is in flight treats the send as failed and surfaces the peer's reason to the user.
- The receiver sends NACK for the next expected outer frame when it observes a sequence gap or malformed recoverable frame and no correct frame arrives within 750 ms. Duplicate frames whose payload matches an already accepted frame are ACKed idempotently; conflicting duplicates are NACKed as malformed.
- If no ACK or NACK is received within 2000 ms for an in-flight item frame, the sender retries the same frame. Retry budget is 3 retransmissions per `(frame_type, seq)` per item; the sender retains current-item frames until `ITEM_DONE` ACK or terminal `ERROR` so retransmission is byte-exact.
- Retry exhaustion sends terminal **ERROR** and fails the transfer. AES-GCM authentication failure after full ciphertext decrypt is also terminal **ERROR**, not a NACK loop; outer NACK is only for identifiable missing or malformed protocol frames before decrypt.
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
| **LEFT/RIGHT → USB Deploy, then OK** | `bootstrap.js` | `app-usb.html.gz` | USB vendor HID |
| **LEFT/RIGHT → BLE Deploy, then OK** | `bootstrap-ble.js` | `app-ble.html.gz` | AirBridge serial TX notify |

The FAP loads the matching pair from `/ext/apps_data/pocket_airbridge/`. No
per-chunk ACK is used for the bootstrap stream; USB interrupt IN reports are
hardware-reliable and BLE retries transient notification congestion.

1. **Request (bootstrap → FAP):** byte 0 = `0x42` (`'B'`, bundle request), sent as a 64-byte USB report or a BLE RX write.
2. **Header (FAP → bootstrap):** the first response carries a 4-byte little-endian `total_len` (bundle size in bytes) followed by a 4-byte little-endian `checksum` (the additive uint32 sum of all file bytes). USB pads the report to 64 bytes; BLE sends the eight header bytes directly on TX notify.
3. **Data (FAP → bootstrap):** the bundle bytes follow in order, in up-to-64-byte units. USB uses zero-padded 64-byte reports; BLE sends raw notification payloads.
4. **Verify, inflate, and load:** the bootstrap accumulates exactly `total_len` compressed bytes, recomputes the additive checksum, inflates the gzip with `DecompressionStream("gzip")`, and boots the transferred HTML only when the values match.
5. **Failure:** if `DecompressionStream("gzip")` is unavailable, the landing page shows `Transfer unsupported - retry` before device selection. On checksum mismatch it shows `Transfer corrupt - retry`; if no stream begins or completes before the bootstrap timeout, it shows `Transfer timed out - retry`. In each case the Connect button re-arms, so the user can click Connect again to restart the download.

The FAP only treats `0x42` as a bundle request while the screen is the post-consent Deploy waiting state. In Bridge mode, a report whose first byte is `0x42` remains ordinary relay data and cannot start Deploy streaming. In other non-waiting Deploy states, the FAP shows `DEPLOY NOT ARMED` instead of silently hanging the operator in an ambiguous state.

The additive checksum exists to catch profile-switch race bytes at the start of the stream. It is not a security measure; the trust boundary is physical delivery from the user's own Flipper.

## Known Limitations (MVP)

- End-to-end encryption is browser-only and SAS-authenticated; it does not authenticate the Flipper hardware identity beyond browser picker consent and BLE pairing.
- Single item in flight at a time (half-duplex, enforced by the browser endpoints).
- Single receiver per session.
- No byte-range resume or cross-session transfer resume; NACK only retries exact current-item outer frames.
- Deploy bundles are gzip-compressed; chat items are encrypted but not compressed.
- Runtime BLE radio values are not implied by static firmware settings. The current firmware configures and supports a local ATT MTU maximum of 414, enables DLE, prefers 2M PHY, and requests a 7.5 to 45 ms interval; negotiated MTU is peer-driven, and negotiated MTU/PHY/DLE/interval and throughput need hardware logs.
- Fixed small chunk size optimized for HID report size, not throughput. Observed throughput on hardware is roughly **0.5–2 KB/s** — fine for text and small attachments, slow for anything larger.

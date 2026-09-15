# Pocket AirBridge — Protocol v2

This is the normative current chat specification. Protocol v2 (`AB2S`) keeps
`cryptoVersion:1`, P-256 ECDH, HKDF, AES-GCM and mutual SAS confirmation.
There is no v1/v2 fallback or plaintext mode. See
[ADR 0001](../../docs/adr/0001-airbridge-protocol-v2-streaming.md) for the decision
and [architecture](architecture.md) for ownership and security boundaries.

## Transport and framing

USB vendor HID uses usage page `0xFF00`, usage `0x01`, report ID 0 and 64-byte
reports. BLE AirBridge serial is a byte pipe carrying the same frames. Its
generated UUIDs live in `web/airbridge-identity.js`: service
`7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify
`87825ec0-7398-8cb7-3242-b083eaa34f27`, RX write
`152f7eeb-e3b7-5898-ba41-7ff66121c98d`. Battery, DIS, and HIDS accompany serial;
HIDS exists for explicit BLE Deploy typing and carries no Bridge-mode keyboard
traffic. Identity, bonding and radio settings are described in
[architecture](architecture.md); negotiated throughput requires hardware evidence.

All multibyte chat integers are big-endian. The five-byte outer header is:

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 1 | `type:u8` |
| 1 | 2 | `seq:u16be` |
| 3 | 2 | `len:u16be`, at most 59 |
| 5 | len | payload |

Parsers accept exactly `5 + len` bytes or one 64-byte report with zero padding.
Truncation, excess bytes and nonzero padding fail closed. Outer payload capacity
is 59 bytes; encrypted DATA capacity is **51 bytes**, not 59.

## Item wire layouts

`magic` means four ASCII bytes `AB2S`. `itemId` and `segment` are uint32be.
`V2_CONTROL_SEGMENT = 0xffffffff` is the control-context sentinel, not an extra
field on HELLO/META/DONE/CANCEL. All control outer `seq` values are zero except
META fragment indices. DATA `seq` is the chunk index within its segment.

| Type | Payload |
|---|---|
| HELLO `0x01` | `itemId[4] || magic[4]` |
| ITEM_META `0x02` | `fragmentCount:u16be || JSON slice[1..57]` |
| ITEM_DATA `0x03` | `itemId[4] || segmentIndex[4] || ciphertextSlice[0..51]` |
| ACK `0x04` | `ackedType:u8 || ackedSeq:u16be || magic[4] || itemId[4] || segment[4]` (15 bytes) |
| NACK `0x05` | ACK context followed by `reason:u8` (16 bytes) |
| ITEM_DONE `0x06` | `itemId[4]` |
| ERROR `0x07` | `magic[4] || itemId[4] || reasonUtf8[0..51]` after capability proof |
| BUSY `0x08` | `magic[4] || itemId[4]` |
| CANCEL `0x09` | `itemId[4]` |
| KEY_OFFER `0x0a` | Crypto public-key fragment from USB |
| KEY_REPLY `0x0b` | Crypto public-key fragment from BLE |
| KEY_CONFIRM `0x0c` | Crypto SAS confirmation |
| KEY_ABORT `0x0d` | `version:u8 || reason:u8 || optional UTF-8 detail` |

The framing parser can represent an empty DATA slice; the segment receiver
requires exactly the next expected nonempty slice length (1..51). The context
prefix is excluded from ciphertext totals and plaintext hashes. META has no
item prefix: the accepted HELLO binds it, and every META ACK binds that item.

### Explicit incompatibility

The extended HELLO ACK proves `AB2S` before the sender emits any META or DATA.
A v1 four-byte HELLO, absent/malformed HELLO magic, plain/legacy ACK, absent or
malformed ACK capability proof, or a peer explicitly rejecting streaming fails
with the exact reason **`Unsupported protocol version`**. Before capability is
established the receiver sends legacy UTF-8 ERROR containing that exact reason,
not an item-scoped error that an old peer cannot read. This is graceful failure,
not compatibility. KEY ACKs cannot prove item capability.

An unlocked peer that advertises AB2S but later supplies plaintext/old META is
also rejected with that reason before accepting META or any DATA. It is not
possible to detect such a lying peer's future META at HELLO; fragmented bytes
must first arrive for validation. No valid META event, segment receiver or
plaintext accumulator is created for that schema. Current-v2 malformed fields,
bad framing, wrong context, invalid UTF-8, authentication and hash errors keep
their own errors; they are not broadly relabeled as incompatibility.

Both USB and BLE show/log the same incompatibility reason, lock the failed
session and require reconnect plus a fresh mutually confirmed session. Use the
current v2 page on both endpoints; do not retry an old peer in plaintext.
Items before crypto unlock are rejected with the same reason and clear keys.
General connection/KEY errors remain non-item-scoped.

## Always-on browser-only crypto (unchanged crypto v1)

USB role is `1`, BLE role is `2`. USB sends KEY_OFFER after connect; BLE sends
KEY_REPLY after validating it. Public keys are 65-byte uncompressed P-256 points
`0x04 || X32 || Y32`. At `seq=0`, key fragments contain
`version:u8 || role:u8 || totalLen:u16be || keyBytes[0..55]`; subsequent fragments
contain at most 59 bytes. KEY_OFFER/REPLY/CONFIRM retain **three-byte ACKs**
`ackedType:u8 || ackedSeq:u16be`. Identical key retries are idempotent; conflicts
abort. Handshake ACK timeout is 1000 ms, with up to three attempts; timeout
KEY_ABORT reason is 3, conflict is 4, SAS mismatch is 2.

The transcript is `ascii("PocketAirBridge-crypto-v1") || 0x01 || usbPubRaw ||
0x02 || blePubRaw`. SAS is the first 20 bits of
`SHA-256(ascii("PocketAirBridge SAS v1") || transcript)` modulo 1000000, displayed
as six digits. KEY_CONFIRM is `version:u8 || status:u8 || transcriptHash16`,
where status 1 means accepted (2 is reserved, does not unlock) and
`transcriptHash16` is the first 16 bytes of SHA-256(transcript). Local acceptance
and matching peer confirmation are both required. Mismatch clears keys.

ECDH derives 256 shared bits. HKDF-SHA256 uses salt
`SHA-256(ascii("PocketAirBridge salt v1") || transcript)` and these info strings:

| Output | HKDF info |
|---|---|
| USB→BLE AES-GCM-256 key | `PocketAirBridge v1 USB->BLE item key` |
| BLE→USB AES-GCM-256 key | `PocketAirBridge v1 BLE->USB item key` |
| 16-byte key ID, canonical unpadded base64url | `PocketAirBridge v1 key id` |
| USB→BLE 4-byte nonce prefix | `PocketAirBridge v1 usb-to-ble nonce prefix` |
| BLE→USB 4-byte nonce prefix | `PocketAirBridge v1 ble-to-usb nonce prefix` |

SAS authenticates browser peers, not Flipper hardware identity. Browser device
permission and first-pairing BLE numeric comparison are separate consent gates.
Stored BLE bonds permit subsequent reconnects. The Flipper never possesses
plaintext or session keys; endpoint compromise remains outside this protection.

## Outer META: transport-only JSON

`encodeMeta` fragments UTF-8 JSON with a two-byte total-fragment count in every
frame and zero-based header `seq`. Count/order and exact duplicates are checked.
After reassembly, **only these ten fields** are permitted:

| Field | Required value/validation |
|---|---|
| `kind` | `"encrypted-stream"` |
| `protocolVersion` | 2 |
| `cryptoVersion` | 1 |
| `keyId` | Current key ID: 16 decoded bytes, canonical 22-character base64url |
| `direction` | `"usb-to-ble"` or `"ble-to-usb"`, matching peer role/key |
| `itemId` | HELLO's uint32 ID |
| `totalCiphertextBytes` | Canonical unsigned decimal uint64 **string** |
| `maxSegmentPlaintextBytes` | 65536 |
| `maxSegmentCiphertextBytes` | 65552 |
| `segmentCount` | Positive uint32 |

Parse totals with BigInt and `^(0|[1-9][0-9]*)$`. Reject JSON numbers, signs,
leading zeros, exponent notation, overflow, extra/missing fields, context
mismatch and inconsistent totals before DATA. Final ciphertext length must be
17..65552 bytes (at least 66 for a single segment's mandatory header).
Outer META reveals transport sizes, direction, key ID and item ID, but not
private kind/name/MIME/payload size/hash.

## Private stream header and segment math

Segment zero starts with this authenticated binary header, followed by payload:

| Field | Bytes/encoding |
|---|---|
| magic | 4 ASCII `AB2S` |
| version | u8 = 2 |
| kind | u8: 1=text, 2=attachment |
| nameLen, mimeLen | u16be each, UTF-8 byte lengths |
| payloadSize | u64be |
| payloadSha256 | 32 raw digest bytes |
| name, MIME | nameLen then mimeLen UTF-8 bytes, fatal decoding |

`headerLen = 50 + nameLen + mimeLen`, with `headerLen <= 65536`. The whole
header fits segment zero. Parser/input and DATA-slice splits are supported;
metadata spanning AEAD segments is not.

```
streamPlaintextBytes = headerLen + payloadSize
segmentCount = max(1, ceil(streamPlaintextBytes / 65536))
totalCiphertextBytes = streamPlaintextBytes + 16 * segmentCount
finalCiphertextLen = totalCiphertextBytes - (segmentCount - 1) * 65552
```

Every non-final plaintext segment is 65,536 bytes; AES-GCM adds a 16-byte tag,
giving 65,552 bytes. This envelope occupies **1,286 DATA frames**: 1,285 full
51-byte slices and one 17-byte slice, indices `0..1285`. It is not 1,286 *full*
slices. The next segment resets header `seq` to zero and increments segmentIndex.
Keep uint64 sizes, counters, equality, progress and arithmetic in BigInt. Number
conversion at a browser API boundary requires `<= Number.MAX_SAFE_INTEGER` first.

### Per-segment IV/AAD and nonce domains

Each segment uses its existing direction's AES-GCM-256 key and a 128-bit tag:

```
IV = noncePrefix(direction)[4] || itemId:u32be || segmentIndex:u32be
AAD = ascii("AB2-AAD") || cryptoVersion:u8 || keyId[16 decoded bytes]
      || directionByte:u8 || itemId:u32be || segmentIndex:u32be
      || segmentPlaintextLen:u32be || finalFlag:u8
```

Direction byte 1 is USB→BLE and 2 is BLE→USB; finalFlag is 1 only for the last
segment, otherwise 0. AAD contains only pre-decryption values. Private size/hash
are authenticated as segment-zero plaintext, **not** included in AAD.
Build the IV suffix with explicit uint32 byte writes or an eight-byte BigInt
write; JavaScript `itemId << 32` is forbidden (shift count wraps modulo 32).

Uniqueness is per session/direction key, then monotonic itemId, then unique
segmentIndex. Equal IDs in opposite directions are safe under distinct keys and
prefixes. Never reuse the v1 whole-item IV/counter path for segments or mix v1
and v2 nonce domains within a session. Item ID, segment index or segment-count
overflow fails rather than wraps; ID exhaustion clears the crypto session.

## Sender, receiver, retries and lifecycle

Sender states: Hashing → HELLO/capability → META → Sending DATA → DONE/Verifying
→ Ready, or terminal Failed/Cancelled. Attachments use two independent
`File.stream()` acquisitions, never `tee()` or whole-file materialization.
Pass 1 hashes incrementally and counts bytes. Only after success, immediately
before HELLO, `AirBridgeCryptoSession` allocates a strictly monotonic uint32 ID.
Pass 2 rebuilds the header and encrypts segments; byte count must match pass 1
and File.size. The authoritative digest is pass 1, not a digest collected while
sending. An allocated ID is burned on success, cancel, error, timeout or retry.
Only fresh ECDH keys reset counters. Text remains memory-backed.

Receiver states: idle → hello → meta → data → done → terminal. Monotonic IDs,
item/segment/seq, exact lengths and order are checked. Authenticate each complete
ciphertext segment before releasing any of its plaintext to the private receive
accumulator. Retain at most one 65,552-byte ciphertext segment plus a 51-byte
retry slice. Sender retains one plaintext segment, one ciphertext segment and
one exact outstanding frame, plus bounded source/residual storage. These are
protocol working-set bounds, not total JS/native/browser heap bounds.

The accumulator deliberately retains **O(file size) authenticated plaintext**
chunks, incrementally hashes payload only, then verifies all segments, header/
stream/payload counts and final SHA-256 at DONE. Only after verification create
`new Blob(chunks, {type:mime})` without a giant concatenated Uint8Array, deliver
the verified item callback, and ACK DONE. No completed card, download or URL may
exist before successful verification. A completed card owns its Blob/object URL.
Removal, replacement, invalidation and page disposal revoke that URL exactly
once; remove/invalidate the card atomically so no live link points at a revoked
URL. Releasing references is not guaranteed secure memory erasure.

One item and one outstanding frame at a time. Collision ordering is lexicographic
`(itemId, role)`; USB role 1 wins equal-ID ties. No interleaving. ACK/NACK matches
type, seq, magic, itemId and segment. Controls use the sentinel; DATA uses its
real segment. NACK reasons remain 1=missing, 2=malformed,
3=auth-failed-retryable, 4=busy-window; these codes do not authorize retrying an
AEAD failure. Retry only the exact current frame, not byte offsets or resume.
`ItemSender` defaults are 2000 ms and 3 retransmissions; chat's
`createChatOutbound` selects 5000 ms and 4 retransmissions. A still-pending write
times out instead of starting a concurrent retry. Exhaustion terminates the
operation; the outbound owner performs one best-effort item-bound CANCEL.

An exact currently retryable duplicate is re-ACKed without second append/hash.
An authentication-pending duplicate waits for that authentication; a conflicting
duplicate is terminal, invalidating the original operation too. There is no
unbounded frame/replay cache. Ignore identifiable stale item-scoped ERROR/BUSY;
stale item traffic never enters the active item. Current malformed framing,
AEAD failure, final size/hash mismatch and conflicting/order errors are terminal,
not an authentication NACK loop. Failure produces no verified Blob/DONE ACK.

Cancellation before ID/HELLO emits no frame. After HELLO, cancel is one
best-effort item-bound CANCEL. Receiver timeout, cancel, ERROR, disconnect and
crypto reset invalidate generation ownership and clear ciphertext, accumulated
plaintext and pending work. Check ownership around reads, crypto awaits, writes,
retry waits and callbacks; late item-N work cannot mutate item N+1. Ordinary
cancel/item faults permit a fresh higher-ID item in the still-confirmed session;
incompatibility requires a new confirmed session. No byte-range/cross-session
resume, whole-item sender/ciphertext buffering, disk/OPFS/File System Access
receive, save picker or direct-to-disk path is implemented.

## Practical limits

V2 removes the old AirBridge **4 MiB/4096-chunk demo ceiling**; it does not mean
infinity. uint32 IDs and segment counts, uint64 sizes, browser safe-integer API
boundaries, browser heap, Blob/download/storage capacity, BLE/USB throughput and
transfer time remain finite constraints. uint32 segment count bounds stream
plaintext to roughly 256 TiB even before browser limits. In-memory authenticated
receive normally reaches heap limits much earlier. Historical hardware throughput
was about 0.5–2 KB/s, not a promise for v2 or all hosts. Chat data is not compressed.

## Dual-transport bootstrap (separate, unchanged protocol)

Deploy is explicit on-device consent: LEFT/RIGHT selects the USB or BLE prompt, OK
confirms cursor placement, `TYPING…` stays visible during typing, BACK aborts.
Bridge data paths never emit keyboard reports. The selected deploy transport routes
each bootstrap to its matching bundle:

| Deploy control | Typed bootstrap | Bundle | Delivery channel |
|---|---|---|---|
| **LEFT/RIGHT → USB Deploy, then OK** | `bootstrap.js` | `app-usb.html.gz` | USB vendor HID |
| **LEFT/RIGHT → BLE Deploy, then OK** | `bootstrap-ble.js` | `app-ble.html.gz` | AirBridge serial TX notify |

The canonical builder owns
`airbridge/dist/app-usb.html` and `app-ble.html`, their `.gz` SD containers and the generated digest
header. Bundle regeneration/freshness is a separate integration gate, not implied
by source-level v2 tests. No per-chunk ACK is used for either bootstrap stream; USB
interrupt IN reports are hardware-reliable and BLE retries transient notification
congestion.

The FAP authenticates normalized printable ASCII bootstrap bytes and the complete
SD bundle container against pinned SHA-256 before execution/delivery. Container:
`ABND[4] || version:u8=1 || reservedZero[3] || htmlSize:u32le || gzipBytes`.
It validates magic/version/reserved/size/gzip magic and digest, strips 12 bytes,
and streams only gzip. Request is byte `0x42` in a 64-byte vendor HID report (USB) or a BLE RX write,
accepted only in post-consent Deploy waiting state. In Bridge it is ordinary
relay data. In other Deploy states the FAP shows `DEPLOY NOT ARMED`.

The first response carries `totalLen:u32le || additiveChecksum:u32le`, padded to
64 bytes on USB and sent as the eight header bytes directly on BLE TX notify;
subsequent reports contain sequential gzip bytes, zero-padded to 64 bytes on USB
and sent as raw notification payloads on BLE, without
chat ACKs. Bootstrap accumulates totalLen, checks the additive uint32 sum,
inflates with `DecompressionStream("gzip")` and boots HTML. This additive checksum
is corruption detection, not the SD trust mechanism. Failure copy remains
`Transfer unsupported - retry`, `Transfer corrupt - retry` or
`Transfer timed out - retry`; Connect re-arms. Bootstrap behavior is not changed
by chat protocol v2.

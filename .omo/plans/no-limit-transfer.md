# no-limit-transfer - Work Plan

## TL;DR (For humans)
**What you'll get:** Pocket AirBridge will support large attachments without an AirBridge protocol-imposed file-size ceiling while preserving end-to-end encryption. The browser endpoints will stream authenticated encrypted segments, verify plaintext integrity incrementally, and fail cleanly when the peer does not speak the new protocol.

**Why this approach:** The Flipper is already the right shape: a stateless encrypted byte pipe. The broken parts are the browser protocol/UI: one global 16-bit chunk counter, full-file plaintext reads, one whole-item AES-GCM operation, full ciphertext buffering, and full plaintext reassembly before decryption. The fix is encrypted segmented streaming plus bounded sender/receiver storage.

**What it will NOT do:** It will not claim literal infinite transfer size, remove authentication or SHA-256 verification, require a server/install/network, expose plaintext to the Flipper, or add legacy mixed-version compatibility. Old or plaintext peers fail explicitly.

**Effort:** XL
**Risk:** High - protocol semantics, AEAD streaming safety, browser storage permissions, bundled deploy output, and physical hardware flows all interact.
**Decisions to sanity-check:** Preserve always-on SAS/ECDH/AES-GCM encryption; replace whole-item encryption with authenticated per-segment encryption; use File System Access for large receive saves; vendor a small offline incremental SHA-256 implementation; old/new protocol mismatch fails instead of interoperates.

Your next move: start work from this plan, or request another high-accuracy review first. Full execution detail follows below.

---

> TL;DR (machine): XL/high-risk browser encrypted protocol v2 and UI streaming change; no Flipper firmware change unless verification proves byte-pipe framing is broken.

## Scope
### Must have
- Replace the current demo-only `4096 chunks` / `4 MiB` receiver rejection with encrypted protocol v2 semantics that impose no AirBridge item-size ceiling.
- Preserve the 64-byte transport frame and 59-byte payload constraint.
- Preserve ACK-per-frame backpressure; no multi-outstanding windows.
- Preserve the stateless Flipper bridge; browser endpoints own protocol and crypto state; the Flipper never sees plaintext or session keys.
- Preserve always-on P-256 ECDH + HKDF + AES-GCM + SAS confirmation; do not allow plaintext item transfer after crypto unlock requirements are met.
- Replace whole-item AES-GCM encryption/decryption for large attachments with authenticated segmented encryption so no peer must hold the full attachment as plaintext or ciphertext.
- Stream selected attachment files from the sender instead of reading the whole file into memory before transfer.
- Stream received authenticated attachment plaintext into a bounded sink instead of keeping every transport chunk or decrypted item in memory.
- Use incremental SHA-256 for plaintext integrity verification during segmented streaming.
- Use File System Access API for received attachments larger than 1 MiB; small-file fallback may stay in memory.
- Make unsupported old/new/plaintext peer combinations fail explicitly with ERROR, not silent corruption or partial interop.
- Keep text-message behavior working and memory-backed; segmented encrypted streaming primarily targets attachments.
- Update the protocol harness, mock chat pages, bundles, README, and docs.
### Must NOT have (guardrails, anti-slop, scope boundaries)
- Do not modify Flipper firmware unless browser-side QA proves the stateless byte pipe cannot forward the new frames.
- Do not change USB/BLE report size, require BLE MTU changes, or add transport fragmentation below the existing 64-byte frame.
- Do not add any server, cloud, network fetch, native app, browser extension, package manager runtime dependency, or install step.
- Do not remove or weaken SAS confirmation, AEAD authentication, replay protection, or end-to-end SHA-256 plaintext integrity verification.
- Do not create a large-file path that depends on holding the entire attachment as one `Uint8Array`, `Map`, `Blob`, full ciphertext, or full plaintext before verification.
- Do not write unauthenticated plaintext to the receive sink.
- Do not preserve v1/v2 compatibility beyond explicit graceful failure.
- Do not leave README/docs claiming unlimited/infinite size without practical browser/disk/time constraints.

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: TDD / characterization-first in `web/protocol-harness.html`, followed by implementation and browser mock QA.
- Evidence: <attemptDir>/task-<N>-no-limit-transfer.md plus screenshots/log extracts where browser QA is used. If outside ulw-loop, use `.omo/evidence/no-limit-transfer/`.
- Agent-executed browser verification must mock `showSaveFilePicker` and fake writable streams for deterministic File System Access tests. Real browser permission prompts and hardware flows are optional gated QA and must use the project `question` flow; they are not required for browser-mock completion.
- Large-file threshold: attachment size `> 1 * 1024 * 1024` bytes must use the File System Access path when available. If File System Access is unavailable or denied for a large receive, fail explicitly with user-visible ERROR/CANCEL cleanup. Do not silently fall back to a giant in-memory Blob for large files.
- Final status vocabulary: `PASS_BROWSER_MOCK` means all protocol/bundle/mock-browser checks passed; `PASS_HARDWARE` means physical AirBridge QA also ran and passed; `HARDWARE_PENDING_NOT_RUN` is allowed only as an explicit evidence status and must never be worded as hardware pass.
- Required automated commands after relevant todos:
  - `python3 tools/build_bundle.py`
  - `python3 -m http.server 8080 --directory web` for browser/manual QA when required
  - `window.runAllProtocolTests()` in `http://localhost:8080/protocol-harness.html`
- Required browser mock checks before hardware:
  - `http://localhost:8080/chat-usb.html?mock=1`
  - `http://localhost:8080/chat-ble.html?mock=1`
- Hardware QA is final-wave only unless implementation unexpectedly touches firmware or transport adapters.

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.
- Wave 1: encrypted protocol contract/tests, incremental hasher, receive sink abstraction can run in parallel.
- Wave 2: segmented AEAD implementation first (todo 4), then streaming sender (5), then streaming receiver (6); UI (7) follows.
- Wave 3: UI progress/finalization, explicit failure behavior, docs/bundles depend on Wave 2.
- Wave 4: full mock/browser/hardware-oriented regression and final documentation cleanup.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 4,5,6,8,9 | 2,3 |
| 2 | none | 4,5,6,7,9 | 1,3 |
| 3 | none | 5,6,7,8,9 | 1,2 |
| 4 | 1,2 | 5,6,7,8,9 | none |
| 5 | 1,2,3,4 | 6,7,8,9 | none |
| 6 | 1,2,3,4,5 | 7,8,9 | none |
| 7 | 3,5,6 | 8,9,10 | none |
| 8 | 5,6,7 | 10 | none |
| 9 | 5,6,7 | 10 | none |
| 10 | 8,9 | final wave | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [ ] 1. Define encrypted protocol v2 segmented streaming contract and lock tests first
  What to do / Must NOT do: Update protocol constants/helpers and `web/protocol-harness.html` characterization tests for encrypted v2 metadata, segmented stop-and-wait `ITEM_DATA` semantics, segment rollover beyond a single uint16 chunk range, explicit ERROR on unsupported peer/protocol/crypto mismatch, and removal of demo `4096`/`4 MiB` as protocol ceilings. Exact contract to implement (this paragraph is the normative wire contract; every number is pinned):

  **Transport framing.** `HELLO` payload remains the existing 4-byte itemId, but v2 changes how itemIds are generated (see "Item ID discipline" below). `ITEM_DATA.seq` remains uint16 and is the frame index within the current ciphertext segment. Each v2 `ITEM_DATA` payload is `[segment:uint32be][chunkInSegment:uint16be][ciphertext slice]`, so the effective data bytes per frame drop from 59 to 53 (`V2_DATA_SLICE_LEN = MAX_PAYLOAD - 6 = 53`); all v2 chunk counts and ciphertext sizes are computed against 53, never against `MAX_PAYLOAD`. The 6-byte prefix is transport-only and is never encrypted, hashed into ciphertext hashes, or counted in segment ciphertext lengths. Segment boundaries are byte-derived: when a segment's derived ciphertext byte count is consumed, the next frame is `segment+1, chunkInSegment 0`. Since a uniform segment spans 1237 frames, `chunkInSegment` never approaches the uint16 maximum — per-segment indexing (not a global uint16 counter) is precisely what removes the v1 chunk-count ceiling.

  **Item ID discipline (crypto-critical).** IV uniqueness depends on itemId uniqueness, so v2 makes item IDs strictly monotonic per session per direction: each endpoint keeps a per-crypto-session uint32 counter starting at 0 and increments it per item; an itemId MUST NOT repeat under the same session key. The existing generators are non-conformant and must be replaced: `chat-usb.html:243` wraps at 16 bits and `chat-ble.html:844` uses `Date.now() + Math.random()`. When the counter would wrap past 0xffffffff, the endpoint aborts with ERROR and requires a fresh crypto session (re-handshake) — at ~2^-32 of practical usage this is unreachable in a demo but is the normative rule. The harness must include wrap/collision tests: reused itemId under one session key is rejected; wrap forces session abort.

  **v2 ACK/NACK segment context.** Because seq is reused across segments, v2 extends the ACK payload for `ITEM_DATA` frames to `[acked_type:1][acked_seq:2be][segment:uint32be][chunkInSegment:uint16be]` (9 bytes) and the NACK payload to `[nacked_type:1][nacked_seq:2be][segment:uint32be][chunkInSegment:uint16be][reason:1]` (10 bytes); both are still within the 59-byte limit. A v2 sender matches a DATA ACK/NACK on all context fields (plus reason parsing for NACK); a v2 receiver always emits the extended forms for ITEM_DATA. ACK payloads for HELLO/DONE/CANCEL/KEY frames keep the existing 3-byte form; ITEM_META is the sole non-DATA exception (capability marker, next paragraph). This eliminates the stale-ACK ambiguity where a delayed segment-N ACK could falsely satisfy segment-M's wait for the same seq. Duplicate retransmissions (same segment, same chunkInSegment, same ciphertext bytes — caused by a lost ACK) are idempotently re-ACKed without re-buffering; a frame matching segment/seq but with different bytes is rejected as a conflict.

  **v2 capability proof.** A v2 receiver MUST include a capability marker in its META ACK so an old v1 peer cannot be mistaken for a v2 peer: v2 ACKs for `ITEM_META` also use the extended form `[acked_type:1][acked_seq:2be][capabilities:4 ASCII "AB2S"]`. Current v1 receivers ACK META with the plain 3-byte form, so a v2 sender talking to an old receiver fails fast after the first META ACK (ERROR `Unsupported protocol version`) before any ITEM_DATA is sent. A v2 receiver rejects v1/missing-`protocolVersion`/non-stream metadata with the same ERROR before accepting any ITEM_DATA. Both directions are fail-closed before data transfer.

  **Outer META (cleartext, transport-only).** `ITEM_META` JSON contains ONLY transport fields: `kind:"encrypted-stream"`, `protocolVersion:2`, `cryptoVersion:1`, `keyId`, `direction`, `itemId`, `totalCiphertextBytes`, `segmentCiphertextBytes:65552` (single uniform value; mandatory), and `segmentCount`. Plaintext kind/name/mime/size/hash MUST NOT appear in outer META — this preserves the current v1 privacy property where the Flipper and observers see only encrypted transport metadata. Mismatch handling is the fail-closed behavior described above.

  **Encrypted stream header.** The first decrypted bytes of the plaintext stream are an authenticated binary stream header: `magic "AB2S"`, `version:uint8 = 2`, `kind:uint8` (1=text, 2=attachment), `nameLen:uint16be`, `mimeLen:uint16be`, `plaintextSize:uint64be`, `plaintextSha256:32 bytes`, `name bytes`, `mime bytes`, then the plaintext payload. Because this header is inside the AEAD-protected stream, plaintext metadata privacy matches current v1 behavior.

  **Two-pass sender.** The plaintext SHA-256 must be known before the stream header is sent, so the sender performs two streaming passes over `File.stream()`: pass 1 computes the incremental hash and byte count; pass 2 encrypts and sends. Both passes are memory-bounded. This is the normative resolution of hash placement; no trailing hash field is added and `ITEM_DONE` payload stays empty.

  **Segment sizing and boundaries.** `segmentPlaintextBytes` is MANDATORY in the encrypted stream negotiation: v2 uses a uniform plaintext segment size of 65536 bytes (64 KiB) except the final segment, which is the remainder derived from `plaintextSize`. Receiver derives each segment's ciphertext length as `segmentPlaintext + 16` (AEAD tag) from META + header fields alone, so it always knows when a segment is complete and can authenticate before releasing plaintext. Per-segment unauthenticated ciphertext buffered at the receiver is therefore bounded at 65552 bytes plus one in-flight transport frame.

  **Per-segment AEAD.** Each plaintext segment (stream header bytes for segment 0, then payload) is encrypted with AES-GCM-256 using the existing direction key. The IV is `noncePrefix(direction)` (4 bytes) followed by an 8-byte big-endian concatenation of `itemId:uint32be || segmentIndex:uint32be` — a 12-byte GCM IV computable by BOTH sides from fields already authenticated on the wire, so the receiver needs no counter mirroring and a mid-item CANCEL/ERROR cannot desync IVs. IMPLEMENTATION HAZARD (normative): the 8-byte suffix MUST be built with explicit byte writes or `DataView#setBigUint64` on `BigInt(itemId) << 32n | BigInt(segmentIndex)` — JavaScript's `itemId << 32` is a silent no-op (shift count mod 32) and would produce catastrophic cross-item IV collisions. Uniqueness argument: noncePrefix is unique per (session, direction); itemId is unique per item within a session direction (enforced by the Item ID discipline above); segmentIndex is unique per segment within an item — therefore no IV repeats under a direction key. Both `itemId` and `segmentIndex`/`segmentCount` are hard-bounded at uint32: exceeding either aborts the item/session with ERROR rather than wrapping. The existing per-item `makeIv` pattern and the global outbound counter MUST NOT be reused for segments. The harness must include tests asserting: distinct IVs across all segments of a multi-segment item and across items; `itemId=1, segmentIndex=0` produces a different IV than `itemId=0, segmentIndex=1` (guards the JS shift hazard); and segmentIndex/segmentCount overflow aborts. AAD for every segment contains ONLY values known to both sides before decrypting that segment: `ascii("AB2-AAD") || cryptoVersion || keyId || directionByte || itemId:uint32be || segmentIndex:uint32be || segmentPlaintextLen:uint32be || finalFlag:uint8`. The stream-header fields (`plaintextSize`, `plaintextSha256`) are NOT in AAD — they are authenticated implicitly because they are segment-0 plaintext covered by that segment's GCM tag. Anti-replay tracking key is `{keyId, direction, itemId, segmentIndex}`; exact-duplicate retransmissions are idempotently re-ACKed (see above); conflicting duplicates and out-of-order segments are rejected. Receiver releases plaintext to the sink only after the segment's GCM tag verifies.

  **Completion.** `ITEM_DONE` is ACKed only after all segments authenticated, total plaintext byte count matches the stream header, and the incremental plaintext SHA-256 matches the stream header hash.

  Do not implement sender/receiver UI yet. Do not change frame size or Flipper assumptions. Do not use vague "windowed" or multi-outstanding ACK behavior.
  Parallelization: Wave 1 | Blocked by: none | Blocks: 4,5,6,8,9
  References (executor has NO interview context - be exhaustive): `web/airbridge-protocol.js:17-32,167-175,177-180,487-830,1289-1371`; `web/chat-usb.html:493-531,640-675`; `web/chat-ble.html:630-636,694-721,741`; `docs/protocol.md:28-53,55-83,124-160`; `web/protocol-harness.html:77,109-119,261,822-1180`
  Acceptance criteria (agent-executable): New harness tests fail before implementation and pass after only protocol helpers are updated; tests cover >4096 transport chunks computed against 53-byte data slices, multi-segment rollover (`chunkInSegment` resets 1236→0 at the byte-derived segment boundary across ≥3 segments), extended v2 DATA ACK matching on all context fields (stale segment-N ACK must NOT satisfy segment-M wait), extended v2 DATA NACK (10 bytes incl. reason byte) parsed correctly, v2 META capability marker (`AB2S`) required and absent from v1 peers (v2 sender treats a plain 3-byte META ACK as explicit `Unsupported protocol version`, not timeout/retry exhaustion), v1/missing-version ERROR reason `Unsupported protocol version` in BOTH directions before ITEM_DATA, per-session monotonic itemId enforcement (reused itemId under one session key rejected; uint32 wrap forces session abort/re-handshake), exact-duplicate DATA retransmission idempotently re-ACKed without double-releasing plaintext, conflicting duplicate rejected, plaintext-after-unlock rejection, outer META carrying no plaintext kind/name/mime/size/hash fields (privacy regression test mirroring the existing v1 no-leak test at `web/protocol-harness.html:1109-1110`), stream-header roundtrip, mandatory uniform `segmentCiphertextBytes:65552`, receiver-derived segment boundaries, distinct per-segment IVs computable by both sides, hash mismatch ERROR, cancel state reset, cancel-mid-item followed by a fully successful subsequent item (IV desync regression test), replay rejection, and unambiguous ACK behavior under stop-and-wait seq reuse.
  QA scenarios (name the exact tool + invocation): happy: serve `web` and run `window.runAllProtocolTests()` in `protocol-harness.html`, evidence `<attemptDir>/task-1-no-limit-transfer.md`; failure: inject mismatched protocol version metadata and assert sender surfaces peer ERROR, evidence same file.
  Commit: Y | test(protocol): characterize encrypted segmented streaming v2

- [ ] 2. Add offline incremental SHA-256 implementation and tests
  What to do / Must NOT do: Add a small vendored browser-compatible incremental SHA-256 module under `web/` and wire tests comparing incremental output against `crypto.subtle.digest()` for known fixtures and chunk boundaries. Record source/provenance, license, version/commit if applicable, retained attribution header, and confirm license compatibility with this MIT repo in the evidence. Keep it self-contained for offline deploy bundles and include it in `tools/build_bundle.py`. Do not fetch code at runtime. Do not replace small full-buffer hash helper unless tests prove equivalence.
  Parallelization: Wave 1 | Blocked by: none | Blocks: 4,5,6,7,9
  References (executor has NO interview context - be exhaustive): `web/airbridge-protocol.js:235-239,241-244,787,818,1356-1360`; `tools/build_bundle.py:15-31,46-59,81-138`; `docs/protocol.md:150-155`; browser research: WebCrypto digest is not incremental, Streams/File System Access/OPFS are stable Chromium primitives.
  Acceptance criteria (agent-executable): Harness includes incremental hash tests for empty input, small text, binary data, non-uniform chunk boundaries, and a multi-megabyte generated stream. Add instrumentation using a generated chunk source/fake stream that counts emitted bytes and throws if a consumer requests full materialization; evidence records max buffered bytes or equivalent counter for the incremental path.
  QA scenarios (name the exact tool + invocation): happy: `window.runAllProtocolTests()` shows incremental hash vectors pass, evidence `<attemptDir>/task-2-no-limit-transfer.md`; failure: intentionally corrupt one plaintext chunk in a test stream and assert final hash mismatch is detected, evidence same file.
  Commit: Y | feat(protocol): add incremental sha256 for streaming transfers

- [ ] 3. Add receive sink abstraction for small memory and File System Access saves
  What to do / Must NOT do: Create a browser-side receive sink abstraction that supports `write(chunk)`, `close()`, `abort()`, byte counts, and final materialization/download metadata. Use File System Access for attachments larger than 1 MiB when available; provide safe small-file memory fallback only for `<= 1 MiB`. OPFS is out of implementation scope except as a fake/test sink if useful; do not build an OPFS product path in this plan. Handle save picker cancellation, `showSaveFilePicker` unavailable, writable stream write failure, and close failure explicitly with clear ERROR/CANCEL behavior and cleanup. Do not require a server or native install. Do not create giant Blob URLs for large transfers. Do not accept unauthenticated plaintext.
  Parallelization: Wave 1 | Blocked by: none | Blocks: 5,6,7,8,9
  References (executor has NO interview context - be exhaustive): `web/airbridge-ui.js:103-149,153-163`; `web/chat-usb.html:493-531`; `web/chat-ble.html:630-636`; `docs/architecture.md:64-72,110-121`; browser research: File System Access `createWritable/write/close`, OPFS, Blob URL revoke guidance.
  Acceptance criteria (agent-executable): Sink tests or harness scenarios prove small receive returns data for existing attachment rendering, large receive writes only authenticated plaintext incrementally without retaining all chunks, abort cleans partial state, and unsupported storage returns a clear user-facing error.
  QA scenarios (name the exact tool + invocation): happy: protocol harness simulates a large generated attachment through sink and verifies byte count/hash, evidence `<attemptDir>/task-3-no-limit-transfer.md`; failure: simulated sink write failure triggers ERROR/CANCEL cleanup and no completed download link, evidence same file.
  Commit: Y | feat(web): add streaming receive sinks

- [ ] 4. Implement segmented AEAD item crypto and tests
  What to do / Must NOT do: Extend the existing `AirBridgeCryptoSession` with protocol v2 segmented AEAD item encryption/decryption APIs exactly as contracted in todo 1. Keep the existing ECDH/SAS/session key derivation unchanged (crypto version stays 1). Implement: two-pass sender (streaming incremental SHA-256 pass over `File.stream()` to build the stream header, then a second streaming pass to encrypt/send); per-segment IV = 4-byte `noncePrefix(direction)` followed by explicit 8-byte big-endian `itemId:uint32be || segmentIndex:uint32be` (built via explicit byte writes or `DataView#setBigUint64` with BigInt — never JS `<< 32`, which is a no-op) so both sides compute it independently and cancel/error can never desync IV state; the exact AAD layout from todo 1 (only pre-decryption-known fields); mandatory 64 KiB uniform plaintext segments; receiver-side per-segment authentication before any plaintext is released; extended v2 ACK/NACK payloads with segment context; the `AB2S` META capability marker; idempotent re-ACK of exact-duplicate retransmissions; anti-replay tracking by `{keyId, direction, itemId, segmentIndex}`. Replace the non-conformant itemId generators in both chat pages (`chat-usb.html:243` 16-bit wrap; `chat-ble.html:844` `Date.now()+Math.random()`) with the per-session monotonic uint32 counter from todo 1, including wrap-abort/re-handshake. Sender MUST NOT compute the hash "while sending" — it is computed in pass 1 before HELLO/META. Do not weaken nonce uniqueness or replay checks. Do not decrypt or emit unauthenticated plaintext.
  Parallelization: Wave 2 | Blocked by: 1,2 | Blocks: 5,6,7,8,9
  References (executor has NO interview context - be exhaustive): `web/airbridge-protocol.js:311-330,343-368,371-383,487-830`; `web/protocol-harness.html:109-119,822-1180`; `web/chat-usb.html:640-664`; `web/chat-ble.html:694-721`
  Acceptance criteria (agent-executable): Harness covers segmented encrypt/decrypt roundtrip, tampered segment tag failure, segment reordering failure, duplicate/replayed segment failure, wrong key id/session failure, cancel/abort clears crypto state, and no full-file plaintext/ciphertext materialization in the large-file API contract. Additional required tests: (a) assert distinct IVs across every segment of a multi-segment item by instrumenting the IV derivation; (b) assert receiver buffers never exceed 65552 bytes + one 53-byte frame of unauthenticated ciphertext per item; (c) E2E mock test with instrumented File streams that throw if any consumer requests full materialization (e.g., a File subclass or stream spy) on the sender, and a bound assertion on ItemReceiver storage (`receiver.chunks.size` never exceeds the in-flight window) on the receiver.
  QA scenarios (name the exact tool + invocation): happy: `window.runAllProtocolTests()` segmented AEAD tests pass, evidence `<attemptDir>/task-4-no-limit-transfer.md`; failure: mutate one ciphertext byte in any non-final or final segment and assert ERROR/AEAD failure before plaintext reaches the sink, evidence same file.
  Commit: Y | feat(crypto): add segmented authenticated item encryption

- [ ] 5. Implement streaming encrypted sender paths in both chat pages
  What to do / Must NOT do: Replace attachment `FileReader.readAsArrayBuffer(file)` send flow in `chat-usb.html` and `chat-ble.html` with the two-pass streaming flow from todo 1: pass 1 streams the file through incremental SHA-256 to build the encrypted stream header; pass 2 re-streams the file through segmented AEAD encryption and sends protocol v2 frames. Keep text sends working and encrypted. Do not preload the whole attachment into memory on either pass. Do not alter mock transport semantics except as needed for v2 frames. Sender progress must count plaintext bytes sent, not raw `chunks.length` only.
  Parallelization: Wave 2 | Blocked by: 1,2,3,4 | Blocks: 6,7,8,9
  References (executor has NO interview context - be exhaustive): `web/chat-usb.html:181-191,493-531,640-719,720-779`; `web/chat-ble.html:244-250,630-677,678-741,742-840`; `web/airbridge-protocol.js:832-1150`; `docs/protocol.md:86-98,131-138,150-156`
  Acceptance criteria (agent-executable): Mock USB and BLE pages can send text and attachment data using encrypted v2 sender without full-file FileReader path; generated attachment above old `4096 * 59` threshold sends successfully; progress is byte-based; cancel aborts file reads promptly.
  QA scenarios (name the exact tool + invocation): happy: Playwright/Chrome on `chat-usb.html?mock=1` and `chat-ble.html?mock=1` sends a generated attachment larger than old 4096 chunks and completes encrypted, evidence `<attemptDir>/task-5-no-limit-transfer.md`; failure: cancel during active file stream stops send, aborts in-flight item, and resets UI, evidence same file.
  Commit: Y | feat(chat): stream encrypted outbound attachments

- [ ] 6. Implement bounded-memory authenticated streaming receiver and v2 ACK/ERROR handling
  What to do / Must NOT do: Refactor `ItemReceiver` so encrypted v2 attachments validate transport segments, authenticate each AEAD segment, write only authenticated plaintext chunks to the receive sink in order, update incremental plaintext hash, ACK only after bytes are accepted, and finalize on `ITEM_DONE` without `concatChunks(allChunks)` or full decrypt. Keep v1/text behavior only as needed for existing tests or explicit fail behavior. Do not retain all attachment transport chunks, ciphertext, or plaintext in `Map`. Do not ACK `ITEM_DONE` before final hash/AEAD verification.
  Parallelization: Wave 2 | Blocked by: 1,2,3,4,5 | Blocks: 7,8,9
  References (executor has NO interview context - be exhaustive): `web/airbridge-protocol.js:1210-1287,1289-1371,1374-1431`; `web/chat-usb.html:272-282,296-340,493-531`; `web/chat-ble.html:344-357,365-394,630-636`; `docs/protocol.md:100-113,150-160,198-204`
  Acceptance criteria (agent-executable): Protocol harness proves attachment receive over old demo limits uses bounded state, AEAD tamper/hash mismatch sends ERROR and does not ACK DONE, DATA before META remains safe, cancel/error aborts sink and crypto state, and plaintext never reaches UI/sink before authentication.
  QA scenarios (name the exact tool + invocation): happy: `window.runAllProtocolTests()` large encrypted streaming receive passes, evidence `<attemptDir>/task-6-no-limit-transfer.md`; failure: corrupted final encrypted segment yields peer-visible ERROR and no completed item, evidence same file.
  Commit: Y | feat(protocol): receive encrypted attachments as authenticated streams

- [ ] 7. Update chat UI for encrypted streaming progress, save prompts, finalization, and cleanup
  What to do / Must NOT do: Update shared UI and both chat pages so in-progress large encrypted attachments show authenticated plaintext byte progress, pending save/storage state, final success/failure, SAS state, and URL revocation/cleanup. Small attachments may still render as Blob cards after full verification. Large attachments should not require one giant Blob URL. Do not regress text messages, status logs, crypto panel, or cancel buttons.
  Parallelization: Wave 3 | Blocked by: 3,5,6 | Blocks: 8,9,10
  References (executor has NO interview context - be exhaustive): `web/airbridge-ui.js:103-163`; `web/chat-usb.html:154-170,272-282,493-531,600-643`; `web/chat-ble.html:157-172,344-357,630-636,787-840`; `README.md` browser compatibility section
  Acceptance criteria (agent-executable): Add stable test selectors for transfer cards, progress text, cancel button, save status, completion state, crypto state, and error state if they do not already exist. Playwright assertions must check exact text/state: authenticated byte progress reaches total bytes, completed small file exposes a download link, large file reports saved/finalized without creating a giant object URL, cancel removes active progress state, and sink/AEAD failures show specific actionable errors.
  QA scenarios (name the exact tool + invocation): happy: Playwright/Chrome verifies mock large receive progress reaches 100% via selectors and completed card state is correct, evidence `<attemptDir>/task-7-no-limit-transfer.md`; failure: simulated sink unsupported/write/AEAD failure shows expected error text and no stale active progress card, evidence same file.
  Commit: Y | feat(ui): show encrypted streaming attachment lifecycle

- [ ] 8. Enforce explicit no-compat failure and update protocol/security docs
  What to do / Must NOT do: Make v1/v2, plaintext-only, or unsupported-streaming mismatches fail explicitly with ERROR reason `Unsupported protocol version` before data transfer, per user decision: “no compat, just fail.” Update `docs/protocol.md`, `docs/architecture.md`, and README to describe encrypted protocol v2, segmented AEAD transfer, no protocol-imposed item-size ceiling, practical limits, browser storage prompts, unchanged always-on E2E crypto, and unchanged stateless Flipper role. Do not imply old peers interoperate. Do not claim literal infinity. Do not remove or contradict SAS/security model.
  Parallelization: Wave 3 | Blocked by: 5,6,7 | Blocks: 10
  References (executor has NO interview context - be exhaustive): `docs/protocol.md:1-160,227-234`; `docs/architecture.md:34-40,64-90,110-121`; `README.md` protocol, verified hardware, known gaps, browser compatibility sections; `web/airbridge-protocol.js:17-32,487-830`; user decision recorded in `.omo/drafts/no-limit-transfer.md` and conversation: “3. no compat, just fail”.
  Acceptance criteria (agent-executable): Docs and UI error messages consistently state old/new/plaintext peers fail explicitly with `Unsupported protocol version`; protocol harness has a mismatch test; a reviewer READ checklist confirms no docs mention `4096`/`4 MiB` as the active protocol ceiling except in migration/history context.
  QA scenarios (name the exact tool + invocation): happy: READ docs and complete a checklist comparing intended behavior to written docs, evidence `<attemptDir>/task-8-no-limit-transfer.md`; failure: harness simulates legacy/plaintext metadata and asserts ERROR reason `Unsupported protocol version`, evidence same file.
  Commit: Y | docs(protocol): document encrypted v2 streaming and explicit mismatch failure

- [ ] 9. Rebuild bundles and harden regression coverage
  What to do / Must NOT do: Ensure `tools/build_bundle.py` includes any new protocol/UI/hasher modules in `dist/app-usb.html` and `dist/app-ble.html`; add bundle sanity checks if imports are added. Run full protocol harness and mock page flows. Do not leave module imports unresolved in bundled deploy output. Do not expand `web/bootstrap.js` beyond its 1200-character guard enforced by `tools/build_bundle.py:135-137`.
  Parallelization: Wave 3 | Blocked by: 5,6,7 | Blocks: 10
  References (executor has NO interview context - be exhaustive): `tools/build_bundle.py:15-31,46-59,81-138`; `web/bootstrap.js`; `web/bootstrap-ble.js`; `web/protocol-harness.html`; README Deploy app section
  Acceptance criteria (agent-executable): `python3 tools/build_bundle.py` exits 0; bundled apps contain the new required code with no `import`/`type="module"` leftovers; load `dist/app-usb.html` and `dist/app-ble.html` in Chromium and assert no module/import console errors; protocol harness and mock chat pages still pass text, small attachment, large attachment, crypto SAS/tamper/replay/pre-unlock, hash mismatch, cancel, busy/arbitration, and picker-cancel tests.
  QA scenarios (name the exact tool + invocation): happy: run bundle command and browser mock regression including bundled pages, evidence `<attemptDir>/task-9-no-limit-transfer.md`; failure: temporarily add or simulate unresolved import in a bundle fixture/check and verify build/test catches it, evidence same file.
  Commit: Y | build(web): bundle encrypted streaming transfer support

- [ ] 10. Final end-to-end regression and hardware-ready QA record
  What to do / Must NOT do: Run all automated/browser mock verification, collect evidence, and if hardware is available run the documented Pocket AirBridge QA flows: bidirectional text, encrypted attachment transfer with SHA-256, cancel, USB deploy, protocol harness, and Flipper counters. If hardware is unavailable, produce a hardware-ready runbook and mark hardware evidence as `HARDWARE_PENDING_NOT_RUN`, not passed. Do not fabricate physical-device results. Overall browser/mock completion may be `PASS_BROWSER_MOCK` without hardware; only real device evidence may claim `PASS_HARDWARE`.
  Parallelization: Wave 4 | Blocked by: 8,9 | Blocks: final wave
  References (executor has NO interview context - be exhaustive): `AGENTS.md` Test Surface and Gate Discipline; `README.md` Quickstart and Verified on hardware sections; `docs/firmware-guide.md`; `scripts/ble_qa_runbook.md`; `tests/fixtures/README.md`; `web/protocol-harness.html`
  Acceptance criteria (agent-executable): Evidence file records exact commands, browser URLs, results, any hardware gates used, and final status using only `PASS_BROWSER_MOCK`, `PASS_HARDWARE`, or `HARDWARE_PENDING_NOT_RUN`. No final claim says hardware passed unless the agent actually ran it with user-confirmed physical gates.
  QA scenarios (name the exact tool + invocation): happy: protocol harness 100%, mock bidirectional text and large encrypted attachment complete, bundle builds, evidence `<attemptDir>/task-10-no-limit-transfer.md`; failure: hardware unavailable path produces explicit pending hardware checklist and does not block browser-only completion, evidence same file.
  Commit: Y | test(airbridge): verify encrypted streaming transfer regressions

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [ ] F1. Plan compliance audit
  Reviewer: read `.omo/plans/no-limit-transfer.md`, `docs/protocol.md`, `docs/architecture.md`, `README.md`, and changed files. APPROVE only if every todo acceptance criterion is satisfied with evidence and no unchecked implementation todo remains.
- [ ] F2. Code quality review
  Reviewer: inspect all changed JS/HTML/Python/docs for protocol correctness, AEAD nonce/AAD/replay safety, bounded-memory behavior, no unresolved imports, no dead compatibility paths, no TODO placeholders, and clean error/cancel paths. APPROVE only with concrete file:line evidence.
- [ ] F3. Automated browser and optional physical QA
  Reviewer: run `python3 tools/build_bundle.py`, serve `web`, execute `window.runAllProtocolTests()`, and drive mock chat pages in Chromium. If hardware is available, also run Pocket AirBridge physical QA with required user gates; otherwise record `HARDWARE_PENDING_NOT_RUN`. APPROVE is allowed for `PASS_BROWSER_MOCK` plus explicit hardware pending; APPROVE with `PASS_HARDWARE` requires real physical evidence.
- [ ] F4. Scope fidelity
  Reviewer: verify no server/network/native install/firmware/report-size/legacy-compat scope creep was added; verify the implementation matches user decisions: File System Access yes, vendored incremental SHA-256 yes, compatibility no/explicit fail, always-on E2E crypto preserved.

## Commit strategy
- Prefer atomic commits per todo, using the suggested commit line.
- Do not commit unrelated dirty worktree changes.
- If multiple todos land in one unavoidable edit pass, commit only after all covered todos are verified and mention all scopes in the commit body.
- Final wave should not create a commit unless it fixes a discovered issue.

## Success criteria
- `python3 tools/build_bundle.py` exits 0.
- `window.runAllProtocolTests()` passes in `http://localhost:8080/protocol-harness.html`.
- `chat-usb.html?mock=1` and `chat-ble.html?mock=1` can exchange bidirectional encrypted text and attachments after SAS confirmation.
- A generated attachment above the old `4096 * 59` threshold transfers in mock/browser QA without triggering “META chunk count exceeds 4096”.
- Outer v2 ITEM_META carries only encrypted transport fields; plaintext kind/name/mime/size/hash appear only inside the AEAD-protected stream header.
- Receiver does not store all large attachment transport chunks, ciphertext, or plaintext in a long-lived `Map`/array before finalization; unauthenticated ciphertext buffered at the receiver never exceeds one 65552-byte segment plus one in-flight 53-byte frame.
- Sender does not use `FileReader.readAsArrayBuffer(file)` for the large attachment transfer path; both hash and encrypt passes use bounded streaming.
- Large attachment sender and receiver do not call WebCrypto AES-GCM on the entire item.
- Per-segment AEAD authentication happens before plaintext reaches UI or receive sink.
- Hash mismatch, AEAD tamper, replay, cancel, DATA-before-META, busy/arbitration, and picker-cancel regressions still pass.
- Old/new/plaintext protocol mismatch fails explicitly with ERROR.
- Docs state “no AirBridge protocol-imposed file-size limit” and practical constraints; they do not claim literal infinity.
- Flipper firmware remains unchanged unless an evidence-backed todo explains why the byte pipe required a change.

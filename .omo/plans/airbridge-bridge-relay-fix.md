# Sisyphus Plan — Pocket AirBridge Bridge Relay Fix & Truthfulness Pass

## Objective

Make the deployed firmware actually relay bytes in **both** directions (USB HID ↔ BLE Serial), prove it with a real-hardware end-to-end chat + attachment demo, harden the shared JS protocol against the correctness bugs found in review, and bring every document into agreement with the shipped behavior.

**Product story after this plan:** two browser pages exchange text and small attachments through the Flipper Zero bridge on real hardware, with verified SHA-256 integrity in both directions, and docs that tell the truth.

## Background — Why This Plan Exists

A full-codebase critical review (2026-07-19) found that the web-side implementation is solid but the shipped FAP cannot complete a single transfer on real hardware:

1. **Queue truncation** — `pocket_airbridge.c:103` allocates the event queue with `sizeof(uint32_t)` elements, but `usb_event_callback` posts ~72-byte `BridgeEvent` structs (`pocket_airbridge.c:49`). Only the 4-byte `type` field survives; payload/len/direction are silently discarded.
2. **Missing relay handler** — the main loop (`pocket_airbridge.c:121-128`) only handles `EVENT_TYPE_INPUT`. `EVENT_TYPE_RELAY` falls through to a screen redraw. `bt_serial_tx()` is never called anywhere in the FAP.
3. **Consequence** — USB→BLE data and USB→BLE ACKs both die in the Flipper, so no transfer in either direction can complete. This matches the prior session's hardware gate note: "USB→BLE: ACK timeout" and "data not reaching browser" (`.omo/notepads/airbridge-chat-attachments/learnings.md`, gate 7).

### Verified firmware API facts (grounding for the fix)

- `bt_serial_tx(const uint8_t* data, uint16_t len)` exists in the patched `bt.c:30`, returns `bool`, wraps `ble_profile_serial_tx`.
- `ble_svc_serial_update_tx` (`targets/f7/ble_glue/services/serial_service.c:226`) accepts up to `BLE_SVC_SERIAL_DATA_LEN_MAX = 486` bytes and fragments internally at 243-byte characteristic boundaries — a 64-byte frame is a single TX update.
- The raw serial callback fires from `bt_serial_event_callback` ("Called from GAP thread from Serial service", `bt.c:202-212`) — so the current inline BLE→USB path calls `furi_hal_hid_vendor_send_response` from the GAP thread. Both directions should be funneled through the FAP main loop instead.
- RX char accepts `WRITE_WITHOUT_RESP | WRITE` with `ATTR_PERMISSION_AUTHEN_*` — pairing with PIN is required, which *is* the on-device consent mechanism (docs should say this).
- The raw-callback return value is only logged (`serial_service.c:112-113`), but returning 0 is dishonest; return remaining capacity.
- `furi_hal_hid_vendor_send_response(uint8_t* data, uint8_t len)` — note `uint8_t len`, non-const data pointer.
- `HID_VENDOR_PACKET_LEN = 64` (`furi_hal_usb_airbridge.h:7`).
- The FAP has no git history (untracked `applications_user/`); the BT patches are uncommitted local modifications. Do not run the `git checkout` revert from the firmware guide until this plan completes.

## Non-Negotiable Constraints

- Flipper remains a **dumb byte relay**: no protocol parsing, no storage beyond in-flight queue slots.
- 64-byte transport frames, 59-byte max payload, ACK-per-frame backpressure stays.
- No BadUSB, no keyboard emulation, no network/cloud.
- Wire-protocol changes (ACK payload, HELLO payload) must land in `web/airbridge-protocol.js` **and** the protocol harness **and** both chat pages atomically — the mock E2E must stay green at every step.
- Hardware gates use the **`question` tool** for every physical action (exit app, replug USB, launch FAP, browser pickers) per `AGENTS.md`. Never poll, never assume.
- `docs/*` and `README.md` must describe the system as it behaves at the end of this plan — no aspirational claims.

## Execution Order & Delegation Map

| Phase | Depends on | Delegation |
|-------|-----------|------------|
| 0 — Baseline repro | — | Orchestrator + user hardware gates |
| 1 — FAP relay fix | 0 (can start in parallel, deploy after) | `deep` + `flipper-zero-dev` skill |
| 2 — Hardware smoke E2E | 0, 1 deployed | Orchestrator + user hardware gates |
| 3 — JS protocol hardening | (parallel with 1) | `deep` |
| 4 — Arbitration fix | 3 | `unspecified-high` |
| 5 — Docs truthfulness | 1–4 | `writing` |
| 6 — Full regression + close | all | Orchestrator |

Phases 1 and 3 are independent — run them in **parallel**. Phase 2 gates on Phase 1's deployment. The final hardware regression (Phase 6) runs against the combined firmware + hardened pages so every wire change is re-verified on hardware exactly once.

---

## Phase 0 — Reproduce the Failure on Hardware (Baseline)

**Goal:** Capture a falsifiable "before" state so the fix is provably effective.

**Work:**

1. Rebuild the current FAP unmodified:
   ```bash
   cd /Users/asutov/projects/flipperzero-firmware
   ./fbt build APPSRC=applications_user/pocket_airbridge
   ```
2. Deploy + launch via the verified script flow (see `docs/firmware-guide.md` Option A). If `/dev/cu.usbmodemflip_*` is absent, the AirBridge app is still running on the device — use the `question` tool to have the user exit the app (BACK button) so the serial port reappears.
3. Serve the pages:
   ```bash
   cd /Users/asutov/projects/flipper-hid/web
   python3 -m http.server 8080 --bind 127.0.0.1
   ```
4. Open `http://127.0.0.1:8080/chat-usb.html` and `http://127.0.0.1:8080/chat-ble.html` (two windows; use two machines if available).
5. Connect USB side via WebHID picker, BLE side via Web Bluetooth picker (`question` tool gates for both).
6. Send `baseline usb to ble` from the USB page; observe the Flipper screen counters and the BLE page.

**Expected result (failure signature):**

- Flipper screen shows `B->U: 0` increasing only when the BLE side sends; `U->B` never increments even though USB frames arrive (the relay path is dead).
- USB page log shows `Send failed: ACK timeout` after retries.
- BLE→USB text may arrive on the USB page, but the BLE sender then fails with ACK timeout (its ACKs die USB→BLE).

**Verification scenario:** Record exact on-screen counter values and page log excerpts in `.omo/notepads/airbridge-bridge-relay-fix/issues.md`. This baseline is the regression reference — do not proceed to Phase 2 claims without it.

---

## Phase 1 — FAP Relay Fix (Core)

**Goal:** Bytes arriving on either transport are forwarded to the other transport from the FAP main loop, with no truncation and no cross-thread HAL calls.

**Affected file:** `~/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c` (single file; the BT/USB HAL patches stay untouched).

**Work:**

1. **Fix the queue**: allocate with the real event size:
   ```c
   FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(BridgeEvent));
   ```
   **Trap (found in plan review):** once elements are `sizeof(BridgeEvent)`, *every* producer must post a full `BridgeEvent` — including `input_callback`, which today posts a bare `uint32_t`. Posting 4 bytes into a 72-byte slot makes `furi_message_queue_put` read ~68 bytes past the variable (out-of-bounds read). Change `input_callback` to post `BridgeEvent{.type = EVENT_TYPE_INPUT}` as well.
2. **Relay in the main loop**: read events as `BridgeEvent`, handle all types:
   - `EVENT_TYPE_RELAY` + `to_ble == true` → `bool ok = bt_serial_tx(be.data, be.len);` — on failure increment an on-screen `TX-ERR` counter (do **not** spin-retry; protocol-level backpressure handles loss).
   - `EVENT_TYPE_RELAY` + `to_ble == false` → `furi_hal_hid_vendor_send_response(be.data, (uint8_t)be.len);`
   - `EVENT_TYPE_USB` (connect/disconnect) → update `usb_connected` and redraw.
   - `EVENT_TYPE_INPUT` → exit loop.
3. **Move BLE→USB off the GAP thread**: `ble_raw_serial_callback` must stop calling `furi_hal_hid_vendor_send_response` inline. Instead it posts `BridgeEvent{.type = EVENT_TYPE_RELAY, .to_ble = false}` with the bytes copied, using `furi_message_queue_put(queue, &be, 0)` (non-blocking; GAP-thread safe). It needs the queue passed via `bt_set_raw_serial_callback(cb, event_queue)` context.
   - Return value semantics: return the bytes the bridge can still accept (e.g., `HID_VENDOR_PACKET_LEN` on successful enqueue, `0` when the queue was full and the frame was dropped).
4. **Drop accounting**: if either callback fails to enqueue (queue full), increment a `DROPPED` counter shown on screen. With ACK-per-frame backpressure the queue should never exceed 1–2 entries; drops indicate a real problem worth seeing in the demo.
5. **Hygiene in the same file** (same edit session, separate concerns):
   - Add the missing `furi_record_close(RECORD_GUI)` before exit.
   - Replace `furi_check(furi_hal_usb_set_config(...) == true)` with a graceful error screen + early exit (a crash here wedges USB until reboot).
   - Keep the `(uint8_t*)data` cast but add a `/* HAL API takes non-const; buffer is not modified */` comment.
   - Add `furi_message_queue_put` timeout comment: callbacks use `0` because they run in GAP/USB callback context and must not block.
6. **Do not** add protocol parsing, half-duplex logic, or buffering beyond the queue. The bridge stays dumb.

**Expected result:** A FAP build where the screen counters `U->B` and `B->U` both increment when the respective direction carries traffic, and `DROPPED`/`TX-ERR` stay at 0 during healthy transfers.

**Verification scenario (pre-deploy static):**

1. Build:
   ```bash
   cd /Users/asutov/projects/flipperzero-firmware
   ./fbt build APPSRC=applications_user/pocket_airbridge
   ```
2. Expected: build succeeds, no new warnings in `pocket_airbridge.c`.
3. Self-review checklist (executor must confirm each in the final report):
   - [ ] Queue element size is `sizeof(BridgeEvent)`.
   - [ ] Every `furi_message_queue_put` posts a `BridgeEvent`; every `furi_message_queue_get` reads a `BridgeEvent`.
   - [ ] `bt_serial_tx` and `furi_hal_hid_vendor_send_response` are called **only** from the FAP main loop.
   - [ ] `bt_set_raw_serial_callback` no longer touches the USB HAL.
   - [ ] `furi_record_close(RECORD_GUI)` present.

---

## Phase 2 — Hardware Smoke E2E (Unblocks Everything)

**Goal:** Prove bidirectional relay on real hardware with the **current, unmodified** chat pages (isolates firmware as the only changed variable).

**Work:** Deploy the Phase 1 FAP and repeat the Phase 0 scenario, this time expecting success. Use the `question` tool for: exiting the running app, replugging USB if the serial port is missing, WebHID picker, Web Bluetooth picker.

**Verification scenario (all must pass):**

1. USB→BLE text: send `smoke usb to ble` from `chat-usb.html` → appears in `chat-ble.html` transcript; USB page logs `Sent text message`; Flipper `U->B` increments, `DROPPED: 0`, `TX-ERR: 0`.
2. BLE→USB text: send `smoke ble to usb` → appears on USB page; `B->U` increments.
3. USB→BLE attachment: 1 KB `.txt` fixture → BLE page shows attachment card `SHA-256 verified` + download link.
4. BLE→USB attachment: same fixture back → USB page verifies hash.
5. Cancel (sender side): start a ≥20 KB attachment from the USB side, press **Cancel on the USB (sending) page** mid-transfer → BLE page shows `Transfer cancelled`, both sides return to `idle` within 2 s, Flipper `DROPPED: 0`. (Note: current pages only expose Cancel while *sending* — receiver-side cancel is added in Phase 3 and regression-tested in Phase 6.)
6. Disconnect mid-transfer: unplug BLE-side (flip Bluetooth off or close the BLE tab) during a USB→BLE send → USB page fails with a visible error, Flipper screen returns to `BLE: --`, app does not crash (screen still redraws, BACK exits cleanly).

**If any step fails:** stop, record symptoms in the notepad, and consult Oracle with the Phase 0 baseline + new symptoms before changing code. Do not shotgun-debug the firmware.

---

## Phase 3 — JS Protocol Hardening

**Goal:** Fix the shared-protocol correctness bugs without changing the transport shape. All changes land in `web/airbridge-protocol.js` first, then both chat pages, then the harness — mock E2E must stay green throughout.

**Affected files:** `web/airbridge-protocol.js`, `web/chat-usb.html`, `web/chat-ble.html`, `web/protocol-harness.html`.

**Work:**

1. **ACK disambiguation** — ACK payload becomes 3 bytes: `[acked_type(1), acked_seq(2 BE)]`. `ItemSender.sendWithAck` matches on both fields; `readAckSeq` returns `{type, seq}`. This kills the HELLO/META-0/DONE/CANCEL shared-seq-space collision class. Update the harness's frame fixtures.
2. **HELLO carries the item id** — `sendHello(itemId)` sends the 4-byte item id from the item's metadata (per `docs/protocol.md`), not the per-connection `sessionId`. Both pages pass `meta.itemId`. (Prerequisite for Phase 4 arbitration.)
3. **Hash-failure propagation** — `ItemReceiver.handleDone`: verify SHA-256 **before** ACKing DONE; on mismatch send `MSG.ERROR` with a UTF-8 reason and emit `error`. `ItemSender`: any `MSG.ERROR` received while a send is in flight rejects the pending send with the peer's message (no more silent success/failure divergence between the two UIs).
4. **Receiver input caps** — `ITEM_DATA` before any `ITEM_META`: **drop silently** (log via the `invalid` event, do **not** send `MSG.ERROR` — stale frames from a Phase 4 arbitration yield must not poison the winner's transfer). Oversized metadata (`meta.chunks > 4096` or `meta.size > 4 MiB`): reject with `MSG.ERROR` (bounded demo memory; error is user-visible on both sides).
5. **Split busy events** — `ItemReceiver` emits `rejected` (we turned away a peer's HELLO) separately from `busy` (peer told us to back off). `chat-ble.html` backs off only on `busy`; the current self-penalty (chat-ble.html:368-376 reacting to our own rejection) disappears.
6. **Disconnect aborts transfer** — `chat-usb.html` `handleDisconnected` aborts `activeTransfer` (parity with chat-ble).
7. **Receiver-side cancel** — both pages expose Cancel while *receiving* as well as while sending (today `cancelBtn.hidden` keys off `isSending` only, e.g. chat-ble.html:188). Receiver cancel sends `MSG.CANCEL`; the sending peer aborts via its existing CANCEL handling. This is required by the protocol spec ("either side may send CANCEL") and by the Phase 2 test narrative.
8. **Object URL hygiene** — revoke `URL.createObjectURL` handles when a transcript entry is replaced/cleared.

**Expected result:** All existing harness tests updated to the 3-byte ACK pass, plus new harness cases: ACK type mismatch is not accepted, ERROR-on-hash-mismatch reaches the sender, DATA-before-META is rejected, oversized META is rejected.

**Verification scenario:**

1. Serve `web/` and open `http://127.0.0.1:8080/protocol-harness.html`.
2. Expected results include:
   ```text
   PASS frame roundtrip
   PASS fragmented meta
   PASS text item usb-to-ble
   PASS text item ble-to-usb
   PASS attachment sha256
   PASS busy collision
   PASS ack type mismatch rejected
   PASS hash mismatch sends error
   PASS data before meta rejected
   PASS cancel regression
   ```
3. Mock pages (`chat-usb.html?mock=1`, `chat-ble.html?mock=1`) exchange text and a small attachment in both directions with zero console errors.

---

## Phase 4 — Half-Duplex Arbitration Fix

**Goal:** Make simultaneous-send resolution match the spec (or fix the spec — pick one and make code + docs agree).

**Decision (pre-made by this plan):** Implement the spec — **lower `itemId` wins** — since Phase 3 already puts the real item id into HELLO. Deterministic beats accidental.

**Work (both chat pages, mirrored logic):**

1. On receiving a peer `HELLO` while locally sending: compare peer `itemId` vs local `outboundItemId`.
   - Peer id lower → local side aborts its send (user-visible `Yielded to peer item`), sends `ACK` for the peer HELLO, and becomes receiver.
   - Local id lower → send `BUSY` carrying `{winnerItemId, loserItemId, retryAfterMs}` and continue.
2. On receiving `BUSY`: if `winnerItemId` == peer's id and our id is higher → back off `retryAfterMs + random(0..400)` jitter, then retry (existing `sendWithBusyRetry` path, now fed by real comparison instead of unconditional).
3. Equal ids (astronomically unlikely with random 32-bit ids): both sides treat as loss, back off with jitter, retry.
4. **Stale-frame grace rule:** after a yield, the loser may still have ≤1–2 frames in flight (ACK-per-frame bounds this). The winner's receiver absorbs them via the Phase 3 silent DATA-before-META drop — **no `MSG.ERROR`, no state reset**. The yield abort happens between chunks (AbortController is checked per frame), so the window is inherently tiny; do not add flush logic beyond this.

**Expected result:** New harness case `PASS simultaneous hello arbitration` — two mock endpoints start sends in the same tick; exactly the lower-id item completes, the other retries and completes after.

**Verification scenario:** Harness case above, plus mock pages: trigger sends from both pages within the same second (scripted via Playwright clicking both Send buttons) → both messages eventually appear on both pages, exactly one `Yielded to peer item` system message exists across both transcripts.

---

## Phase 5 — Documentation Truthfulness Pass

**Goal:** Every doc describes the system as it now behaves. No aspiration presented as fact.

**Affected files:** `README.md`, `AGENTS.md`, `docs/architecture.md`, `docs/protocol.md`, `docs/firmware-guide.md`, `flipper/bridge_app.c`.

**Work:**

1. `docs/firmware-guide.md`
   - Rewrite "Bridge Logic (Inside the FAP)" to match the shipped design: queue-based dumb relay, main-loop forwarding both directions, drop/error counters.
   - Replace "The firmware now supports bidirectional chat and attachment exchange" with a status line gated on Phase 2/6 results (state exactly what was tested: text + attachment, both directions, date).
   - Note: deploy requires exiting the running app first so `/dev/cu.usbmodemflip_*` reappears (already in notepads; belongs in the guide).
2. `docs/protocol.md`
   - ACK payload is now `[type, seq]` (3 bytes) — update the type table and ACK/NACK semantics section.
   - HELLO payload is the 4-byte `itemId` (matches implementation after Phase 3).
   - META JSON field is `kind` (code wins; doc changes), not `itemType`.
   - Document: receiver verifies hash before ACKing `ITEM_DONE` and sends `ERROR` on mismatch; `NACK` is defined but currently unused (reserved).
   - Document the consent model: BLE pairing PIN on the Flipper screen + browser permission pickers are the consent mechanism.
3. `docs/architecture.md`
   - Bridge section: state plainly that the Flipper is a stateless byte pipe; half-duplex is enforced by the endpoints; remove or clearly mark the bridge state machine as *not implemented*.
   - Design decisions table: update the "bridge enforces half-duplex" rationale.
4. `AGENTS.md`
   - Repository layout: add `web/airbridge-protocol.js`, `web/airbridge-transports.js`, `web/protocol-harness.html`, `web/chat-usb.html`, `web/chat-ble.html`; mark `sender.html`/`receiver.html` as legacy.
   - Protocol list: `HELLO(0x01), ITEM_META(0x02), ITEM_DATA(0x03), ACK(0x04), NACK(0x05), ITEM_DONE(0x06), ERROR(0x07), BUSY(0x08), CANCEL(0x09)`.
5. `README.md`
   - Update phase checkboxes honestly; add a "Verified on hardware (date)" line listing exactly what passed.
6. `flipper/bridge_app.c`
   - Move to `docs/attic/bridge_app.c` with a header comment: *superseded pseudocode — the live FAP is `applications_user/pocket_airbridge/pocket_airbridge.c` in the firmware repo*. (Do not delete; it has historical value.)

**Verification scenario:** Grep-driven checks:

1. `grep -rn "itemType" docs/` → zero hits.
2. `grep -n "bt_serial_tx" docs/firmware-guide.md` → description matches the shipped relay.
3. `grep -n "BUSY" AGENTS.md` → present in the protocol list.
4. A reader following only `README.md` + `docs/firmware-guide.md` reaches a working two-direction demo without prior session knowledge.

---

## Phase 6 — Full Regression & Close-Out

**Goal:** One combined pass over everything, then lock the state.

**Verification gates (all must pass before closing):**

1. - [x] **Harness:** all protocol tests PASS on localhost, zero console errors. (13/13 incl. arbitration, verified 2026-07-20)
2. - [x] **Mock E2E:** text + attachment both directions on `?mock=1` pages; simultaneous-send arbitration case passes. (cross-tab BroadcastChannel mock added)
3. - [x] **Hardware text:** both directions on real Flipper (2026-07-20).
4. - [x] **Hardware attachment:** 38 B and 20 KB both directions, SHA-256 verified both times (~22 s per 20 KB).
5. - [x] **Hardware cancel + disconnect:** sender-side and receiver-side cancel verified; receiver CANCEL propagates via retry loop.
6. - [x] **Counters:** `U->B`/`B->U` symmetric, `DROP: 0`, `TXERR: 0` healthy; one benign TXERR during cancel race.
7. - [x] **Docs:** Phase 5 grep checks pass; firmware bundle added at `docs/firmware/` (patches audited against latest upstream dev — still required).
8. - [ ] **Review:** `review-work` skill run over the firmware + web diffs; findings triaged. (optional follow-up)

**Close-out:** Update this plan's checkboxes, append outcomes to `.omo/notepads/airbridge-bridge-relay-fix/learnings.md`, and record rough throughput numbers (chunks/sec, KB/s) observed in gate 4 for the demo script.

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| `bt_serial_tx` from the FAP main thread misbehaves (only ever called from RPC thread upstream) | Phase 2 gate tests it immediately; fallback is posting through the BT service message queue (`BtMessage`) like the reset-request path does. Consult Oracle before any stack-level change. |
| Browser writes are fragmented under small ATT MTU (frame arrives as multiple `DataReceived` events) | Phase 2 smoke would show garbled frames on the USB side. Fix: length-prefixed reassembly in the FAP raw callback (header already carries `len`) — small, contained change. Same reassembly on the JS TX-indication side (`airbridge-transports.js`). |
| Single-PC two-tab WebHID open fails (device exclusivity) | AGENTS.md already sanctions single-PC testing; if macOS refuses the second open, use two machines or a second browser profile. Record what was used in the notepad. |
| ACK `[type, seq]` change breaks mock tests mid-phase | Harness updated in the same commit as `airbridge-protocol.js`; never land page changes before the shared module + harness are green. |
| Queue-full drops under bursty USB input | Queue depth 8 ≈ 600 B RAM; with ACK-per-frame there is at most ~1 frame in flight per direction. `DROPPED` counter makes any violation visible during the demo instead of silent corruption. |
| Scope creep into throughput work | `writeValueWithoutResponse` + windowed ACKs are explicitly out of scope; current speed (~0.5–2 KB/s) is demo-acceptable. |

## Out of Scope

- Encryption/authentication beyond the existing BLE pairing requirement.
- Throughput overhaul (write-without-response, sliding-window ACKs, larger MTU negotiation).
- True full-duplex concurrent transfers.
- Multi-peer rooms, resume/partial-transfer recovery, compression.
- Committing the BT/USB HAL patches to the firmware repo's git (separate decision; the revert script in the firmware guide stays valid until then).
- Legacy `sender.html`/`receiver.html` feature parity (they keep their existing warning banners).

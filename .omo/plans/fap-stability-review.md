# fap-stability-review - Work Plan

## TL;DR (For humans)

**What you'll get:** A fix set for the Pocket AirBridge FAP's intermittent
button unresponsiveness, plus hardening of a residual use-after-free window in
the patched firmware's `bt.c`. Root cause (evidence-backed): the app's single
8-slot event queue carries both button events and USB-ISR/BLE relay traffic;
while the main loop blocks in BLE typing retries (worst ~5 s/keystroke), BLE
stream spins (~2 s/chunk), or 50 ms/chunk blocking USB sends, relay events
fill the queue and button presses are silently dropped (`dropped++` only).

**Why this approach:** surgical, evidence-ranked fixes — a dedicated input
queues drained first every loop iteration (relay traffic can never consume
input capacity; BACK has its own queue and cannot be evicted by non-BACK
input), abort-aware retry/spin loops plus
a non-blocking async release-all (BACK honored at the next retry boundary of
the in-flight operation: typical 20–50 ms, worst ~1.2 s for one gatt retry
storm — `gatt.c` retry bounds stay untouched), a bounded teardown wait
replacing the 200 ms heuristic, and firmware-side `bt->current_profile`
ownership hardening with an explicit, narrowed safety argument. No
worker-thread refactor.

**What it will NOT do:** redesign the app architecture, change the wire
protocol, touch the web pages, or add features. No runtime USB profile
switching. No behavior change to happy-path transfers.

**Effort:** ~6 files touched (1 FAP source + firmware `bt.c`, `bt_api.c`, and
`bt_i.h` + regenerated bundle + this repo's README note). **Risk:** moderate;
`bt.c` is a patched upstream service file, so the firmware change is kept
minimal but includes a real mutex barrier around profile-pointer lifetime.

**Decisions (user-approved 2026-07-24):** FAP + firmware hardening in scope;
input-queue + abort-aware loops (no worker thread); tests-after with hardware
QA per AGENTS.md regression rule plus new freeze-soak tests.

## Scope

**IN:**
- `/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c`
  (canonical FAP source; the `flipper-hid/firmware/pocket_airbridge/` bundle
  copy is byte-identical today and is regenerated, not hand-edited).
- `/Users/asutov/projects/flipperzero-firmware/applications/services/bt/bt_service/bt.c`,
  `bt_api.c`, and `bt_i.h` (minimal hardening of `bt->current_profile`
  ownership/lifetime).
- Regeneration of the `flipper-hid/firmware/` bundle after fixes land.
- Hardware verification on the user's Flipper Zero (per AGENTS.md gate
  protocol).

**OUT (Must-NOT-Have):**
- No worker-thread redesign of typing/streaming.
- No changes to `web/*.html`, `web/*.js`, the wire protocol, or BLE/USB UUIDs.
- No new features, screens, or config keys.
- No changes to `furi_hal_usb_airbridge.c`, `airbridge_serial_service.c`,
  `airbridge_profile.c`, `gatt.c` (their retry behavior is absorbed, not
  modified).
- No runtime USB profile switching (known-fatal on this USB stack).

## Verification strategy

- **Tests-after** (no unit-test infra exists for this FAP; verification is
  build + static review + agent-executed hardware QA).
- Build gates: `./fbt build APPSRC=applications_user/pocket_airbridge` (FAP)
  and a full `./fbt` firmware build (for the `bt.c` change) must compile
  warning-clean with `-Werror` as configured by the tree.
- Hardware QA (AGENTS.md regression rule + new soaks), executed by the agent
  with `question`-tool gates for physical actions and the Playwright window
  for browser surfaces:
  1. Bridge chat both directions (USB↔BLE text).
  2. File transfer with SHA-256 verification, both directions.
  3. USB Deploy flow end-to-end (blank.org + DevTools console).
  4. BLE Deploy flow end-to-end.
  5. Protocol harness 17/17.
  6. NEW freeze-soak A: during a BLE file transfer, run exact valid button
     pairs — `UP -> USB DeployPrompt -> BACK -> Bridge`, `DOWN -> BLE
     DeployPrompt -> BACK -> Bridge`, and `OK on Bridge -> no screen change`;
     no BACK lost (BACK has its own queue, with coalescing only when multiple
     BACKs are already pending).
  7. NEW freeze-soak B: press BACK once mid-BLE-typing under induced link
     congestion — abort honored at the next retry boundary of the in-flight
     keystroke (typical 20–50 ms, worst ~1.2 s under a sustained gatt storm,
     measured from the permanent BACK enqueue/handled debug logs), no
     spurious error screen, release-all retried asynchronously up to 3× with
     logged failure (best-effort, not hard-guaranteed).
  8. NEW teardown-soak: launch app, connect BLE central, exit app — 20
     cycles, zero crashes/hangs (post-exit log ring buffer or on-screen state
     checked for furi_crash/assert).
- Debug counters `DROP`/`TXERR` must remain 0 during healthy baseline
  transfers; BACK never increments `DROP`; non-BACK overflow may increment
  `DROP` and is not a failure outside the scripted soak.

## Execution strategy

Single worker, sequential on the FAP file (todos 1–3), firmware bt-service
hardening (`bt.c`/`bt_api.c`/`bt_i.h`, todo 4) may run in parallel with 1–3
since it is a different file set;
build/deploy (5) after both; hardware QA (6) after deploy; bundle regen (7)
last. Dependency matrix:

| Todo | Depends on |
|------|-----------|
| 1 | — |
| 2 | 1 (same file, uses input-service helper) |
| 3 | 1 (same file) |
| 4 | — (different file/repo area) |
| 5 | 1, 2, 3, 4 |
| 8 | 4 (regression fix) |
| 6 | 5, 8 |
| 7 | 6 |

## Todos

- [x] 1. FAP: dedicated input queue, lossless ordered BACK, drained first each loop
  - **What:** In `pocket_airbridge.c`: add `FuriMessageQueue* input_queue`,
    `FuriMessageQueue* back_queue`, `uint32_t next_input_sequence`, and
    `bool running` to `AirbridgeApp` (struct at lines 79-112). Extend
    `BridgeEvent` (lines 71-77) with `uint32_t tick` and
    `uint32_t sequence`, both set in `input_callback` at enqueue time
    (`sequence = app->next_input_sequence++`). Allocate
    `input_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent))` and
    `back_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent))` next to
    the existing relay queue alloc (line 1191). Change
    `view_port_input_callback_set(view_port, input_callback, app->event_queue)`
    (line 1197) to pass `app` (NOT a bare queue) as context. Rewrite
    `input_callback` (lines 1081-1096): keep the Back=Press / other=Short
    filter; Back events go to `app->back_queue`, all other accepted events go
    to `app->input_queue`. If `back_queue` is full, coalesce the new BACK
    into the already-pending BACK set: do NOT increment `dropped`, do NOT
    evict an older BACK, and log `FURI_LOG_D(TAG, "BACK coalesced")`.
    If `input_queue` is full for a non-BACK event, `dropped++` as today. Move
    the loop's `running` local (line 1207) into `app->running`. Add helper
    `static void app_service_input(AirbridgeApp* app)` that merges the heads
    of `back_queue` and `input_queue` by `(tick, sequence)` lexicographic
    order: lower tick wins; when two events share the same tick, lower
    monotonic sequence wins. At each step, peek/dequeue the older head; if
    the platform queue lacks peek, implement
    this with one cached head slot per queue in `AirbridgeApp` (`bool
    have_back_head`, `BridgeEvent back_head`, `bool have_input_head`,
    `BridgeEvent input_head`). Call `app_handle_input(app, be.key,
    &app->running)` per merged event. This preserves temporal order even for
    multiple button events in the same scheduler tick while ensuring relay
    traffic and non-BACK input can never evict BACK. Add two permanent
    debug-level timing logs: in
    `input_callback`
    on accepted BACK (`FURI_LOG_D(TAG, "BACK enqueued %lu", furi_get_tick())`)
    and in `app_service_input` after each BACK is handled
    (`"BACK handled %lu"`) — QA timing evidence for todo 6. Main loop
    (lines 1208-1293): call `app_service_input(app)` FIRST every iteration,
    then `furi_message_queue_get(app->event_queue, &be, 10)` for relay/USB
    events only. Remove the now-dead `EVENT_TYPE_INPUT` dispatch branch
    (lines 1216-1217). Update the other two `running` uses when moving it
    into `app->running`: the loop condition (line 1208) and the
    BLE-config-failure `running = false;` (line 1234). Add a forward
    declaration of `app_service_input` above `app_ble_kb_report_with_retry`
    (line 157) so todo 2 can call it. Free `input_queue` and `back_queue` at
    teardown before
    `furi_message_queue_free(app->event_queue)` (line 1307).
  - **References:** `applications_user/pocket_airbridge/pocket_airbridge.c`
    lines 79-112 (struct), 1081-1096 (input_callback), 1098-1155
    (app_handle_input), 1176-1311 (app entry/teardown).
  - **Acceptance:** code compiles; input events no longer share storage with
    relay events — relay traffic can NEVER consume input queue capacity
    (precise claim; non-Back presses can still drop if >4 arrive inside one
    uninterruptible ~1 s gatt stall, acceptable); BACK is semantically
    lossless: it is either queued in `back_queue` or coalesced with already
    pending BACK events; BACK never increments `dropped`; merged servicing by
    `(tick, sequence)` preserves temporal order between queued BACK and queued
    non-BACK events, including same-tick events;
    the two BACK timing logs exist at debug level.
  - **QA:** happy — `./fbt build APPSRC=applications_user/pocket_airbridge`
    succeeds; evidence: build log saved to `/tmp/fap-build-t1.log`. failure —
    introduce deliberate relay flood (hardware soak in todo 6) and confirm
    presses still honored; evidence: soak notes in QA log.
  - **Commit:** `fix(fap): dedicated input queue, drain before relay queue`

- [x] 2. FAP: abort-aware BLE retry loops + non-blocking release-all
  - **What:** (a) Change `app_ble_kb_report_with_retry` (lines 157-173) to
    signature `static bool app_ble_kb_report_with_retry(AirbridgeApp* app, uint8_t* report)`
    (use `app->ble_profile` internally; NO flag needed — see (b): the only
    remaining call sites are the typing press/release paths). Add
    `uint32_t typing_generation` to `AirbridgeApp`; increment it in
    `app_start_typing` (line 549) and `app_abort_typing` (line 193). At
    helper entry, capture `uint32_t generation = app->typing_generation`.
    Before each retry attempt call `app_service_input(app)`, then return
    false if `app->screen` is no longer `AirbridgeScreenTyping` OR
    `generation != app->typing_generation`. Check the same generation again
    immediately before sending the local report. This closes the queued
    `BACK -> DOWN -> OK` race: an old local report from an aborted typing
    session cannot be sent into a newly-started typing session that happens
    to reuse `AirbridgeScreenTyping`.
    Call sites: `app_typing_press` (line 577) and `app_typing_release`
    (line 586).
    (b) NON-BLOCKING RELEASE-ALL (fixes the hidden abort-latency budget):
    `app_abort_typing` (lines 193-210) currently sends the BLE release-all
    synchronously through the full 5-attempt retry wrapper — up to ~5 s under
    congestion, ON the abort path. Change it: `app_abort_typing` no longer
    calls any BLE report function. It clears the typing flags
    (`typing_key_down`, `typing_enter_pending`, `typing_enter_done`), calls
    `app_set_hids_adv(app, false)` for BLE, and — if a key was down — sets a
    new `app->release_all_pending = true` (add to struct; pending is
    inherently BLE-only, independent of mutable `typing_transport`). USB path
    keeps its existing synchronous `furi_hal_hid_airbridge_kb_release_all()`
    (ISR-driven, non-blocking). Main loop: service release-all BEFORE any new
    `app_typing_step()` call; when `release_all_pending` and
    `app->ble_profile` is non-NULL, attempt ONE
    `ble_profile_airbridge_kb_report` zero-report directly (NO retry
    wrapper); on success clear the flag and reset `release_all_attempts`; on
    failure increment the counter, and after 3 failed attempts clear the flag
    and `FURI_LOG_E(TAG, "release-all FAILED - tap a key on target")`. If the
    user starts a new typing session while `release_all_pending` is still set,
    `app_start_typing()` must first clear the flag and log
    `FURI_LOG_W(TAG, "release-all superseded by new typing session")`; this is
    safe because HID keyboard reports are absolute state and the first new
    key report overwrites the host's previous state. Teardown (before
    `app_restore_ble`): if `release_all_pending` still set, drain it with up
    to 3 attempts separated by 20 ms; explicitly accepted worst-case exit
    stall under sustained gatt congestion is ~3.6 s (3 × ~1 s GATT retry +
    delays + 500 ms disconnect wait).
    (c) Abort/generation-vs-error distinction: in `app_typing_step`, before
    EVERY call to `app_typing_press` or `app_typing_release`, capture
    `uint32_t step_generation = app->typing_generation`. At EVERY place a
    press/release false return currently leads to
    `app_show_error(app, "KEYBOARD SEND ERROR")` (lines 602-605, 628-630,
    639-641), first check
    `if(app->screen != AirbridgeScreenTyping || app->typing_generation != step_generation) return;`
    — a false return caused by user abort or by queued `BACK -> DOWN -> OK`
    starting a NEW typing generation must NOT clobber the new session with an
    error screen. On the release path specifically, this generation/screen
    check must run BEFORE any `app_abort_typing(app)` call; otherwise an old
    release failure can abort the newly-started generation. NOTE: genuinely
    queued inputs after BACK are honored normally — if the user pressed BACK
    then DOWN, the screen legitimately becomes DeployPrompt; the guarantee is
    "no spurious error screen", not "always lands on Bridge".
    (d) In `app_stream_step_ble` (lines 783-790): inside the
    `while(!bt_serial_tx(...))` spin add
    `app_service_input(app); furi_delay_ms(2);` per iteration; if
    `app->screen != AirbridgeScreenStreaming` after servicing (BACK abort ran
    `app_stream_close`), return immediately without touching
    `stream_tx_strikes`. Keep `BLE_STREAM_RETRY_MAX` at 20.
  - **References:** `pocket_airbridge.c` lines 157-173, 193-210, 564-589,
    591-657, 743-793; blocking-cost evidence: `targets/f7/ble_glue/furi_ble/gatt.c:144-153`
    (1000×1 ms — one in-flight kb_report call can block ~1 s, NOT
    interruptible), `targets/f7/ble_glue/services/airbridge_serial_service.c:298-316`
    (100×1 ms — one bt_serial_tx ≤ ~100 ms).
  - **Acceptance:** BACK during BLE typing is honored at the next retry
    boundary of the IN-FLIGHT keystroke only (typical 20–50 ms, worst ~1.2 s)
    — the release-all path adds ZERO to abort latency because it is async;
    no "KEYBOARD SEND ERROR" after a user abort or generation change; no
    report from an old typing generation can be sent after queued input starts
    a new typing generation; an old step's false return cannot abort or error
    the new generation; queued `BACK -> DOWN -> OK` during a blocked BLE
    report starts the new generation cleanly with no old-step error screen;
    release-all is attempted up
    to 3× across loop iterations and its failure is logged (honest criterion:
    stuck-modifier avoidance is best-effort-with-retries, verified in soak B,
    not hard-guaranteed); BACK during BLE streaming honored within ~100 ms
    (one bt_serial_tx bound); no input serviced from the release-all path
    (no recursion — verifiable by inspection: `app_service_input` is called
    only from the retry helper, the stream spin, and the main loop, and
    `app_abort_typing` calls no report function at all).
  - **QA:** happy — build clean, evidence `/tmp/fap-build-t2.log`. failure —
    soak B (todo 6): BACK mid-typing under congestion aborts with no error
    screen; release-all succeeds in normal congestion or logs failure
    honestly; target PC key state checked for stuck modifier.
  - **Commit:** `fix(fap): service input inside BLE retry/spin loops so BACK aborts promptly`

- [x] 3. FAP: bounded BLE teardown wait, OOM comment, minor correctness
  - **What:** (a) In `app_restore_ble` (lines 426-454): replace the fixed
    `furi_delay_ms(200)` after `bt_disconnect` (line 443) with a bounded wait
    for the disconnect to actually land:
    `uint32_t start = furi_get_tick(); while(app->ble_connected && (furi_get_tick() - start < 500)) furi_delay_ms(10);`
    If `ble_connected` is already false, no wait at all (0 ms — there is NO
    200 ms floor). The disconnect signal reaches `app->ble_connected`
    indirectly (the `GapEventTypeDisconnected` branch at `bt.c:352-368` does
    not itself fire the status callback; the callback fires via the
    subsequent StopAdvertising path `bt.c:373-376`), so the 500 ms budget is
    intentional. Treat the wait as "give BleEventWorker time to finish
    processing the disconnect event, bounded" — `ble_connected` is a proxy:
    if the link dropped while GAP was idle, no status update fires and the
    full 500 ms elapses, which is fine because the HCI event has long been
    processed by then. Keep the existing explanatory comment, updated.
    (b) NO new caller-side alloc NULL checks — verified infeasible:
    `view_port_alloc` (`gui/view_port.c:93-98`) and `storage_file_alloc`
    (`applications/services/storage/storage_external_api.c:926-935`)
    dereference their `malloc` result before returning, and
    `furi_message_queue_alloc` (`furi/core/message_queue.c:27-40`)
    `furi_check`s internally — OOM crashes inside the allocator in all three
    cases, so caller-side checks would be dead code. Instead: keep the one
    check that exists (the app-struct `malloc` at line 1178) and add a short
    comment at the startup alloc block stating OOM-in-allocator is
    unrecoverable by firmware design. (c) Mark `app->ble_connected` as
    `volatile` (written on the BtSrv thread via `bt.c:619-620` →
    `app_ble_status_changed_callback` lines 175-191, read on main + GUI
    threads) and the file-static `usb_connected` as `volatile` (written by
    the MAIN loop at line 1215, read by the GUI thread at lines 910/916).
    (d) The raw-serial credit return stays constant
    `HID_VENDOR_PACKET_LEN` — intentionally NOT changed (the service only
    logs it; changing advertised credit would alter happy-path flow control
    and risk overcommit drops; out of scope).
  - **References:** `pocket_airbridge.c` lines 114-121 (statics), 426-454,
    824-839, 1176-1210, 1295-1311; teardown-race evidence:
    `applications/services/bt/bt_service/bt.c:306-307` (GAP-thread readers of
    `bt->current_profile`), `bt_api.c:25,30` (cross-thread assignments), FAP
    mitigation comment at `pocket_airbridge.c:432-441`.
  - **Acceptance:** teardown waits for the observed disconnect status update
    with a 500 ms bounded fallback (zero wait when already disconnected); the
    startup alloc block carries the OOM-by-design comment and no dead NULL
    checks are added; no functional change to happy path (including
    unchanged serial credit).
  - **QA:** happy — build clean, evidence `/tmp/fap-build-t3.log`. failure —
    teardown-soak (todo 6, 20 exit cycles under active BLE link): zero
    crashes/hangs; evidence: QA log with cycle count + heartbeat observation.
  - **Commit:** `fix(fap): bounded disconnect wait at teardown, volatile cross-thread flags`

- [x] 4. Firmware: serialize `bt->current_profile` lifetime in bt_service
  - **What:** In `/Users/asutov/projects/flipperzero-firmware/applications/services/bt/bt_service/`,
    make the BtSrv thread the sole writer of `bt->current_profile` AND add a
    real lifetime barrier for every reader. Exact edits (worker verifies
    current lines before editing):
    (1) `bt_api.c:25` — DELETE the caller-thread assignment
    `bt->current_profile = profile_instance;` inside `bt_profile_start`. The
    return channel ALREADY exists: `bt_change_profile` writes the new profile
    into `BtMessage.profile_instance` (`bt.c:479-480, 491-492`, member
    declared `bt_i.h:64`) and `bt_profile_start` reads it into its local
    (`bt_api.c:11,16,26`). Do NOT add a new message field.
    (2) `bt_api.c:30` — REWRITE `bt_profile_restore_default` (currently
    `bt->current_profile = bt_profile_start(bt, ble_profile_serial, NULL);`,
    another caller-thread write) to
    `return bt_profile_start(bt, ble_profile_serial, NULL) != NULL;`.
    (3) `bt_i.h` — add `FuriMutex* current_profile_mutex` to `Bt`. Allocate
    it before any `bt->current_profile` assignment and free it with the Bt
    service object. Serialize ALL writers: startup/key-load writers
    (`bt_load_keys` and `bt_start_application`, `bt.c:522,529`) either run
    before callbacks can observe `Bt` (document with code-order evidence) or
    take the mutex around their writes; `bt_change_profile` always takes it.
    (4) `bt_change_profile` (`bt.c:456-495`) — call
    `bt_close_rpc_connection(bt)` FIRST with no profile mutex held (line 460;
    avoids deadlock with any in-flight RPC callback), then acquire
    `current_profile_mutex`, publish `bt->current_profile = NULL`, release
    the mutex, call `furi_hal_bt_change_app` (line 464; it reinits Core2 and
    joins/stops GAP), then reacquire the mutex to publish the new pointer
    (or leave NULL on failure) and release it. NEVER hold
    `current_profile_mutex` across `furi_hal_bt_change_app` or any GAP join.
    (5) Cover every current-profile read/use site, not just the first
    snapshot. Reader list to update: `bt_serial_tx` (`bt.c:38-45`),
    `bt_close_rpc_connection` (`bt.c:445-453`),
    `bt_serial_event_callback` (`bt.c:225,236,243`),
    `bt_rpc_send_bytes_callback` (`bt.c:274,277`),
    `bt_serial_buffer_is_empty_callback` (`bt.c:295-296`), and ALL
    `bt_on_gap_event_callback` reads/uses (`bt.c:306-307,336-338,361,365`),
    plus the read in `bt_start_application` (`bt.c:529`) if it is not proven
    startup-only-before-callbacks.
    Lock granularity is mandatory: hold `current_profile_mutex` only while
    copying the profile pointer / computing profile-type booleans or while
    doing a short immediate profile call that needs the profile pointer;
    NEVER hold it across `rpc_session_close`, `furi_hal_bt_change_app`, GAP
    thread joins, `furi_event_flag_wait`, raw-serial callbacks, queue puts,
    or any operation whose completion depends on GAP/RPC callbacks. In void
    For `bt_close_rpc_connection`, snapshot/type-check under the mutex, then
    release before `rpc_session_close`; do not raw-deref `bt->current_profile`
    after the snapshot. In void RPC callbacks, replace furi_check-style assumptions with executable
    guarded behavior: if profile is NULL or wrong type, log once and return
    early; for `bt_serial_buffer_is_empty_callback`, set the RPC
    buffer-sent/unblock event before returning if that is required to avoid a
    stuck waiter. No impossible boolean return values are added to void
    callbacks.
  - **Safety argument:** the mutex is the lifetime barrier. Any reader already
    using the old profile completes before NULL is published; readers arriving
    after NULL publication see NULL/type-guarded bail paths while the old
    profile is freed and the new pointer is assigned; fresh readers then see
    a valid profile or a NULL/type-guarded bail path. The RPC callbacks are included because
    `rpc_session_close()` may leave an in-flight callback after unregistering
    callbacks and signaling the worker; the mutex prevents use-after-free,
    and the NULL/type guards prevent furi_check crashes.
  - **References:** `applications/services/bt/bt_service/bt.c:28-45, 225,236,243,
    274-277, 295-296, 306-307, 336-338, 361,365, 445-453, 456-495, 522,529`; `bt_api.c:4-43`; `bt_i.h` (`Bt` struct
    and `profile_instance` member around line 64); `applications/services/rpc/rpc.c:203-205,423-431`
    (why RPC callbacks need the barrier); FAP mitigation comment at
    `pocket_airbridge.c:432-441`.
  - **Acceptance:** grep confirms no `bt->current_profile =` writes in
    `bt_api.c`; every writer (`bt_change_profile`, `bt_load_keys`,
    `bt_start_application`) is startup-only-before-callbacks or
    mutex-protected; every current-profile reader named above is covered by
    `current_profile_mutex` or uses booleans copied under that mutex; RPC
    session close still sees the old pointer (close precedes mutex-protected
    NULL publish); no profile mutex is held across waits, GAP joins,
    `furi_hal_bt_change_app`, `rpc_session_close`, raw callbacks, queue puts,
    or callback-dependent operations; full
    firmware `./fbt` build warning-clean; RPC/BLE stock behavior unchanged —
    verified by `scripts/storage.py list /ext` after flash; stress: launch
    AirBridge while a BLE RPC session is active, then restore default 20×
    without crash.
  - **QA:** happy — full `./fbt` build log `/tmp/fw-build-t4.log` +
    post-flash RPC round-trip (`scripts/storage.py list /ext`) succeeds,
    evidence in QA log. failure — teardown-soak from todo 6 plus the active
    BLE-RPC launch/restore stress shows zero `furi_crash`; evidence: QA log.
  - **Commit:** `fix(bt): serialize current_profile lifetime during profile changes`

- [x] 5. Build, flash firmware, deploy FAP to device
  - **What:** Full firmware build (includes todo 4), flash to the Flipper
    (user's established method per `docs/firmware-guide.md`), then rebuild +
    deploy the FAP via `scripts/storage.py send` to the canonical path
    `/ext/apps/USB/pocket_airbridge.fap` (assert exactly one copy:
    `storage.py list /ext/apps | grep -i airbridge` returns one line; remove
    strays), verify size, launch with `scripts/runfap.py`.
  - **References:** `docs/firmware-guide.md`; AGENTS.md "Development
    Workflow" §3.
  - **Acceptance:** device boots on new firmware; FAP launches and shows the
    bridge screen with heartbeat LED.
  - **QA:** happy — `storage.py size` matches build artifact, app screen
    visible; evidence: QA log + screen photo/`runfap.py` output. failure —
    if DFU/flash needed, use `question`-tool gate with attention signal per
    AGENTS.md.
  - **Commit:** none (deploy step).

- [x] 8. Firmware: eliminate `current_profile_mutex`/HCI deadlock via reader refcount
  - **What:** REGRESSION FIX (hardware wedge found post-flash: device freezes
    on navigation/deploy, random but often; root cause confirmed with
    citations: `bt_serial_tx` and `bt_rpc_send_bytes_callback` hold
    `current_profile_mutex` across `aci_gatt_update_*` → `hci_send_req`,
    which blocks on `hci_sem` released only by the BleEventWorker thread;
    BleEventWorker blocks on `current_profile_mutex` in
    `bt_on_gap_event_callback` (bt.c:358) while holding `gap->state_mutex`
    (gap.c:141) — circular wait). In
    `/Users/asutov/projects/flipperzero-firmware/applications/services/bt/bt_service/`:
    (a) `bt_i.h` — add `uint32_t current_profile_readers` to `Bt` (init 0).
    (b) `bt.c` — add helpers:
    `static FuriHalBleProfileBase* bt_current_profile_acquire(Bt* bt)`
    (mutex{ if current_profile: readers++, return copy } else return NULL)
    and `static void bt_current_profile_release(Bt* bt)`
    (mutex{ readers-- }), plus
    `static void bt_current_profile_wait_quiescent(Bt* bt)` (loop: mutex{
    n = readers } until 0, `furi_delay_ms(1)` between checks — bounded by the
    max aci call ~100 ms + margin; NEVER holds the mutex while waiting).
    (c) Convert every blocking profile user to acquire/release: `bt_serial_tx`
    (acquire; NULL→false; type-check the snapshot (pure comparison); call
    `ble_svc_airbridge_serial_update_tx`/`ble_profile_serial_tx` WITHOUT the
    mutex; release; return), `bt_rpc_send_bytes_callback` (per chunk:
    acquire + type-check + call, release BEFORE `furi_event_flag_wait`),
    `bt_serial_buffer_is_empty_callback` (same; keep the
    `BT_RPC_EVENT_BUFF_SENT` unblock-on-gone behavior), `bt_on_gap_event_callback`
    (acquire a reader ref at entry — brief mutex sections only — use the
    snapshot for ALL profile work, release at every exit; do NOT hold the
    mutex across `ble_profile_serial_set_event_callback`/`set_rpc_active`),
    `bt_close_rpc_connection` (acquire ref for the close sequence, release
    before returning; still called with no mutex held).
    (d) Writers: in `bt_change_profile` AND `bt_start_application`, call
    `bt_current_profile_wait_quiescent(bt)` AFTER publishing NULL and BEFORE
    `furi_hal_bt_change_app` (which frees the old profile). Document in
    comments: the mutex is only ever held for pointer/counter manipulation
    (µs), never across `hci_send_req`/`aci_*`/event waits — that is the
    deadlock-freedom invariant.
  - **References:** `bt.c:46-65` (bt_serial_tx), `:288-326` (RPC send),
    `:328-347` (buffer-empty), `:350-419` (GAP callback), `:501-517`
    (close_rpc), `:519-572` (change_profile), `:609-634` (start_application);
    deadlock evidence: `ble_app.c:136-139` (hci_sem wait),
    `hci_tl.c:275-281` (release on BleEventWorker), `gap.c:129-141,521-545`
    (state_mutex held across callback), `furi_hal_bt.c:203-240` (reinit holds
    core2_mtx across gap_thread_stop + hci_reset), `furi_hal_bt.c:156-164`
    (check_profile_type is pure).
  - **Acceptance:** no `current_profile_mutex` acquisition spans an
    `aci_*`/`hci_*`/event-wait/`rpc_session_close` call (grep-verified);
    `furi_hal_bt_check_profile_type` remains the only type test (pure); full
    `./fbt` build warning-clean; hardware: 30× navigation/deploy cycles
    (UP/DOWN/OK/BACK through Bridge↔DeployPrompt↔Typing, both transports)
    with ZERO wedges, plus 20× app exit/relaunch (soak C) — buttons always
    responsive, can always quit.
  - **QA:** happy — build log `/tmp/fw-build-t8.log`; failure — hardware
    wedge-hunt results in `/tmp/airbridge-qa-t8.log` (wedge count must be 0;
    if a wedge occurs, capture heartbeat state + DROP + whether USB unplug
    changes the glyph before resetting).
  - **Commit:** `fix(bt): refcount current_profile readers; never hold mutex across HCI calls`

- [~] 6. Hardware QA: AGENTS.md regression suite + executable freeze/teardown soaks
  - **What:** Run ALL previously-working flows FIRST (regression rule):
    bridge text both directions; file transfer SHA-256 both directions; USB
    deploy; BLE deploy; protocol harness 17/17. Then the soaks below. Because
    the FAP owns USB as an HID/vendor profile while it runs (no CDC serial),
    live USB CLI logging is unavailable during the app session. Capture
    timing/failure evidence by: (1) keeping the permanent debug logs from
    todos 1-2 minimal so they survive in the ring buffer, (2) exiting the app
    after each soak, restoring USB CDC, then immediately running the Flipper
    CLI `log` command to dump the post-exit ring buffer, and (3) optionally
    using UART/RTT if already configured. Playwright window for chat pages
    (`http://localhost:8081/`) and `https://blank.org`; `question`-tool gates
    with attention signal for every physical action; check-before-gating and
    open-before-gating per AGENTS.md.
    SOAK A (valid button sequence during BLE transfer): start a 20 KB
    attachment PC-B→PC-A (BLE→USB direction exercises both relay paths). Run
    exactly: `UP` -> expect USB DeployPrompt -> `BACK` -> expect Bridge;
    `DOWN` -> expect BLE DeployPrompt -> `BACK` -> expect Bridge; `OK` while
    on Bridge -> expect NO screen change (OK is ignored on Bridge in current
    code). Repeat the three checks 3× at ~1 check/s. PASS: every expected
    screen effect occurs, every BACK returns/continues toward Bridge, and
    non-BACK behavior matches the exact current state machine. EVIDENCE: QA
    table (input -> expected screen -> observed screen); `DROP` counter
    before/after noted, with BACK-related DROP treated as failure and other
    DROP compared against the pre-fix baseline.
    SOAK B (abort under congestion): start BLE Deploy typing of
    `bootstrap-ble.js`; DURING typing, degrade the link (move the Flipper
    ~5 m from PC-B behind a wall, or wrap in foil briefly — physical gate via
    `question` tool) until retry warnings are likely, then press BACK exactly
    once. PASS: one `BACK enqueued` / `BACK handled` log pair exists in the
    post-exit ring-buffer dump (delta = abort latency; record the number —
    informational bound: typical 20–50 ms, worst ~1.2 s under sustained gatt
    storm), no "KEYBOARD SEND ERROR" screen, no "Deploy error" screen, and
    within the session release-all either succeeds (no `release-all FAILED`
    log) or fails with the explicit log and target key-state check showing no
    stuck modifier. EVIDENCE: post-exit log excerpt (or UART/RTT live log if
    used) with the BACK pair + screen state after abort.
    SOAK C (teardown): with an active BLE central connected (PC-B chat page
    connected), exit the app via BACK; relaunch; repeat 20×. PASS: 20/20
    clean exits — heartbeat LED stops, app list responsive, no frozen screen,
    no reboot/crash. EVIDENCE: cycle-count table + post-exit log tail (or
    UART/RTT if used) checked for `furi_crash`/assert.
  - **References:** AGENTS.md "Test Surface and Gate Discipline";
    `web/protocol-harness.html`; `web/chat-usb.html`; `web/chat-ble.html`;
    BACK timing logs added in todo 1; retry/failure log strings from todo 2.
  - **Acceptance:** all regression flows pass as before; soak A/B/C pass
    with the PASS criteria and evidence formats above; abort latency numbers
    recorded from post-exit log deltas (informational, not hard gates beyond
    the ~1.2 s bound); `DROP`/`TXERR` unchanged vs pre-fix baseline in
    healthy transfer; BACK never causes DROP.
  - **QA:** this todo IS the QA wave; evidence: `/tmp/airbridge-qa-t6.log`
    plus post-exit log excerpts (or UART/RTT capture if used) and harness
    screenshot.
  - **Commit:** none (verification step).

- [x] 7. Regenerate firmware bundle in flipper-hid and sync docs
  - **What:** Regenerate `flipper-hid/firmware/` (patches + `pocket_airbridge/`
    copy + `firmware/README.md` regeneration note with new firmware commit
    hash) from the fixed `flipperzero-firmware` tree, following the existing
    regeneration workflow documented in `firmware/README.md` header. Update
    `docs/` only if observable behavior changed (expected: no protocol/
    behavior change; note the stability fixes in README regeneration line
    only).
  - **References:** `flipper-hid/firmware/README.md` (regeneration workflow),
    AGENTS.md repository layout.
  - **Acceptance:** `diff` of bundle `pocket_airbridge.c` vs canonical copy
    is empty; `git apply --check` of regenerated patches succeeds against
    pristine base `c9ab2b68`; README states the new firmware commit hash.
  - **QA:** happy — diff-empty + apply-check output saved to
    `/tmp/bundle-check-t7.log`. failure — any divergence aborts the todo;
    re-copy from canonical.
  - **Commit:** `chore(firmware): regenerate bundle with stability fixes`

## Final verification wave

- [~] F1. Plan compliance audit — independent agent re-reads this plan and
  diffs it against the landed diffs in both repos; every todo's acceptance
  criteria mapped to evidence; any unplanned change flagged.
- [x] F2. Code quality review — independent agent reviews the final
  `pocket_airbridge.c` and `bt.c`/`bt_api.c`/`bt_i.h` diffs for correctness
  (re-entrancy of input servicing, teardown ordering, ownership invariant),
  style consistency with surrounding code, and comment accuracy.
- [~] F3. Real manual QA — replay of todo 6 evidence: regression suite +
  freeze-soak A/B + 20-cycle teardown-soak logs exist, counters clean, and at
  least one full deploy flow (USB or BLE) re-run cold after a device reboot.
- [x] F4. Scope fidelity — confirm no out-of-scope edits (web/, protocol,
  UUIDs, other firmware files beyond bt.c/bt_api.c/bt_i.h, no worker-thread
  refactor); bundle regeneration contains only the intended changes.

## Commit strategy

Commits land in TWO repos:
1. `flipperzero-firmware` (canonical): one commit per todo 1-4 as listed on
   each todo's Commit line, on the project's working branch; firmware commit
   hash recorded for the bundle README.
2. `flipper-hid`: one commit for todo 7 (bundle regeneration + README hash
   note). No commits for deploy/QA todos (5, 6).
Each commit message prefixed per the todo's Commit line; no squashing across
repos; hardware QA evidence referenced in the todo-7 commit body.

## Success criteria

1. Relay traffic can never consume input queue capacity; BACK is
   semantically lossless via dedicated BACK queue + coalescing even under
   queue overflow;
   every valid input in the soak-A sequence produces the expected state-machine
   effect (freeze-soak A/B pass).
2. BACK aborts typing/streaming at the next retry boundary of the in-flight
   operation — typical 20–50 ms, worst ~1.2 s for one in-flight gatt retry
   storm (typing), ~100 ms (streaming) — with no spurious error screen; the
   release-all path adds zero abort latency (async, up to 3 attempts, failure
   logged; stuck-modifier avoidance is best-effort, verified in soak B).
3. 20/20 clean app exits under an active BLE link; no `furi_crash`/hang from
   the profile-teardown path; `bt->current_profile` single-writer + mutex
   lifetime invariant holds.
4. All pre-existing flows (AGENTS.md regression rule) still pass; `DROP` and
   `TXERR` remain 0 in healthy baseline transfers; BACK never increments
   `DROP`, while non-BACK overflow is recorded but not a failure outside the
   scripted soak.
5. `flipper-hid/firmware/` bundle regenerated, diff-clean vs canonical,
   patches apply to pristine base.

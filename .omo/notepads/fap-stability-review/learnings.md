# fap-stability-review — Learnings

## Todo 1 — dedicated input queue, lossless ordered BACK, drained first each loop (2026-07-26)

### What changed (only `applications_user/pocket_airbridge/pocket_airbridge.c`)
- `BridgeEvent` gained `uint32_t tick` + `uint32_t sequence` (stamped in
  `input_callback` at enqueue: `tick = furi_get_tick()`,
  `sequence = app->next_input_sequence++`).
- `AirbridgeApp` gained `input_queue`, `back_queue` (both
  `furi_message_queue_alloc(4, sizeof(BridgeEvent))`), `next_input_sequence`,
  `bool running`, and cached-head slots `have_back_head`/`back_head`/
  `have_input_head`/`input_head` (furi_message_queue has NO peek).
- `input_callback`: context is now `app` (was the bare shared queue). Back=Press /
  other=Short filter unchanged. BACK -> `back_queue`; put failure = coalesce
  (NO `dropped++`, NO eviction, `FURI_LOG_D(TAG, "BACK coalesced")`), plus
  `FURI_LOG_D(TAG, "BACK enqueued %lu", furi_get_tick())` on every accepted BACK.
  Non-BACK -> `input_queue`; put failure keeps the old `dropped++`.
- New `app_service_input(app)` after `app_handle_input` (forward-declared above
  `app_ble_kb_report_with_retry` for todo 2): merges queue heads by
  `(tick, sequence)` lexicographic — lower tick wins, same tick -> lower
  sequence wins — using the cached-head slots; calls
  `app_handle_input(app, key, &app->running)`; logs
  `FURI_LOG_D(TAG, "BACK handled %lu", furi_get_tick())` after each BACK.
- Main loop: `app_service_input(app)` runs FIRST every iteration, then
  `furi_message_queue_get(app->event_queue, &be, 10)` handles RELAY/USB only;
  dead `EVENT_TYPE_INPUT` dispatch branch removed; `bool running` local moved
  to `app->running` (loop condition + BLE-config-failure path updated).
- Teardown frees `input_queue` and `back_queue` before `event_queue`.

### Build result
- `./fbt build APPSRC=applications_user/pocket_airbridge` — SUCCESS,
  0 warnings, 0 errors. Log: `/tmp/fap-build-t1.log`.
  Artifact: `build/f7-firmware-D/.extapps/pocket_airbridge.fap` (22476 B).

### Deviations / notes
- `applications_user/` is gitignored in the firmware repo
  (`applications_user/.gitignore:1:*`), so the file needed `git add -f`.
  The file was never tracked before; the todo-1 commit introduces it as a
  new file containing the full (already todo-1-patched) source.
  `application.fam` / `icon.png` remain untracked per "commit ONLY this file".
- Creating the commit is DENIED by an environment permission rule in this
  session; the file is staged (`A applications_user/pocket_airbridge/pocket_airbridge.c`)
  and the commit must be created by the orchestrator/user with message:
  `fix(fap): dedicated input queue, drain before relay queue`
  in repo `/Users/asutov/projects/flipperzero-firmware`.
- Kept `.type = EVENT_TYPE_INPUT` on queued input events and the
  `EVENT_TYPE_INPUT` define — harmless, preserves BridgeEvent semantics; the
  merge helper dispatches on queue identity, not `type`.
- `app_service_input` drains ALL queued input even after `app->running`
  goes false mid-drain (extra BACK/OK on Bridge screen are no-ops) — matches
  plan wording "call app_handle_input per merged event".

### Risks for later todos
- Todos 2/3 must call `app_service_input` only from the retry helper, stream
  spin, and main loop (plan no-recursion criterion); the forward declaration
  at ~line 167 is already in place.
- Input latency when the loop blocks on the relay queue: up to 10 ms (queue
  get timeout) — same as before the split.

## Todo 4 — serialize `bt->current_profile` lifetime in bt_service (2026-07-26)

### What changed (only `bt.c`, `bt_api.c`, `bt_i.h` in `applications/services/bt/bt_service/`)
- `bt_i.h`: `Bt` gained `FuriMutex* current_profile_mutex` plus cached type
  flags `bool current_profile_is_serial` / `bool current_profile_is_airbridge`
  (written only under the mutex).
- `bt_api.c`: caller-thread writes DELETED — `bt_profile_start` no longer does
  `bt->current_profile = profile_instance;` (instance already returned via
  `BtMessage.profile_instance` written by `bt_change_profile`); 
  `bt_profile_restore_default` is now
  `return bt_profile_start(bt, ble_profile_serial, NULL) != NULL;`.
- `bt.c`: new helper `bt_publish_current_profile(bt, profile)` (sets pointer +
  both cached flags; always called with the mutex held, BtSrv thread only).
  The only remaining writes to `bt->current_profile` in the whole tree are
  inside that helper and the `bt_alloc` NULL init (mutex allocated first in
  `bt_alloc`, before the init).

### Writer inventory (all on the BtSrv thread after this change)
- `bt_change_profile`: `bt_close_rpc_connection` FIRST (no mutex), keys load,
  mutex{publish NULL}, `furi_hal_bt_change_app` (NO mutex — its
  `furi_hal_bt_stop_advertising` waits on GAP-thread progress, and GAP stop
  invokes `bt_on_gap_event_callback` which takes the mutex -> would deadlock),
  mutex{publish new}. Post-publish logic uses the `new_profile` local.
- `bt_load_keys` (`bt.c:522` old): NOT startup-only — reachable at runtime via
  `BtMessageTypeReloadKeysSettings` enqueued by `bt_storage_callback` on SD
  card mount — so the NULL publish is mutex-wrapped (code comment records the
  code-order evidence).
- `bt_start_application` (`bt.c:529` old): same runtime reachability, so:
  mutex{check NULL}, `furi_hal_bt_change_app` (no mutex; pointer stays NULL so
  readers bail), mutex{publish}. Check cannot go stale — BtSrv is the sole writer.

### Reader inventory (grep-verified, all covered)
- `bt_serial_tx`: mutex held across pointer snapshot AND the short profile
  call (`ble_profile_serial_tx` is a single non-blocking aci call;
  `ble_svc_airbridge_serial_update_tx` spins <= 100x1 ms; both depend only on
  the HCI transport thread, never on bt.c callbacks -> no lock cycle; writer's
  NULL publish just waits out the call).
- `bt_on_gap_event_callback`: entry snapshot under mutex (pointer + both
  booleans); the 4 profile uses (old 336/338/362/365) use the snapshot local.
- `bt_rpc_send_bytes_callback`: per-chunk mutex{ type-check + `ble_profile_serial_tx` },
  release BEFORE `furi_event_flag_wait`; NULL/wrong type -> log once + return
  (void callback, no furi_check, no boolean return added).
- `bt_serial_buffer_is_empty_callback`: mutex{ type-check +
  `ble_profile_serial_notify_buffer_is_empty` }; on failure log once, set
  `BT_RPC_EVENT_BUFF_SENT` to unblock any RPC sender, return.
- `bt_close_rpc_connection`: snapshot + type-check under mutex, RELEASE before
  `rpc_session_close`, final `ble_profile_serial_set_event_callback` uses the
  snapshot local (valid: runs only on BtSrv, the sole writer). Close still
  sees the old pointer (it runs before the NULL publish).
- `bt_serial_event_callback` (DEVIATION): reads the CACHED flags, never the
  mutex — see lock ordering.

### Lock ordering (deadlock-freedom argument)
- Both serial services (stock `serial_service.c:94-114` and
  `airbridge_serial_service.c:99-121`) hold their `buff_size_mtx` WHILE
  invoking `bt_serial_event_callback` on the DataReceived path. That file is
  off-limits, so the callback must never acquire `current_profile_mutex`:
  otherwise `buff_size_mtx -> current_profile_mutex` (DataReceived) vs
  `current_profile_mutex -> buff_size_mtx` (`bt_serial_buffer_is_empty_callback`
  -> `ble_svc_serial_notify_buffer_is_empty`, which takes `buff_size_mtx`) is
  an ABBA deadlock. Cached flags break the cycle; the only lock edge is
  `current_profile_mutex -> buff_size_mtx`. Stale-flag reads during a profile
  change are benign (link is being torn down; no pointer is dereferenced).
- Mutex is never held across: `rpc_session_close`, `furi_hal_bt_change_app`,
  `furi_event_flag_wait`, raw-serial callbacks, queue puts.
- `rpc_session_close` (rpc.c:423-431) unregisters callbacks + signals the
  worker WITHOUT joining it -> in-flight callbacks can outlive it; that is why
  the void RPC callbacks guard instead of furi_check.

### Build result
- Full `./fbt` in `/Users/asutov/projects/flipperzero-firmware` — SUCCESS,
  exit 0, 0 warnings, 0 errors (`grep -ci warning` = 0). Log: `/tmp/fw-build-t4.log`.
  firmware.bin 197 flash pages, 237.02 K free flash.

### Deviations / notes
- `bt_serial_event_callback` uses writer-maintained cached type flags instead
  of the plan's literal "booleans computed under the mutex" pattern — forced
  by the plan's own deadlock rule (ABBA via `buff_size_mtx`, above). All other
  readers follow the plan pattern exactly.
- No `furi_mutex_free`: there is NO Bt teardown path in this tree (bt_srv
  never exits; the special-boot-mode path suspends the thread and keeps `bt`
  alive via `furi_record_create(RECORD_BT, bt)`). The mutex lives for the
  service lifetime like the Bt object itself.
- `bt_rpc_send_bytes_callback`'s if/else chunk split was merged into one
  `bytes_to_send` ternary so the mutex guard exists in one place; behavior
  identical.
- Residual (pre-existing, NOT introduced, plan-accepted): a GAP callback that
  snapshotted a non-NULL profile and read `bt->rpc_session` non-NULL can
  theoretically still deref the old profile at old lines 361/365 if BtSrv
  completes an entire reinit in between (profile stop at furi_hal_bt.c:212
  precedes `gap_thread_stop` at :220). The NULL-publish-before-reinit shrinks
  this window vs upstream; closing it fully would require holding the mutex
  across `rpc_session_close` (forbidden).
- Commit DENIED by environment permission rule (same as todo 1). The three
  files are STAGED (`M bt.c`, `M bt_api.c`, `M bt_i.h`); the todo-1 worker's
  `A applications_user/pocket_airbridge/pocket_airbridge.c` is also staged and
  must NOT be swept in. Orchestrator/user must run, in
  `/Users/asutov/projects/flipperzero-firmware`:
  `git commit -m "fix(bt): serialize current_profile lifetime during profile changes" -- applications/services/bt/bt_service/bt.c applications/services/bt/bt_service/bt_api.c applications/services/bt/bt_service/bt_i.h`

### Risks for later todos
- Todo 5 flashes this firmware: post-flash RPC round-trip
  (`scripts/storage.py list /ext`) exercises `bt_rpc_send_bytes_callback` +
  `bt_close_rpc_connection`; then the launch-AirBridge/restore-default x20
  stress exercises `bt_change_profile` under an active RPC session.
- `bt_serial_tx` and the raw-serial airbridge hook are behaviorally unchanged
  on happy paths (same call order, same return semantics; NULL profile ->
  false, as before).

## Todo 2 — abort-aware BLE retry loops + non-blocking release-all (2026-07-26)

### What changed (only `applications_user/pocket_airbridge/pocket_airbridge.c`)
- Struct: `uint32_t typing_generation` (line 120), `bool release_all_pending`
  + `uint8_t release_all_attempts` (123-124).
- (a) `app_ble_kb_report_with_retry` (176): signature now
  `(AirbridgeApp* app, uint8_t* report)`, uses `app->ble_profile` internally;
  captures `generation = app->typing_generation` at entry; EVERY attempt opens
  with `app_service_input(app)` then returns false if the screen left Typing
  or the generation changed; the identical check repeats immediately before
  the send (closes the queued `BACK -> DOWN -> OK` race where a NEW session
  reuses `AirbridgeScreenTyping`). Call sites `app_typing_press` /
  `app_typing_release` (624/633) pass `app`.
- (b) `app_abort_typing` (225): calls NO report function at all. Bumps
  `typing_generation`, clears `typing_key_down` / `typing_enter_pending` /
  `typing_enter_done`, `app_set_hids_adv(app, false)` for BLE, and — if a key
  was down — latches `release_all_pending` (BLE; latched while the aborted
  session's transport is known) or keeps the synchronous
  `furi_hal_hid_airbridge_kb_release_all()` (USB, ISR-driven). Main loop
  (1372-1384, BEFORE the screen dispatch): when pending &&
  `ble_profile != NULL`, ONE direct `ble_profile_airbridge_kb_report`
  zero-report (no retry wrapper); success clears the flag + resets attempts;
  after 3 failures clears the flag +
  `FURI_LOG_E(TAG, "release-all FAILED - tap a key on target")`.
  `app_start_typing` (591-596): clears pending (+attempts) and logs
  "release-all superseded by new typing session" BEFORE anything else (HID
  reports are absolute state; the new session self-heals). Teardown
  (1448-1455, immediately before `app_restore_ble` NULLs `ble_profile`):
  drains a still-pending release-all with up to 3 direct attempts 20 ms apart.
- (c) `app_typing_step`: `step_generation` captured before each of the three
  press/release calls (649/683/701); at every "KEYBOARD SEND ERROR" site the
  staleness check `screen != Typing || typing_generation != step_generation`
  returns first — on the release path strictly BEFORE `app_abort_typing`, so
  an old step's false return can neither error nor abort the new generation.
- (d) `app_stream_step_ble` spin (852-867): `app_service_input(app);
  furi_delay_ms(2);` per iteration; screen != Streaming after servicing
  returns immediately WITHOUT touching `stream_tx_strikes`.
  `BLE_STREAM_RETRY_MAX` stays 20.

### Build result
- `./fbt build APPSRC=applications_user/pocket_airbridge` — SUCCESS, exit 0,
  0 warnings, 0 errors (`grep -ci "warning\|error"` = 0). Log:
  `/tmp/fap-build-t2.log`. Artifact:
  `build/f7-firmware-D/.extapps/pocket_airbridge.fap` (23116 B; was 22476 B
  after todo 1).

### Deviations / notes
- `app_start_typing` also resets `release_all_attempts` when superseding (plan
  said only "clear it and log"): the counter belongs to the pending episode —
  a stale count would let a later episode exhaust the 3-attempt budget early.
- The plan's "check the generation again immediately before sending" produced
  a second, currently-adjacent check right after the post-service check
  (nothing intervenes in the present loop shape) — kept verbatim per plan; it
  future-proofs anything later inserted between service and send.
- No-recursion criterion verified by grep: `app_service_input` is called from
  exactly three sites — retry helper (182), stream spin (858), main loop
  (1334). `app_abort_typing` contains zero report calls.
- Mid-edit accident (self-inflicted, fixed same pass): a botched Edit
  oldString briefly removed the Streaming/Waiting dispatch branches in the
  main loop; restored immediately and verified by re-reading 1366-1395 plus a
  clean build. Final file is correct.
- Git: commit still permission-blocked. Nothing newly staged by this worker;
  `git status --short` shows `AM applications_user/pocket_airbridge/
  pocket_airbridge.c` (todo-1 `A` snapshot + todo-2 working-tree `M`) and the
  todo-4 `M` bt_service trio, all untouched. Orchestrator batches commits.

### Risks for later todos
- Todo 3 touches teardown/volatile flags in the same file: the teardown drain
  block sits immediately before `app_restore_ble` (clean seam);
  `ble_connected` / `usb_connected` are still non-volatile (todo 3 marks them).
- Hardware QA (todo 6 soak B): BACK mid-typing under congestion must abort
  with NO "KEYBOARD SEND ERROR"; release-all either lands within 3 loop
   attempts or logs "release-all FAILED - tap a key on target"; check the
   target PC for stuck modifiers after aborts.

## Todo 3 — bounded BLE teardown wait, OOM comment, minor correctness (2026-07-26)

### What changed (only `applications_user/pocket_airbridge/pocket_airbridge.c`)
- (a) `app_restore_ble` (490-492): fixed `furi_delay_ms(200)` after
  `bt_disconnect` replaced with the plan's bounded wait —
  `uint32_t start = furi_get_tick(); while(app->ble_connected && (furi_get_tick() - start < 500)) furi_delay_ms(10);`
  Zero wait when already disconnected (NO 200 ms floor). Comment above
  rewritten: the wait gives BleEventWorker time to finish processing the
  disconnect event (500 ms fallback); `ble_connected` is only a proxy —
  GapEventTypeDisconnected (bt.c:408-424, verified against post-todo-4 bt.c)
  does NOT fire the status callback; the callback fires via the subsequent
  StopAdvertising path (bt.c:429-432 -> do_update_status ->
  BtMessageTypeUpdateStatus -> bt.c:707-713 status_changed_cb with
  BtStatusOff). If the link dropped while GAP was idle, no status update
  fires and the full 500 ms elapses — safe, the HCI event has long been
  processed by then. Stale references updated: old "bt.c:339-344" and the
  "Mirrors the stock hid_app exit … + 200 ms" tail dropped (todo 4 shifted
  bt.c line numbers).
- (b) OOM comment at the startup alloc block (1325-1330): states OOM is
  unrecoverable by firmware design (view_port_alloc/storage_file_alloc deref
  their malloc before returning; furi_message_queue_alloc furi_checks
  internally), so no caller-side NULL checks are added — they would be dead
  code. App-struct malloc NULL check (1313) kept. Zero new alloc checks.
- (c) `volatile bool ble_connected` (struct, 127 — written on the BtSrv
  thread via app_ble_status_changed_callback, read on main + GUI threads) and
  `static volatile bool usb_connected` (139 — written by the main loop, read
  by the GUI render thread). One-line rationale comment on each.
- (d) `ble_raw_serial_callback` credit return UNCHANGED
  (`return HID_VENDOR_PACKET_LEN;` at 922) — intentionally out of scope.

### Signal-path detail (verified in current bt.c, post-todo-4)
- GapEventTypeDisconnected branch (bt.c:408-424) sets NO `do_update_status` —
  it only clears airbridge-serial callbacks / the RPC session.
- Status callback fires only via `do_update_status` ->
  BtMessageTypeUpdateStatus (bt.c:454) -> BtSrv loop (bt.c:707-713).
  Disconnect -> GAP stops advertising -> GapEventTypeStopAdvertising sets
  BtStatusOff (bt.c:429-432) -> FAP callback sees non-Connected ->
  ble_connected=false + restarts advertising (-> GapEventTypeStartAdvertising
  -> BtStatusAdvertising).
- The status callback is cleared AFTER `bt_profile_restore_default` in
  `app_restore_ble`, so it can still fire during the bounded wait — the wait
  is meaningful. No locks in the FAP wait loop -> no deadlock.

### Build result
- `./fbt build APPSRC=applications_user/pocket_airbridge` — SUCCESS, exit 0,
  0 warnings, 0 errors (`grep -ci "warning\|error"` = 0). Log:
  `/tmp/fap-build-t3.log`. Artifact:
  `build/f7-firmware-D/.extapps/pocket_airbridge.fap` (23156 B; was 23116 B
  after todo 2).

### Deviations / notes
- None vs plan. The wait loop is the plan's exact one-line form (no braces);
  line length (87 cols) matches nearby long lines (e.g. the retry-helper
  staleness checks).
- Todos 1-2 preserved by inspection: release-all teardown drain (1461-1472)
  sits immediately before `app_restore_ble` (1473); input/back queues,
  generation guard, async release-all paths untouched.
- Git: commit still permission-blocked; nothing staged by this worker.
  Orchestrator batches with plan message:
  `fix(fap): bounded disconnect wait at teardown, volatile cross-thread flags`

### Risks for later todos
- Todo 6 teardown-soak: with an active central, exit should observe
  `ble_connected` false well inside the 500 ms bound (GAP stop-advertising
  turnaround is fast); a full-500 ms exit means the link dropped while GAP
  was idle — safe per the comment, just a slower exit.
- Todo 5: tree left buildable; this file plus todo-4 firmware are the inputs.


## Todo 5 — build, flash firmware, deploy FAP to device (2026-07-26)

### Environment note
- The `question` tool referenced by AGENTS.md is NOT exposed in this session's
  toolset (no question-tool MCP either). Physical gates were handled as
  end-of-turn direct user prompts, each preceded by the `afplay Funk.aiff` x2
  attention signal, with check-before-gating done programmatically first.
  Gates used: (1) plug-in/unlock/exit-app preflight, (2) post-launch screen +
  LED confirmation. Both passed on first reply.

### Build (pre-flash)
- Full `./fbt`: no-op rebuild, API 90.1 up to date, exit 0, 0 warnings/errors —
  confirms the todo-4 full build was still current (no firmware sources changed
  since; FAP changes don't affect the firmware image).
- `./fbt build APPSRC=applications_user/pocket_airbridge`: no-op, exit 0,
  0 warnings/errors (log `/tmp/fap-build-t5.log`). Artifact
  `build/f7-firmware-D/.extapps/pocket_airbridge.fap` = **23156 B**, identical
  size to the todo-3 verified build.

### Flash
- Method: `./fbt flash_usb` (the documented method in
  `docs/firmware-guide.md` — self-update package uploaded over serial CLI, no
  DFU, no physical buttons). Uploaded `firmware.dfu` (99 chunks),
  `update.fuf`, `updater.bin`; exit 0.
- Serial port returned ~15 s after trigger; `flipper_alive.py` -> ALIVE.
  Device booted normally on the new firmware. No DFU/recovery needed.

### Post-flash RPC round-trip (todo-4 acceptance)
- `scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 list /ext` -> exit 0,
  full SD tree returned. Stock RPC intact after the `current_profile`
  hardening (exercises `bt_rpc_send_bytes_callback` + session close paths
  over the serial RPC transport).

### FAP deploy
- Pre-deploy assertion `storage.py list /ext/apps | grep -i airbridge`:
  exactly one line, `/ext/apps/USB/pocket_airbridge.fap, size 21856b` (old
  build at the canonical path; no strays anywhere under /ext/apps — the
  listing is recursive).
- `storage.py send -f` -> exit 0 (3 chunks).
- `storage.py size /ext/apps/USB/pocket_airbridge.fap` -> **23156** — matches
  the build artifact exactly.
- Post-deploy assertion: exactly one line,
  `/ext/apps/USB/pocket_airbridge.fap, size 23156b`.

### Launch + verification
- `scripts/runfap.py -p ... -s build/.../pocket_airbridge.fap -t /ext/apps/USB/pocket_airbridge.fap`
  -> exit 0 ("Launching app"). NOTE: the documented terminal
  `Device not configured` error did NOT appear this time — the script's reads
  finished before the USB switch. Benign either way.
- Programmatic launch evidence: CDC serial port disappeared (app owns USB);
  `ioreg` shows the hp_kbd_vendor impersonation enumerating — "HP Wireless
  Keyboard and Mouse", VID 0x03F0 (1008), PID 0x5341 (21313). No STM32 VID
  0x0483 on the bus.
- Physical gate: user CONFIRMED the on-device "Pocket AirBridge" screen and
  the green LED heartbeat (~500 ms).

### Final state / handoff to todo 6
- Device runs the todo-4 firmware (bt_service `current_profile` hardening) and
  the todos 1-3 FAP (input/back queues, generation-guarded retries, async
  release-all, bounded teardown). **App left RUNNING** per todo-6 dependency.
- For todo 6 storage/RPC/CLI operations (e.g. post-exit `log` ring-buffer
  dumps): exit the app via BACK first (physical gate), serial port returns
  automatically.
- Evidence log: `/tmp/airbridge-deploy-t5.log` (14 sections, all exit 0).

### Risks for later todos
- None new. Pre-existing: `runfap.py`'s missing `Device not configured` line
  is timing-dependent, not a pass/fail signal — treat ioreg impersonation +
  screen as the launch evidence.

## Todo 8 — refcount current_profile readers; never hold mutex across HCI calls (2026-07-27)

### Root cause (regression introduced by todo 4, confirmed, not re-litigated)
- `bt_serial_tx` and `bt_rpc_send_bytes_callback` held
  `current_profile_mutex` across `aci_gatt_update_char_value[_ext]` ->
  `hci_send_req`, which blocks on `hci_sem` (ble_app.c:136-139) released only
  by the BleEventWorker thread (hci_tl.c:275-281). BleEventWorker blocked on
  the same mutex in `bt_on_gap_event_callback` while holding
  `gap->state_mutex` (gap.c:129-141) — circular wait. Hardware wedge on
  navigation/deploy/exit, random but often.

### What changed (only `bt.c`, `bt_i.h` in `applications/services/bt/bt_service/`)
- `bt_i.h`: mutex comment rewritten to state the deadlock-freedom invariant
  (mutex held only for pointer/counter manipulation, microseconds); added
  `uint32_t current_profile_readers` (line 91).
- `bt.c` helpers (52-83): `bt_current_profile_acquire` (mutex{ if profile:
  readers++, return copy } else NULL), `bt_current_profile_release`
  (mutex{ furi_assert(readers>0); readers-- }),
  `bt_current_profile_wait_quiescent` (poll counter under the mutex,
  `furi_delay_ms(1)` AFTER release, never across the delay).
- `bt_alloc`: `bt->current_profile_readers = 0;` (line 247) with the other
  profile-state inits.
- Acquire/release conversions (mutex never spans a blocking call):
  - `bt_serial_tx` (90-108): acquire 97, pure snapshot type check, airbridge
    `ble_svc_airbridge_serial_update_tx` / stock `ble_profile_serial_tx`
    WITHOUT the mutex, release 105. NULL profile -> false preserved.
  - `bt_rpc_send_bytes_callback` (342-371): per chunk acquire 349 +
    `furi_hal_bt_check_profile_type` + `ble_profile_serial_tx`, release 355
    BEFORE `furi_event_flag_wait` (363); guarded log-once abort kept
    (357-360).
  - `bt_serial_buffer_is_empty_callback` (374-395): acquire 381 + type-check
    + `ble_profile_serial_notify_buffer_is_empty`, release 387;
    `BT_RPC_EVENT_BUFF_SENT` unblock-on-gone kept (390-394).
  - `bt_on_gap_event_callback` (398-514): ref at entry (411), snapshot
    pointer + cached type booleans for ALL profile work including
    `ble_profile_serial_set_event_callback`/`set_rpc_active` (442-445,
    468-472, called with NO mutex, protected by the held ref), release at the
    single exit (510-512). `rpc_session_close` (471) runs under the ref, not
    the mutex. NO trylock — every GAP event still handled.
  - `bt_close_rpc_connection` (555-573): acquire 560, close sequence incl.
    `rpc_session_close` (566) under the ref, release 570-572 before return;
    still called with no mutex held (BtSrv thread).
- Writers: `bt_change_profile` — mutex{publish NULL} 590-592, then
  `bt_current_profile_wait_quiescent` (594), then `furi_hal_bt_change_app`
  (596) frees the old profile; publish new 603-605. `bt_start_application` —
  pointer already NULL, so `bt_current_profile_wait_quiescent` (680) before
  `furi_hal_bt_change_app` (682); publish 689-691. `bt_load_keys` NULL
  publish (661-663) unchanged (no free there; the paired
  `bt_start_application` does the drain).
- `bt_serial_event_callback` untouched (mutex-free by todo-4 design via
  cached flags; ABBA via buff_size_mtx argument still holds).
- `bt_publish_current_profile` unchanged (called under mutex, BtSrv only).

### Build result
- First attempt FAILED (exit 2): the invariant comment contained
  `aci_*/hci_*` — the embedded `*/` terminated the block comment early,
  cascading parse errors. Fixed by removing asterisk-slash pairs and
  em-dashes (files now pure ASCII; verified `LC_ALL=C grep -c $'\xe2'` = 0
  and no `word*/` sequences). Gotcha worth remembering for any future
  comment mentioning glob-style `aci_*`/`hci_*` names.
- Full `./fbt` in `/Users/asutov/projects/flipperzero-firmware` — SUCCESS,
  exit 0, 0 warnings, 0 errors (`grep -ci "warning|error"` = 0). Log:
  `/tmp/fw-build-t8.log`. firmware.bin 197 flash pages, 236.91 K free flash.

### Deadlock-invariant proof (grep/awk, this tree)
- 8 `furi_mutex_acquire(bt->current_profile_mutex` sites remain: bt.c 53,
  63, 75 (the three helpers), 590, 603 (change_profile publishes), 661
  (load_keys publish), 670 (start_application NULL check), 689
  (start_application publish). awk scan: no
  `aci_`/`hci_`/`furi_event_flag_wait`/`rpc_session_close`/`furi_delay`
  between any acquire and its matching release (the only calls inside are
  `bt_publish_current_profile`/`furi_hal_bt_check_profile_type`/
  `bt_profile_is_airbridge`/`furi_assert` — all pure/non-blocking).
  `furi_delay_ms(1)` in wait_quiescent (line 81) is AFTER the release (77).
- Direct `bt->current_profile` pointer accesses exist only at: publish
  helper write (36), acquire helper read (54), `bt_alloc` init (246),
  `bt_start_application` NULL check (671). No stray readers.

### Deviations / notes
- None vs plan design. `bt_on_gap_event_callback` has exactly one exit, so
  "release at every exit path" is one release site (510-512).
- `bt_current_profile_release` carries `furi_assert(readers > 0)` —
  unbalanced release is a programmer error; codebase convention.
- Git: commit permission-blocked as before. `git status --short`:
  `MM bt.c`, `MM bt_i.h` (todo-4 staged snapshot + todo-8 unstaged working
  tree), `M bt_api.c` (todo-4, untouched). Nothing staged by this worker;
  the todo 1-3 `pocket_airbridge.c` unstaged diff is other workers' — left
  alone. Orchestrator batches with plan message:
  `fix(bt): refcount current_profile readers; never hold mutex across HCI calls`

### Risks for later todos
- Theoretical residual (accepted, flagged for todo-6 soak): the GAP callback
  holds its reader ref across `FuriWaitForever` message-queue puts (battery
  put in the Connected branch, UpdateStatus put at exit). If the 8-slot
  BtSrv queue were FULL while BtSrv spins in `wait_quiescent`, the put would
  block the ref release and vice versa. Requires ~8 pending producer events
  inside one profile-change window (the Disconnect branch — the common
  teardown event — does NO puts; the puts are pre-existing upstream
  behavior, unchanged). Not observed; watch for it in the 30x
  navigation/deploy + 20x exit/relaunch soaks.
- PinCodeVerify holds the ref across the modal user dialog: a concurrent
  profile change stalls until the user answers (liveness stall, not a
  deadlock — no lock held while waiting).
- After reflash (todo-5 worker pattern), hardware wedge-hunt evidence goes
  to `/tmp/airbridge-qa-t8.log`; wedge count must be 0 (if a wedge occurs,
  capture heartbeat state + DROP + whether USB unplug changes the glyph
  before resetting).

## Todo 8 reflash — current_profile_mutex/HCI deadlock fix on device (2026-07-27)

### Pre-flash state (check-before-gating; NO exit/reset gate needed)
- Serial port `/dev/cu.usbmodemflip_Luwot1` PRESENT and no HP impersonation on
  the USB tree -> the app was already exited (device at desktop with CDC).
- `flipper_alive.py` -> ALIVE; `storage.py list /ext` probe exit 0. Device was
  NOT wedged — the anticipated wedge-exit gate was skipped as already-true.

### Flash (todo-8 firmware: bt.c/bt_i.h reader-refcount fix)
- `./fbt flash_usb` exit 0 (firmware.dfu + update.fuf + updater.bin uploaded).
- Port returned ~15 s after trigger; `flipper_alive.py` -> ALIVE. Normal boot.
- Post-flash RPC round-trip `storage.py list /ext` -> exit 0, full tree:
  stock RPC intact on the todo-8 firmware.

### FAP assertion (unchanged since todo-5 deploy)
- `storage.py list /ext/apps | grep -i airbridge` -> exactly one line:
  `/ext/apps/USB/pocket_airbridge.fap, size 23156b`.
- `storage.py size` -> 23156 == build artifact. No redeploy (SD card survives
  flash; FAP source unchanged).

### Launch
- `runfap.py` exit 0; CDC port disappeared; ioreg shows HP Wireless Keyboard
  and Mouse VID 0x03F0 / PID 0x5341. App running on todo-8 firmware.
- Screen + heartbeat confirmation: end-of-turn gate (orchestrator relays via
  question tool). Evidence log: `/tmp/airbridge-deploy-t8.log`.

### Input-path instrumentation (wedge hunt, 2026-07-27, TEMPORARY)
- Added guarded diagnostics to `pocket_airbridge.c` behind `#define AIRBRIDGE_DEBUG_INPUT 1` (flag-off builds are byte-identical; delete the blocks to strip). Four counters: `dbg_events` (every input_callback call, GUI input thread, before the key filter), `dbg_back_events` (Back/Press passing the filter), `dbg_handled` (main thread, immediately before each app_handle_input), `dbg_loop_iters` (main thread, once per loop iteration). A `render_debug_input` helper draws `E<events> B<back> H<handled> L<loop> q<back,input,event queue depths>` right-aligned on the y=63 row of BOTH render_bridge and render_deploy_prompt; L is shown mod 1000 for fixed width (spinning low digits = loop alive). Layout note: the six fields cannot share the bottom row with the "BACK: ..." hint without overlap (FontSecondary ~6 px/char, 128 px screen, no free row exists), so in DEBUG builds the hint is suppressed and the debug line owns the row — discrimination readout: E frozen on keypress = delivery dead (GUI/input layer); E moving, H frozen = queue drain/merge dead (app_service_input); both moving = app_handle_input/state machine at fault. Build: exit 0, zero warnings (`/tmp/fap-build-dbg.log`), fap 23764b (+608b vs 23156b baseline). NOT deployed, NOT committed.

## Todo 9 — pubsub stall-tracer debug instrument in furi/core/pubsub.c (2026-07-27, TEMPORARY, DO NOT COMMIT)

### What changed (only `furi/core/pubsub.c`, unstaged)
- All instrument code guarded by `#define FURI_DEBUG_PUBSUB_STALL 1` at the top
  of the file; setting it to 0 restores byte-identical publish semantics.
- `furi_pubsub_publish` mutex acquire is now a 500 ms timeout slice first, then
  `FuriWaitForever` on timeout: a PERMANENTLY held mutex still logs
  `STALL mutex <ms>ms pubsub=<p>` (post-hoc timing alone could never print one).
  Acquire semantics unchanged — still waits until acquired.
- Each subscriber callback is timed; >500 ms logs
  `STALL cb index=<i> cb=<p> ctx=<p> pubsub=<p> <ms>ms`.
- NEW mechanism beyond the brief: a static RAM breadcrumb (`pubsub_stall_crumb`)
  is written before every callback (pending flag set last). Every publish on ANY
  thread sweeps it; a callback still inside after 500 ms logs
  `WEDGE cb index=<i> ... age=<ms>ms (callback still running)` from the sweeping
  thread. Rationale: an abrupt PERMANENT callback block (the prime suspect class
  — full-queue FuriWaitForever put) never returns, so post-hoc timing yields an
  empty log in exactly the scenario being hunted; the wedged thread can never
  report itself. The sweep is opportunistic (needs any publish on a live thread
  within the window: storage/power/loader/desktop/dolphin pubsubs).
- Rate limiting: one line per publish call (`stall_logged` flag), one line per
  pubsub pointer per 10 s (4-slot static table). One WEDGE line per wedged
  callback (`flushed` flag on the breadcrumb).
- Log helper: `furi_record_exists` guard, refuses to run on the `StorageSrv`
  thread (storage_file_* from the storage worker's own thread would
  self-deadlock on its message queue), then record-open / alloc / open
  `/ext/pubsub_stall.log` FSAM_WRITE|FSOM_OPEN_APPEND / write / close / free /
  record-close. All failures silently skipped. Static 128 B line buffer +
  busy flag (input service stack is only 1 KiB).
- Escalation switch `#define FURI_DEBUG_PUBSUB_STALL_ENTER 0` (default off):
  when set to 1, logs an `ENTER cb index=<i> ...` line before EVERY callback —
  guaranteed identification of an abrupt permanent block at the cost of heavy
  SD traffic during bursts. Use only if default instrumentation comes up empty.

### Build result
- Full `./fbt` in `/Users/asutov/projects/flipperzero-firmware` — SUCCESS,
  exit 0, 0 warnings, 0 errors (`grep -ci "warning|error"` = 0). Log:
  `/tmp/fw-build-stall.log`. Instrument strings verified present in
  `dist/f7-D/flipper-z-f7-firmware-local.elf` (STALL/WEDGE formats, log path).

### Log interpretation (for the post-wedge read of /ext/pubsub_stall.log)
- RECORD_INPUT_EVENTS subscription order at boot: notification=0, GUI=1,
  [desktop unsubscribes while a fullscreen app runs], FAP debug tap=last
  (index 2 while desktop is unsubscribed).
- `cb` in 0x08xxxxxx = firmware callback — match against the debug ELF symbol
  map (`build/f7-firmware-D/firmware.elf` / `dist` elf via addr2line or
  `arm-none-eabi-nm`). A RAM-range `cb` = the FAP's own tap (external apps are
  not in the firmware symbol map — that itself would be the answer).
- Only a `STALL mutex` line = block is on the pubsub mutex itself (someone
  holds it forever). A `STALL cb` line = slow-but-returning callback. A
  `WEDGE cb` line = callback never returned; index+cb identify the subscriber.
- Empty log after a real wedge = publish was never called again (input thread
  stuck BEFORE publish, not inside it) OR abrupt permanent callback block with
  no other-thread publish before reset — escalate with
  `FURI_DEBUG_PUBSUB_STALL_ENTER 1` and re-run.

### Concerns / notes
- Storage is called from the stalled thread (usually InputSrv) mid-stall:
  synchronous RPC to the storage thread, safe while storage is alive; happens
  only on >500 ms stalls (essentially once per wedge). If the wedge itself
  involves the storage thread, the helper silently yields no line.
- snprintf + storage chain on the 1 KiB InputSrv stack: line buffer is static;
  stall path adds ~300-400 B peak at a shallow call site — accepted residual
  risk for a debug build.
- Lock-free statics (breadcrumb, rate table, busy flag) can race; worst case
  is a skipped/duplicated/garbled debug line, never a crash.
- Git: NOT staged, NOT committed (task requirement). `git status --short`
  shows ` M furi/core/pubsub.c`; other staged/modified files (bt_service trio,
  pocket_airbridge.c) are other workers' — left untouched.

## Todo 9b — watchdog escalation of the pubsub stall-tracer (2026-07-30, TEMPORARY, DO NOT COMMIT)

### Why
- First capture: wedge reproduced, `/ext/pubsub_stall.log` EMPTY. No STALL
  mutex (not a mutex block), no STALL cb (blocked callback never returns), no
  WEDGE cb (the sweep needed a publish on another thread; the 10 idle seconds
  after the wedge had none). Conclusion: input service is blocked INSIDE a
  subscriber callback on a quiet system; the sweep-only design was blind.

### What changed (only `furi/core/pubsub.c`, still behind FURI_DEBUG_PUBSUB_STALL)
- 1 s periodic `furi_timer` (`pubsub_stall_watchdog`), allocated lazily on the
  first publish (double-checked pointer, loser frees). Its callback runs on
  `TimersSrv` — independent of the blocked input thread AND of any other
  publish happening — and calls the shared `pubsub_stall_wedge_check(now,
  2000 ms)`: state==ENTER (pending) and age>2000 ms -> one WEDGE line via the
  existing storage helper + 10 s rate limiter, then `flushed` (reported).
- Breadcrumb extended with `msg_words[3]` = raw first 12 bytes of the message;
  WEDGE line now ends ` msg=%08lx:%08lx:%08lx`. For input_events that is
  InputEvent{sequence, key, type}: key Up=0 Down=1 Right=2 Left=3 Ok=4 Back=5;
  type Press=0 Release=1 Short=2 Long=3 Repeat=4.
- CRITICAL correctness change: publishers now REFUSE to overwrite a
  still-pending crumb (`stall_crumb_mine` claim; pending cleared only by the
  owner). Without this, a publish on another thread between wedge and the
  watchdog's 2000 ms threshold would erase the evidence — the same hole the
  watchdog was added to close. Trade-off: a concurrent non-input publisher
  whose callback wedges while a foreign crumb is pending stays unnamed
  (accepted; the target wedge is the single-threaded input pubsub).
- Sweep refactored to call the shared wedge-check at 500 ms; all todo-9
  behaviors (mutex slice, STALL cb, ENTER escalation flag) unchanged.

### Build result
- Full `./fbt` — SUCCESS, exit 0, 0 warnings, 0 errors. Log:
  `/tmp/fw-build-stall2.log`. New WEDGE format + watchdog symbols
  (`pubsub_stall_watchdog_callback` etc.) verified in
  `dist/f7-D/flipper-z-f7-firmware-local.elf`.

### Interpretation (next capture)
- WEDGE line -> index names the subscriber (notification=0, GUI=1, tap=2),
  `cb` resolves via `arm-none-eabi-nm`/`addr2line` against the dist ELF (or a
  RAM-range cb = the FAP's tap); msg words give the wedging event (key/type
  decode above; expect the last-burst key, e.g. Press OK = key 4 type 0).
- STILL empty with a confirmed wedge -> the block is in input.c event
  PRODUCTION (GPIO scan), not in publish; escalate to input.c instrumentation.
- TimersSrv stack is 1 KiB (configTIMER_TASK_STACK_DEPTH 256 words); the
  watchdog keeps snprintf and the storage chain sequential (~400 B peak est).
  If the storage thread were dead, the helper would stall TimersSrv (frozen
  LED/animations would be the tell-tale) — not expected; storage was healthy.

### Concerns / notes
- Watchdog storage write blocks the shared timer task for its duration
  (~10-100 ms), once per wedge, debug build only — accepted.
- msg capture is 12 raw bytes for every pubsub type; sizes >= 8 B everywhere
  in-tree, a 4-byte over-read of a publisher's stack is harmless.
- Git: NOT staged, NOT committed. ` M furi/core/pubsub.c` only.

## Todo 9c — input.c stall tracer (2026-07-30, TEMPORARY, DO NOT COMMIT)

### Why
- Second capture: wedge reproduced in ~3 UP→BACK cycles, keys died, LED and
  L counter still alive (main/render threads OK), but `/ext/pubsub_stall.log`
  was still EMPTY. The pubsub watchdog confirmed NO subscriber callback is
  entered. Conclusion: the input thread itself is wedged BEFORE any publish.

### What changed (only `applications/services/input/input.c`, unstaged)
- New guarded block behind `#define FURI_DEBUG_INPUT_STALL 1` at the top.
- `volatile uint32_t input_loop_counter` incremented at the top of every
  `input_srv` loop iteration.
- 1 s periodic `furi_timer` (`input_stall_watchdog`) allocated and started
  lazily inside `input_srv` right after `furi_record_create`. Its callback runs
  on `TimersSrv` (independent of the input thread).
- Watchdog logic: on each tick compare `input_loop_counter` with the previous
  value; if it changes, record the new value + current tick and clear the
  `input_stall_reported` latch. If it is unchanged for >2000 ms, write ONE line
  to `/ext/pubsub_stall.log` and set the latch.
- Lines written:
  - With a breadcrumb available:
    `WEDGE input_thread counter=%lu tick=%lu key=%lu type=%lu key_tick=%lu`
  - Without a breadcrumb:
    `WEDGE input_thread counter=%lu tick=%lu`
- Breadcrumb: before every `furi_pubsub_publish` (Long/Repeat from the press
  timer, Short/Press/Release from the main loop) call
  `input_stall_breadcrumb(key, type)`, which stores {last_key, last_type,
  last_tick} plus a sequence counter for a stable snapshot in the watchdog.
- Stable-read: the watchdog reads `input_stall_breadcrumb_seq`, copies the
  three fields, then re-reads the sequence; if unchanged the breadcrumb is
  considered valid. This avoids torn reads from concurrent updates.
- `#else` stub defines `input_stall_breadcrumb` as a no-op so the four call
  sites compile cleanly when `FURI_DEBUG_INPUT_STALL` is 0.

### Build result
- Full `./fbt` — SUCCESS, exit 0, 0 warnings, 0 errors. Log:
  `/tmp/fw-build-input-stall.log`. Both WEDGE formats and watchdog symbols
  (`input_stall_watchdog_callback`) verified in
  `dist/f7-D/flipper-z-f7-firmware-local.elf`.

### Interpretation (next capture)
- A `WEDGE input_thread` line appears -> the input thread stopped looping.
  The `key`/`type`/`key_tick` fields identify the last event that made it to
  the dispatch path (before the wedge). Key enum: Up=0, Down=1, Right=2,
  Left=3, Ok=4, Back=5. Type enum: Press=0, Release=1, Short=2, Long=3,
  Repeat=4. Expect the last-burst event (e.g. UP→BACK cycle: likely Release
  Back or Release Up).
- The likely stall points in `input_srv` once a publish is ruled out:
  - `furi_thread_flags_wait(INPUT_THREAD_FLAG_ISR, ..., FuriWaitForever)`
    (line ~300) — GPIO ISR stopped firing.
  - `while(furi_timer_is_running(...)) furi_delay_tick(1);` (line ~272) —
    spin-waiting for the press timer to stop; if the timer task is wedged,
    this never exits.
- STILL no WEDGE line with a confirmed wedge -> the input thread is looping
  but not reaching the counter increment (impossible with this placement) or
  the watchdog/timer subsystem itself is dead. If that happens, the next
  escalation is GPIO/ISR-level instrumentation or hooking the timer stop loop.

### Concerns / notes
- Watchdog storage write runs on `TimersSrv` (1 KiB stack, 256 words) and
  briefly blocks the shared timer task during the write — once per wedge,
  debug build only. Same residual stack risk as the pubsub watchdog.
- `input_loop_counter` is incremented at the very top of the outer `while(1)`
  loop body, so any stall that prevents that loop from completing its current
  iteration and starting the next one — including the `furi_timer_is_running`
  spin-wait, the `FuriWaitForever` GPIO flag wait, or any hang in the dispatch
  path before publish — stops the counter and is detected by the watchdog.
- STILL no `WEDGE input_thread` line with a confirmed wedge would mean the
  input thread is still looping but somehow not dispatching (unlikely given
  the code shape) or the watchdog/timer subsystem itself is dead. If that
  happens, the next escalation is GPIO/ISR-level instrumentation.
- Git: NOT staged, NOT committed. `git status --short` now also shows
  ` M applications/services/input/input.c` in addition to the earlier files.
  No other files touched.

## Todo 9d — fix input-thread deadlock root cause (2026-07-30, DO NOT COMMIT YET)

### Root cause
- `gui_input_events_callback` in `applications/services/gui/gui.c:51` used
  `furi_message_queue_put(gui->input_queue, value, FuriWaitForever)`. Under
  rapid key presses the 8-entry GUI input queue fills, the GUI subscriber
  blocks inside `furi_pubsub_publish` while still holding the input pubsub
  mutex.
- The input thread's press timer callback (`input_press_timer_callback`)
  publishes Long/Repeat events to the same pubsub and blocks on that mutex.
- The timer task is now stuck inside the pubsub callback, `tmrSTATUS_IS_ACTIVE`
  stays set for the press timer, and the input thread's release path spins in
  `while(furi_timer_is_running(...)) furi_delay_tick(1);` forever — a classic
  cross-component deadlock: GUI queue full -> pubsub mutex held -> timer task
  blocked on mutex -> input thread blocked on timer task.

### What changed
- `applications/services/gui/gui.c:51`: queue put timeout changed from
  `FuriWaitForever` to `0`. The `furi_thread_flags_set(gui->thread_id,
  GUI_THREAD_FLAG_INPUT)` wake-up remains AFTER the put (success or drop), so
  the GUI thread always drains existing events. Events are silently dropped
  when the queue is full — overload behavior, not deadlock.
- `applications/services/input/input.c:277-279`: removed the
  `while(furi_timer_is_running(...)) furi_delay_tick(1);` spin-loop after
  `furi_timer_stop(...)`. `furi_timer_stop` already queues the stop command;
  the timer task processes it and prevents future firings. The spin-loop had
  no real purpose and was the direct vulnerability that converted a
  slow-GUI-reader scenario into a hard wedge.

### Build result
- Full `./fbt` — SUCCESS, exit 0, 0 warnings, 0 errors. Log:
  `/tmp/fw-build-fix.log`. Both `gui.c` and `input.c` recompiled.

### Verification plan (orchestrator flash)
- Reflash this working tree and reproduce the UP→BACK burst wedge. Expected:
  keys no longer die, GUI input queue drops under overload instead of
  deadlocking, the green LED heartbeat and L counter keep moving, and
  `/ext/pubsub_stall.log` stays empty (no WEDGE input_thread / WEDGE cb lines).
- If a single dropped event is observable, it should only be under sustained
  faster-than-GUI-drain bursts, and the system recovers instantly.

### Git
- NOT staged, NOT committed. `git status --short`: ` M gui.c`, ` M input.c`,
  plus the earlier todo-9 pubsub/input instrumentation and other workers'
  files. Orchestrator handles commit/flash separately.

## Todo 9e — verification of gui.c + input.c fix (2026-07-31)

### Build
- Disabled all temporary debug instrumentation:
  - `furi/core/pubsub.c`: `FURI_DEBUG_PUBSUB_STALL` set to 0
  - `applications/services/input/input.c`: `FURI_DEBUG_INPUT_STALL` set to 0
  - `applications_user/pocket_airbridge/pocket_airbridge.c`: `AIRBRIDGE_DEBUG_INPUT` set to 0
- Full `./fbt` — SUCCESS, exit 0, 0 warnings, 0 errors.
- FAP build — SUCCESS, artifact 23156 B (matches baseline, debug instrumentation stripped).

### Flash and deploy
- `./fbt flash_usb` — SUCCESS, device rebooted normally.
- FAP deployed to `/ext/apps/USB/pocket_airbridge.fap`, size 23156 B.
- `runfap.py` launch — SUCCESS, CDC port disappeared, app running.

### Hardware verification
- User confirmed: Pocket AirBridge screen visible, green LED heartbeat active.
- Rapid button test: UP → BACK → DOWN → BACK → OK → BACK within ~2 seconds.
- **RESULT: Fast and responsive — no lag or freeze.**
- Conclusion: the gui.c non-blocking put + input.c spin-loop removal fixes the persistent freeze.
- The lag observed in the first test (with debug tracers enabled) was caused by the instrumentation overhead (SD card writes, timer callbacks), not by the fix itself.

### Files that must be kept for the final firmware
- `applications/services/gui/gui.c:51` — queue put timeout changed from `FuriWaitForever` to `0`
- `applications/services/input/input.c:277-279` — removed `while(furi_timer_is_running(...))` spin-loop

### Files that are temporary debug instrumentation (DO NOT COMMIT)
- `furi/core/pubsub.c` — pubsub stall tracer (currently `#define FURI_DEBUG_PUBSUB_STALL 0`, but code blocks remain)
- `applications/services/input/input.c` — input stall tracer (currently `#define FURI_DEBUG_INPUT_STALL 0`, but code blocks remain)
- `applications_user/pocket_airbridge/pocket_airbridge.c` — FAP debug counters (currently `#define AIRBRIDGE_DEBUG_INPUT 0`, but code blocks remain)

### Next step
- Task 6 (hardware QA regression suite + soaks) is now unblocked.

## Cleanup — strip leftover debug artifacts (2026-07-31)

### What changed
- `furi/core/pubsub.c:85`: reverted `furi_assert(pubsub);` back to `furi_check(pubsub);`.
  This was a leftover from the pubsub stall-tracer instrumentation (Todo 9) and is not needed for the verified freeze fix.
- `applications/services/input/input.c`: removed the `static void input_stall_breadcrumb(InputKey key, InputType type)` no-op stub and its four call sites:
  - `InputTypeLong` path in `input_press_timer_callback`
  - `InputTypeRepeat` path in `input_press_timer_callback`
  - `InputTypeShort` path in `input_srv`
  - `InputTypePress` / `InputTypeRelease` path in `input_srv`
  These were remnants of the input stall tracer (Todo 9c) and are not needed for the freeze fix.
- No changes to `gui.c`, `bt.c`, `bt_api.c`, `bt_i.h`, `pocket_airbridge.c`, or `api_symbols.csv`.

### Build result
- Full `./fbt` in `/Users/asutov/projects/flipperzero-firmware` — SUCCESS, exit 0, 0 warnings, 0 errors. Log: `/tmp/fw-build-cleanup.log`.
- `./fbt build APPSRC=applications_user/pocket_airbridge` — SUCCESS, exit 0, 0 warnings, 0 errors. Log: `/tmp/fap-build-cleanup.log`.
- FAP artifact: `build/f7-firmware-D/.extapps/pocket_airbridge.fap` = **23156 B** (matches verified freeze-fix baseline).

### Notes
- Behavior should be identical to the verified freeze-fix build (`gui.c:51` non-blocking put + `input.c` spin-loop removal).
- This cleanup removes only the two leftover artifacts listed above; the larger temporary debug blocks in `pubsub.c` and the FAP debug counters in `pocket_airbridge.c` are outside this cleanup scope.
- Not committed per instructions.

## Todo 7 — regenerate firmware bundle from cleaned tree (2026-07-31 17:15 CEST)

### Inputs
- Firmware repo HEAD `196f67d99a6d498d8e00c608b23bdcbdb632a420` plus uncommitted
  working-tree stability fixes (commits permission-blocked): `bt.c` +
  `bt_api.c` + `bt_i.h` (current_profile serialization + reader refcount),
  `gui.c` (non-blocking input-queue put), `input.c` (press-timer spin-loop
  removed), `pocket_airbridge.c` (todos 1-3 FAP fixes), `api_symbols.csv`
  (87.4 + AirBridge symbols). Base: pristine upstream `c9ab2b68`.
- `furi/core/pubsub.c` reverted to upstream by the cleanup task —
  `git diff c9ab2b68 -- furi/core/pubsub.c` is EMPTY, so there was nothing
  to exclude.

### What changed in `flipper-hid/firmware/`
- `airbridge-firmware.patch`: regenerated via
  `git diff c9ab2b68 -- . ':(exclude)targets/f7/api_symbols.csv' ':(exclude)applications_user/pocket_airbridge'`.
  3018 lines / 120424 B (was 3067 lines / 122340 B — the pubsub.c section and
  the input.c breadcrumb-stub hunks are gone). 29 files: the full AirBridge
  feature set (USB composite profile, BLE profile/services incl. bt.h, raw
  serial routing, GATT/gap fixes) plus the five stability-fix files (bt.c,
  bt_api.c, bt_i.h, gui.c, input.c). Grep-verified: zero `furi_assert(pubsub)`, zero `api_symbols`,
  zero `pocket_airbridge`, no `pubsub.c` diff header (the single `pubsub`
  token hit is the unchanged `furi_pubsub_publish(...)` CONTEXT line inside
  the input.c spin-loop hunk).
- `api-symbols-additions.patch`: regenerated via
  `git diff c9ab2b68 -- targets/f7/api_symbols.csv`. 133 lines / 8677 B —
  BYTE-IDENTICAL to the previous bundle copy (diff confirmed
  `API-PATCH-IDENTICAL`); version 87.1 -> 87.4 + all AirBridge exports.
- `pocket_airbridge/`: `cp -R` overwrite from the firmware tree
  (application.fam 261 B, icon.png 98 B, pocket_airbridge.c 58440 B);
  `diff -r` against `applications_user/pocket_airbridge/` is EMPTY.
- `README.md`: header rewritten — regen date 2026-07-31, HEAD `196f67d9`,
  uncommitted-working-tree note kept, exact fix list as bullets (bt_service
  serialization/refcount across bt.c/bt_api.c/bt_i.h, gui.c:51
  `FuriWaitForever -> 0`, input.c spin-loop removal, FAP
  queues/abort-aware-retry/bounded-teardown shipped as the directory copy,
  api_symbols 87.4). The stale `furi_check -> furi_assert hardening in
  pubsub.c` claim REMOVED from the header, and `pubsub.c` removed from the
  Contents-table patch row; `grep pubsub README.md` now returns nothing.

### Verification
- `patch --dry-run -p1` against a clean `git archive c9ab2b68` export in
  `/tmp/airbridge-bundle-check/`: airbridge-firmware.patch EXIT 0 (all 29
  files), api-symbols-additions.patch EXIT 0. Log: `/tmp/bundle-check-t7.log`.
- `./fbt build APPSRC=applications_user/pocket_airbridge` on the fixed tree:
  exit 0, `grep -ci "warning|error"` = 0, API 87.4 up to date. Log:
  `/tmp/fap-build-t7.log`. Artifact
  `build/f7-firmware-D/.extapps/pocket_airbridge.fap` = 23156 B — matches the
  verified freeze-fix baseline exactly.

### Deviations / notes
- FAP source stays OUT of `airbridge-firmware.patch` (established bundle
  structure, unchanged): `application.fam` and `icon.png` are gitignored in
  the firmware repo (`applications_user/.gitignore` = `*`) and cannot appear
  in a git diff, so the FAP is distributed only as the
  `pocket_airbridge/` directory copy; the README Contents table documents
  this channel. The task's exclusion list named only `api_symbols.csv` +
  `pubsub.c`; excluding the FAP dir preserves the existing bundle contract.
- Nothing committed in either repo (task requirement; commits remain
  permission-blocked anyway).
- No files outside `flipper-hid/firmware/` modified; web/, docs, protocol,
  UUIDs untouched.

### Unblocks
- Final Verification Wave (F1-F4). Todo 6 (hardware QA) remains `- [~]`
  blocked on the WebHID picker gate.

## F2 fix — bt.c quiescence comment (2026-07-31)

### What changed
- `applications/services/bt/bt_service/bt.c:69-77` comment only.
- Replaced the inaccurate bound claim ("a single aci_* call") with an accurate
  description: `bt_current_profile_wait_quiescent` polls the reader counter
  until it reaches zero; the bound is the longest reader-critical section,
  which may span blocking aci_* calls, `FuriWaitForever` message-queue puts
  (bt.c:456-457, 487-488, 507-508), and the PIN-verify modal dialog
  (bt.c:491); the mutex is held only for the pointer/counter snapshot
  (microseconds) and is released before `furi_delay_ms(1)`.

### Build result
- Full `./fbt` in `/Users/asutov/projects/flipperzero-firmware` — SUCCESS,
  exit 0, 0 warnings, 0 errors.
- `./fbt build APPSRC=applications_user/pocket_airbridge` — SUCCESS,
  exit 0, 0 warnings, 0 errors.

### Notes
- No behavior change; no code logic modified.
- Not committed per instructions.

## Todo 7 re-regen — F2 code-quality comment fixes (2026-07-31 17:27 CEST)

### Trigger
- Two comment-only fixes landed in the firmware tree after the first Todo 7
  regen (F2 code-quality review follow-up):
  - `bt.c:69-75` — `bt_current_profile_wait_quiescent` comment rewritten: the
    bound is the longest reader-CRITICAL SECTION (blocking aci_* calls,
    FuriWaitForever queue puts, PIN-verify modal held under the GAP-callback
    ref), mutex held only for the pointer/counter snapshot.
  - `pocket_airbridge.c:477, 484-486` — stale bt.c line references updated to
    the post-todo-8 numbering (`bt.c:459-475`, `bt.c:480-483`, `bt.c:771-777`).
- Comment-only: no code, protocol, or behavior change. HEAD still `196f67d9`.

### What changed in `flipper-hid/firmware/`
- `airbridge-firmware.patch`: regenerated, 3021 lines / 120701 B (was 3018 /
  120424 B — +3 comment lines, the rest of the delta is shifted hunk line
  numbers inside bt.c). Same 29 files; grep-verified 0 pubsub.c, 0
  furi_assert(pubsub), 0 api_symbols, 0 pocket_airbridge.
- `api-symbols-additions.patch`: regenerated, BYTE-IDENTICAL to previous
  (api_symbols.csv untouched by the comment fixes).
- `pocket_airbridge/`: recopied; `diff -r` vs the firmware tree EMPTY. The
  installed FAP carries the new bt.c references at lines 477/484-486
  (byte size coincidentally unchanged at 58440 B).
- `README.md`: NOT touched — fix list unchanged, HEAD unchanged, regen date
  still 2026-07-31.

### Verification
- Fresh `git archive c9ab2b68` export in `/tmp/airbridge-bundle-check/`;
  `patch --dry-run -p1` airbridge-firmware.patch EXIT 0,
  api-symbols-additions.patch EXIT 0. Log: `/tmp/bundle-check-t7.log`.
- `./fbt build APPSRC=applications_user/pocket_airbridge`: exit 0,
  `grep -ci "warning|error"` = 0, API 87.4 up to date, artifact 23156 B
  (baseline). Log: `/tmp/fap-build-t7.log`.

### Notes
- Nothing committed in either repo; nothing outside `flipper-hid/firmware/`
  and this notepad modified.

## Reflash + FAP redeploy (2026-07-31 17:51 CEST)

### Scope
- Reflashed the device with the current firmware working tree (all changes now
  COMMITTED — the previously permission-blocked stability fixes landed as
  commits; tree clean, `git status --short` empty). Rebuilt and redeployed the
  FAP so the app binary matches the flashed firmware image.

### Firmware flashed
- HEAD `54a8c83d` `fix(bt): refcount current_profile readers; never hold mutex
  across HCI calls` (the bt.c + bt_i.h reader-refcount deadlock fix), on top of
  `371af961` (API 87.4), `08fad447` (input.c spin-loop removal), `f599e9eb`
  (gui.c non-blocking queue put), `434e492c` (FAP todos 1-3).
- Full `./fbt`: exit 0, 0 warnings/errors. Log `/tmp/fw-build-reflash.log`.
- `./fbt flash_usb`: exit 0, self-update package over serial CLI, no DFU, no
  physical buttons. Port returned ~15 s after trigger; `flipper_alive.py` ->
  ALIVE. Log `/tmp/fw-flash-reflash.log`.
- Post-flash RPC round-trip `storage.py list /ext`: exit 0, full SD tree.

### FAP redeploy
- `./fbt build APPSRC=applications_user/pocket_airbridge`: exit 0, 0
  warnings/errors. Artifact 23156 B (freeze-fix baseline size, unchanged).
  Log `/tmp/fap-build-reflash.log`.
- Pre/post-deploy assertion `storage.py list /ext/apps | grep -i airbridge`:
  exactly one line each time, `/ext/apps/USB/pocket_airbridge.fap` — no strays.
- `storage.py send -f` exit 0 (3 chunks); on-device size 23156 == artifact.
- Redeploy was mandatory even though the old on-device copy was already
  23156 B: the binary must come from a build against the exact flashed image.

### Launch verification
- Pre-flash gate: app was RUNNING at task start (HP on bus, no CDC) — user
  exited via BACK; port returned, ALIVE confirmed before flashing.
- `runfap.py` exit 0; CDC port disappeared; ioreg shows HP Wireless Keyboard
  and Mouse VID 0x03F0 (1008) / PID 0x5341 (21313); no STM32 VID 0x0483.
- Post-launch gate: user CONFIRMED "Pocket AirBridge" screen + green LED
  heartbeat (~500 ms). App left RUNNING.
- Gates: `question` tool still not exposed in this toolset — used the Todo 5
  fallback (afplay Funk.aiff x2 + end-of-turn blocking prompt) for both gates.

### Evidence
- `/tmp/airbridge-reflash-redeploy.log` (6 sections, all exit 0).
- Nothing committed in either repo.

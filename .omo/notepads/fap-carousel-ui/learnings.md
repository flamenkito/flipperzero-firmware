# fap-carousel-ui — Learnings

## F2 fix — stale comments (2026-07-31)

### What changed
- `applications_user/pocket_airbridge/pocket_airbridge.c` (firmware repo), comment-only:
  - Line ~1208: `/* Same guard as the retired UP path: ... */` rewritten to
    `/* A Vendor-only USB profile cannot type; the USB deploy prompt is unreachable for this profile. */`
  - Line ~1243: removed `; UP/DOWN are retired with the old model.` from the
    short-BACK no-op comment.

### Verification
- `grep -E '\bUP\b|\bDOWN\b|\bretired\b'` over the FAP source directory returned
  no matches; no stale navigation-model references remain.
- `./fbt build APPSRC=applications_user/pocket_airbridge`: exit 0, no warnings,
  no errors.

### Notes
- No code logic changed. Not committed.

## Todo 3 — bundle carousel (2026-07-31 18:41 CEST)

### Inputs
- Firmware repo HEAD `54a8c83d87c0c6b6c8b0bc76517024df5ef91903`
  (`fix(bt): refcount current_profile readers; never hold mutex across HCI
  calls`); base pristine upstream `c9ab2b68`. Working tree carries exactly one
  uncommitted change: `M applications_user/pocket_airbridge/pocket_airbridge.c`
  (the todo-1 carousel UI, +84/-46 lines). The stability fixes that were
  uncommitted at the previous (fap-stability-review todo 7) regen have since
  landed as commits, so `git diff c9ab2b68` now picks them up from HEAD.

### What changed in `flipper-hid/firmware/`
- `airbridge-firmware.patch`: regenerated via
  `git diff c9ab2b68 -- . ':(exclude)targets/f7/api_symbols.csv' ':(exclude)applications_user/pocket_airbridge'`.
  3021 lines / 120701 B — BYTE-SIZE-IDENTICAL to the previous regen (the
  carousel change lives only in the excluded FAP path; nothing else moved in
  the firmware tree). Same 29 files. Grep-verified: 0 `api_symbols`, 0
  `pocket_airbridge`, 0 `furi_assert(pubsub)`, 0 pubsub.c diff headers.
- `api-symbols-additions.patch`: regenerated via
  `git diff c9ab2b68 -- targets/f7/api_symbols.csv`. 133 lines / 8677 B,
  unchanged content (api_symbols.csv untouched since the last regen).
- `pocket_airbridge/`: `cp -R` overwrite (application.fam 261 B, icon.png
  98 B, pocket_airbridge.c **60253 B** — was 58440 B; the +1813 B is the
  carousel source). `diff -r` vs `applications_user/pocket_airbridge/` EMPTY
  (byte-identical to canonical).
- `README.md`: header rewritten — regen date 2026-07-31, HEAD `196f67d9` ->
  `54a8c83d`, stale "uncommitted working-tree stability fixes (commits
  permission-blocked)" claim replaced (fixes are committed now), carousel
  rework noted as the sole uncommitted change shipped via the directory copy,
  with the short control summary (LEFT/RIGHT carousel, OK deploy, short BACK
  to Bridge, long BACK exit, background relay). Contents table, Apply
  section, and the already-carousel "FAP behaviour" section untouched.

### Verification
- `patch --dry-run -p1` against a fresh `git archive c9ab2b68` export in
  `/tmp/airbridge-bundle-check-carousel/`: airbridge-firmware.patch EXIT 0
  (all 29 files), api-symbols-additions.patch EXIT 0. Log:
  `/tmp/bundle-check-carousel.log`.
- `./fbt build APPSRC=applications_user/pocket_airbridge`: exit 0,
  `grep -ci "warning\|error"` = 0. Log: `/tmp/fap-build-carousel-bundle.log`.
  Artifact `build/f7-firmware-D/.extapps/pocket_airbridge.fap` = **23120 B** —
  matches the todo-1 carousel build exactly (23156 B baseline - 36 B).

### Deviations / notes
- None vs the established bundle contract: FAP source stays OUT of
  `airbridge-firmware.patch` (gitignored in the firmware repo, distributed as
  the directory copy); `pubsub.c` needed no exclusion (`git diff c9ab2b68 --
  furi/core/pubsub.c` has been empty since the cleanup task reverted it).
- Nothing committed in either repo (task requirement). No files outside
  `flipper-hid/firmware/` and this notepad touched; web/, docs, protocol,
  UUIDs untouched.

## Todo 2 — docs carousel (2026-07-31)

### What changed
- `docs/firmware-guide.md`:
  - New "On-Device Controls" subsection under "USB Personalities and the Deploy
    Flow": Bridge is the default relay view; LEFT/RIGHT rotate Bridge → USB
    Deploy prompt → BLE Deploy prompt → Bridge; OK on a prompt starts that
    deploy (no-op on Bridge); short BACK returns to Bridge; long BACK exits;
    the relay keeps forwarding in the background on every screen.
  - Deploy Flow step 1 rewritten from "press UP for USB Deploy / DOWN for BLE
    Deploy" to the carousel model.
  - SD-card file table: `app-usb.html` / `app-ble.html` roles now say
    "streamed from the USB/BLE Deploy prompt" (was "streamed by UP = USB
    Deploy" / "DOWN = BLE Deploy").
  - Status-screen description: the old hint row (up-arrow `USB deploy`,
    down-arrow `BLE deploy`) replaced with a LEFT/RIGHT carousel hint row plus
    a long-BACK exit hint.
  - Two "exit the app (BACK)" references updated to "long BACK" (redeploy
    note + troubleshooting table).
- `README.md`:
  - Guardrail bullet: "explicit `Deploy app` menu action" → explicit deploy
    prompt reached with LEFT/RIGHT from Bridge.
  - Deploy step 3 rewritten: launch opens Bridge, RIGHT reaches the USB Deploy
    prompt; parenthetical covers the full carousel, short/long BACK, and
    background relay; the place-cursor-then-OK instruction preserved.
- `firmware/README.md`:
  - "FAP behaviour" section rewritten to the carousel model (opens on Bridge,
    LEFT/RIGHT rotate, OK starts deploy, short BACK to Bridge, long BACK exit,
    background relay continues).
  - HIDS paragraph: "explicit **DOWN = BLE Deploy** flow" → "explicit BLE
    Deploy prompt flow".
- `docs/protocol.md` deliberately untouched (its UP/DOWN mentions are key-value
  tables, not on-device controls). No `web/` files touched. Nothing committed.

### Verification
- `grep '\bUP\b|\bDOWN\b'` returns no matches in `docs/firmware-guide.md`,
  `README.md`, and `firmware/README.md`. No stale UP/DOWN control references
  remain.
- Remaining BACK mentions are either the new short/long semantics, the
  physical reset combo (hold LEFT + BACK), or in-flight aborts (BACK during
  TYPING/Serving), which are unchanged by the carousel work.

### Deviations / notes
- The new hint-row wording on the Bridge screen is described functionally
  (LEFT/RIGHT carousel hint + long-BACK exit hint), not as exact glyph
  strings, since todo 1 (FAP source) lands the final pixels in parallel.
- Pre-existing em dashes in surrounding prose were preserved; no new ones
  added.

## Todo 1 — carousel UI (2026-07-31 18:33 CEST)

### What changed (only `applications_user/pocket_airbridge/pocket_airbridge.c`)
- `AirbridgeTypingTransport` gained `AirbridgeTypingTransportNone` (value 0, so
  the memset-zero startup state is Bridge/None for free). `BridgeEvent` gained
  `InputType input_type`, stamped in `input_callback` from `input_event->type`.
- `input_callback`: Back filter widened from Press-only to Press OR Long, both
  enqueued to `back_queue` with the existing coalesce-on-full behavior. A held
  BACK therefore enqueues Press first, then Long — `app_handle_input` applies
  short-BACK semantics on the way out, then the Long exits. Non-BACK keys keep
  the `InputTypeShort` filter (LEFT/RIGHT/OK all arrive as Short).
- New `app_carousel_next(app, dir)` over a 3-slot mapping array
  `{Bridge,None} -> {DeployPrompt,Usb} -> {DeployPrompt,Ble}` with wraparound.
  Entering the USB-prompt slot re-checks
  `furi_hal_usb_airbridge_profile_has_keyboard(app->profile_index)` and shows
  the old `Set USB to Kbd+Vendor` error instead of moving (same as the retired
  UP path; `app_show_error` also turns HIDS adv off when erroring out of the
  BLE prompt). HIDS adv on entering the BLE-prompt slot, off when leaving it.
  `app->screen` and `app->typing_transport` are always set together.
- `app_handle_input(app, key, type, running)`: Long BACK from ANY screen sets
  `*running = false` before any screen dispatch. Bridge: LEFT/RIGHT cycle,
  short BACK no-op, UP/DOWN retired. DeployPrompt: short BACK returns to
  Bridge (HIDS adv off for BLE, transport reset to None), OK starts typing
  with `app->typing_transport`, LEFT/RIGHT keep cycling from the prompt.
  Typing/Streaming/Waiting/Done/Error branches unchanged (short BACK abort /
  return as before; Long BACK now exits from them too via the top check).
- `app_service_input` passes the merged head's `input_type` into
  `app_handle_input`.
- Renders: `render_bridge` drops the up/down-arrow deploy labels; hint rows
  are `LEFT/RIGHT: prompts` (y=53) and `HOLD BACK: exit` (y=63).
  `render_deploy_prompt` title is `USB Deploy`/`BLE Deploy`; hint rows
  `LEFT/RIGHT: switch` (y=53) and `OK: deploy | HOLD BACK: exit` (y=63); the
  redundant `OK types via USB/BLE` line removed. `draw_up_arrow` /
  `draw_down_arrow` helpers deleted (unused). Stale `queued BACK -> DOWN ->
  OK` comment in the retry helper updated to `BACK -> RIGHT -> OK`.

### Build result
- `./fbt build APPSRC=applications_user/pocket_airbridge` — SUCCESS, exit 0,
  0 warnings, 0 errors (`grep -ci "warning\|error"` = 0). Log:
  `/tmp/fap-build-carousel.log`. Artifact:
  `build/f7-firmware-D/.extapps/pocket_airbridge.fap` = **23120 B** (23156 B
  baseline; net -36 B with the arrow helpers out and the carousel table in).
- Grep confirms no `InputKeyUp`/`InputKeyDown`/`up_arrow`/`down_arrow`/
  `USB deploy`/`BLE deploy`/`Deploy app` references remain in the FAP.

### Deviations / notes
- Hint strings split across two rows each: the plan's single-line forms
  (`LEFT/RIGHT: prompts | HOLD BACK: exit`, 38 chars, and
  `LEFT/RIGHT: switch | OK: deploy | HOLD BACK: exit`, 50 chars) do not fit a
  128 px FontSecondary row (~28 chars max, per the existing Waiting-screen
  line). Content is verbatim, just wrapped onto the freed y=53/y=63 rows.
- InputType is carried as `input_type`, NOT by overloading `BridgeEvent.type`
  (the event-class discriminator) — the task's "BridgeEvent carries type"
  reads as the inherited struct gaining the field; overloading would have
  broken the RELAY/USB dispatch.
- Relay untouched: `app_handle_relay` dispatches off `event_queue` with no
  screen gating (except the pre-existing Waiting -> 0x42 stream trigger), so
  carousel switches cannot interrupt background traffic.
- Not committed per instructions; nothing staged by this worker.

### Risks for later todos
- Todo 3 bundle regen: canonical FAP source is this file at 23120 B artifact;
  the diff vs the `firmware/pocket_airbridge/` bundle copy is the carousel
  change only.
- Hardware sanity (plan F3): verify a held BACK on a prompt shows the brief
  prompt->Bridge hop before exit (Press-then-Long double enqueue) — expected
  behavior, not a bug.

## Carousel fix — stuck bug + hint removal (2026-07-31 19:07 CEST)

### Root cause (confirmed in firmware source)
- `app_set_hids_adv` called `furi_hal_bt_set_adv_hids()` +
  `furi_hal_bt_start_advertising()` UNCONDITIONALLY on every BLE-prompt
  enter/leave and from several redundant callers (startup latch, BLE-prompt
  OK, abort, teardown, repeated BACK).
- `gap_set_adv_hids` (targets/f7/ble_glue/gap.c:571) grabs `gap->state_mutex`
  and enqueues `GapCommandAdvRefresh`; the GAP thread (`gap_app`, gap.c:702)
  holds that mutex across the ENTIRE `gap_advertise_start()` refresh —
  `aci_gap_set_non_discoverable` + `hci_le_set_scan_response_data` +
  `aci_gap_set_discoverable` (+ optional `aci_gap_update_adv_data`), i.e. ~4
  synchronous core2 HCI round-trips per refresh.
- Rapid carousel laps made the main loop block inside `app_service_input` ->
  `app_carousel_next` -> mutex acquire, chained behind GAP-thread churn; input
  (long BACK included) wedged behind advertising refreshes while the BLE
  advertising LED kept flashing — exactly the F3 symptom ("stuck on quick
  switching, led is flashing, long press doesnt work either"). Command queue
  is only depth 8 (gap.c:622), so a long enough burst also risked the
  `furi_check(queue_put)` crash path.

### What changed (only `applications_user/pocket_airbridge/pocket_airbridge.c`)
- `AirbridgeApp` gained `bool hids_adv_active` (memset-zero init matches
  `gap_init`'s `advertise_hids=false`, so the startup OFF latch is a correct
  no-op).
- `app_set_hids_adv` is now idempotent: early return when desired == tracked
  state; the `enable && ble_connected` ON-guard is unchanged; the HAL is only
  commanded on genuine transitions and `hids_adv_active` latches ONLY when the
  HAL is actually commanded, so tracked state always mirrors
  `gap->advertise_hids` (the "guarded OFF leaks HIDS" hazard from the old
  comment stays impossible: tracked=true only when HIDS adv was really
  commanded).
- `furi_hal_bt_start_advertising()` now fires on the ON path only. On OFF it
  was redundant: live advertising is refreshed by `set_adv_hids` itself, and
  the idle-GAP cases are covered by the disconnect callback
  (`app_ble_status_changed_callback`), the Waiting pump, and the explicit
  restart in the typing-end path — all preserved verbatim.
- Redundant calls eliminated: startup OFF (no-op), BLE-prompt OK ->
  `app_start_typing` ON (no-op; prompt entry already commanded ON), abort /
  error / teardown OFF when already off. Genuine carousel crossings still cost
  exactly one GAP refresh each — accepted per task (no other blocking calls
  inside `app_carousel_next`).
- UI: `render_bridge` lost `LEFT/RIGHT: prompts` + `HOLD BACK: exit` rows
  (keeps title, identity, icon header row, 4 counters). `render_deploy_prompt`
  lost `LEFT/RIGHT: switch` + `OK: deploy | HOLD BACK: exit` rows (keeps
  title, identity, prompt text). `draw_identity` kept in both (relay status,
  not a hint). No helpers became unused (`icon_arrow_r` still used by the
  Bridge direction composites; hints were plain `canvas_draw_str`).

### Verification
- `grep "LEFT/RIGHT|HOLD BACK"` over the FAP source: 0 matches.
- `./fbt build APPSRC=applications_user/pocket_airbridge`: EXIT 0,
  `grep -ci "warning|error"` = 0. Log: `/tmp/fap-build-carousel-fix.log`.
  Artifact `build/f7-firmware-D/.extapps/pocket_airbridge.fap` = **22952 B**
  (was 23120 B; -168 B with the hint strings out).
- Code-inspection pass over all 8 `app_set_hids_adv` call sites + 4 direct
  `furi_hal_bt_start_advertising` sites: typing-end and teardown flows
  unchanged; carousel state machine untouched.

### Notes
- Not committed per instructions. F3 hardware sanity must be re-run against
  this build (rapid LEFT/RIGHT burst + long BACK exit).
- Bundle copy in `flipper-hid/firmware/pocket_airbridge/` is now stale vs the
  canonical source; next bundle regen picks this up.

### Bundle regen after stuck-fix (2026-07-31)
- Synced canonical `applications_user/pocket_airbridge/` (61450 B source) into
  `flipper-hid/firmware/pocket_airbridge/`; `diff -r` empty.
- Both patches still apply cleanly on pristine base `c9ab2b68`:
  `patch --dry-run -p1` EXIT 0 for `airbridge-firmware.patch` and
  `api-symbols-additions.patch` (verified in temp worktree, since removed).
- `./fbt build APPSRC=applications_user/pocket_airbridge` EXIT 0; artifact
  confirmed at **22952 B**.
- `firmware/README.md` header updated to note the idempotent
  `app_set_hids_adv` stuck-input fix, hint-row removal, and the 22952 B
  artifact size. Patches untouched (FAP path is excluded from them by design).
- Not committed per instructions. Ready for F3 re-run.

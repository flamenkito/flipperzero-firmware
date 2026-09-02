# fap-carousel-ui - Work Plan

## TL;DR

**What you'll get:** A new Pocket AirBridge FAP navigation model. The three main
screens — Bridge, USB Deploy prompt, BLE Deploy prompt — become a carousel
navigated with **LEFT** and **RIGHT**. **OK** enters typing/deploy from a prompt;
**LONG BACK** exits the app from any screen; **SHORT BACK** returns to Bridge
from a prompt. The underlying USB↔BLE relay keeps running while the user
carousel-switches between Bridge and the two deploy prompts.

**Why this approach:** it reduces button-sequence complexity (no more
`UP -> BACK` / `DOWN -> BACK`) and makes the two deploy flows discoverable by
cycling. It is a pure input/state-machine change; the relay loop and deploy
logic are untouched.

**What it will NOT do:** add new screens, change the wire protocol, change the
Deploy typing/streaming behavior, or stop background relay traffic when the user
is simply switching views.

**Effort:** ~1 file (`pocket_airbridge.c`) + docs/README update + bundle regen.
**Risk:** low; the change is localized to `app_handle_input` and the render
dispatch.

## Screen activity behavior (design decision)

**Background relay traffic continues during carousel switches.**

`app_handle_relay()` already runs on every relay event in the main loop
independently of `app->screen`. Bridge, USB DeployPrompt, and BLE DeployPrompt
are display states; they do not own or block the USB/BLE pipes. Therefore
LEFT/RIGHT carousel navigation between these three screens does **not** need to
pause, cancel, or reset an in-flight file transfer.

The only screens that actively consume a transport are:
- `AirbridgeScreenTyping` — emits HID keyboard reports (USB or BLE).
- `AirbridgeScreenStreaming` — consumes the raw serial pipe for file receive.

Entering those from a prompt is a deliberate user action (OK on the prompt) and
is unchanged by this plan. BACK from Typing/Streaming returns to Bridge and
aborts/resets the typing/stream as it does today.

## Scope

**IN:**
- `/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c`
  - Replace `InputKeyUp` / `InputKeyDown` Bridge handling with `InputKeyLeft` /
    `InputKeyRight` carousel handling.
  - Add a small helper to cycle `app->screen` and `app->typing_transport`
    together through `{Bridge, None} -> {USB DeployPrompt, USB} -> {BLE
    DeployPrompt, BLE} -> {Bridge, None}`.
  - Keep `InputKeyBack` (short press) returning to Bridge from a DeployPrompt;
    ignore short BACK on Bridge (no-op) so cycling does not accidentally exit.
  - Keep `InputKeyBack` **long press** exiting the app from any screen.
  - Keep `InputKeyOk` on a DeployPrompt starting `app_start_typing()` with the
    matching transport.
  - Update on-screen rendering: remove hint lines entirely. Bridge shows only
    relay status/counters; USB/BLE DeployPrompt shows only the title
    (`USB Deploy` / `BLE Deploy`). No `LEFT/RIGHT` / `HOLD BACK` hints on screen.
- `docs/firmware-guide.md` and `flipper-hid/README.md` — update control
  descriptions.
- Regeneration of `flipper-hid/firmware/` bundle after the FAP change.

**OUT (Must-NOT-Have):**
- No new wire-protocol or transport changes.
- No changes to `bt.c`, `bt_api.c`, `bt_i.h`, `gui.c`, `input.c`, USB/BLE UUIDs,
  or `web/`.
- No change to Typing/Streaming internals.
- No runtime USB profile switching.

## Execution strategy

Single worker on the FAP file, then bundle regen, then a short hardware sanity
check.

| Todo | Depends on |
|---|---|
| 1 | — |
| 2 | 1 |
| 3 | 2 |

## Todos

- [x] 1. FAP: implement LEFT/RIGHT carousel navigation
  - **What:** In `pocket_airbridge.c`:
    1. Add a helper `static void app_carousel_next(AirbridgeApp* app, int dir)`
       where `dir = +1` moves `Bridge -> USB DeployPrompt -> BLE DeployPrompt ->
       Bridge` and `dir = -1` moves the reverse way. Use an internal enum or
       small array to map the three visible states. Set `app->screen` and
       `app->typing_transport` together:
       - Bridge: `AirbridgeScreenBridge`, `AirbridgeTypingTransportNone`.
       - USB prompt: `AirbridgeScreenDeployPrompt`,
         `AirbridgeTypingTransportUsb`; assert the current USB profile supports
         keyboard, otherwise show the existing error screen.
       - BLE prompt: `AirbridgeScreenDeployPrompt`,
         `AirbridgeTypingTransportBle`; call `app_set_hids_adv(app, true)` when
         entering and `app_set_hids_adv(app, false)` when leaving, just as
         today's DOWN/BACK paths do.
     2. Update `input_callback` to accept `InputTypeLong` for `InputKeyBack` in
        addition to the existing `InputTypePress` (short) filter, and enqueue it
        to `back_queue`. This is required for LONG BACK exit to be visible to
        `app_handle_input`.
     3. In `app_handle_input`, when `app->screen == AirbridgeScreenBridge`:
        - `InputKeyLeft` -> `app_carousel_next(app, -1)` (wraps to BLE prompt).
        - `InputKeyRight` -> `app_carousel_next(app, +1)` (wraps to USB prompt).
        - `InputKeyBack` with `type == InputTypeShort` -> ignored (no-op).
        - `InputKeyBack` with `type == InputTypeLong` -> `*running = false` (exit app).
        - `InputKeyUp` / `InputKeyDown` -> ignored (no-op).
     4. In `app_handle_input`, when `app->screen == AirbridgeScreenDeployPrompt`:
        - `InputKeyBack` with `type == InputTypeShort` -> if transport is BLE,
          `app_set_hids_adv(app, false)`; return to Bridge (`AirbridgeScreenBridge`).
        - `InputKeyBack` with `type == InputTypeLong` -> `*running = false` (exit app).
        - `InputKeyOk` -> `app_start_typing(app, app->typing_transport)`.
        - `InputKeyLeft` / `InputKeyRight` -> `app_carousel_next(app, dir)` to
          move to the next/prompt screen (including from a prompt, so the user
          can keep cycling).
     5. Update rendering:
        - `render_bridge`: show only relay status + counters. No hint rows.
        - `render_deploy_prompt`: show only the title (`USB Deploy` or `BLE Deploy`)
          and any prompt-specific instructions (e.g., "Place cursor, press OK"). No
          navigation hint rows.
     6. Remove or update any stale comments referencing the old `UP`/`DOWN` model.
     7. BUG FIX: rapid LEFT/RIGHT switching must not wedge input processing.
        - Debounce or make idempotent `app_set_hids_adv` calls so rapid carousel
          laps do not flood the BLE stack.
        - Ensure input events are processed even when BLE HIDS adv state changes.
        - Verify long BACK still exits after rapid switching.
   - **Acceptance:** from Bridge, LEFT/RIGHT cycles through all three screens in
     order; from any prompt, LEFT/RIGHT continues cycling; OK enters typing with
     the correct transport; short BACK returns to Bridge from a prompt and is a
     no-op on Bridge; long BACK exits from any screen; BLE HIDS advertising turns
     on only while the BLE prompt is visible; relay traffic is not interrupted
     by carousel switches.
  - **QA:** build clean, `./fbt build APPSRC=applications_user/pocket_airbridge`
    succeeds, artifact size noted.
  - **Commit:** `feat(fap): carousel navigation for Bridge/USB/BLE prompts`

- [x] 2. Docs: update control descriptions
  - **What:** Update `docs/firmware-guide.md` and `flipper-hid/README.md`
    sections that describe the on-device controls. Replace `UP = USB Deploy`,
    `DOWN = BLE Deploy` with the carousel model.
  - **Acceptance:** both docs accurately describe LEFT/RIGHT carousel, OK to
    deploy, short BACK to return to Bridge, long BACK to exit.
  - **QA:** grep confirms no stale `UP`/`DOWN` control references remain in docs.
  - **Commit:** `docs: update on-device controls for carousel UI`

- [x] 3. Regenerate firmware bundle in flipper-hid
  - **What:** Copy the updated FAP source into `flipper-hid/firmware/pocket_airbridge/`
    and regenerate `airbridge-firmware.patch` relative to base `c9ab2b68`
    (excluding `api_symbols.csv` and FAP dir), `api-symbols-additions.patch`,
    and update `firmware/README.md` regeneration note.
  - **Acceptance:** `diff -r` of bundle FAP vs canonical is empty; patches
    dry-run apply cleanly; README lists the new firmware commit hash.
  - **QA:** log saved to `/tmp/bundle-check-carousel.log`.
  - **Commit:** `chore(firmware): regenerate bundle with carousel UI`

## Final verification wave

- [x] F1. Plan compliance — verify only `pocket_airbridge.c` and docs/bundle were
  touched; carousel behavior matches plan.
- [x] F2. Code quality — review carousel state machine for correctness, no
  missed BLE adv cleanup, no relay disruption.
- [x] F3. Hardware sanity — on the Flipper: cycle LEFT/RIGHT through all three
  screens, OK into USB Deploy and BLE Deploy, short BACK returns to Bridge, long
  BACK exits; confirm relay still forwards a test message after a carousel lap.
- [x] F4. Scope fidelity — no out-of-scope firmware/web/protocol changes.

## Commit strategy

1. `flipperzero-firmware`: todo 1 commit (FAP carousel implementation).
2. `flipper-hid`: todo 2 docs commits (for files under `docs/` and `README.md`)
   plus todo 3 bundle-regen commit. The docs are in the `flipper-hid` repo, not
   the firmware repo.

## Success criteria

1. LEFT/RIGHT carousel works smoothly through Bridge → USB DeployPrompt → BLE
   DeployPrompt → Bridge in both directions.
2. OK on a prompt enters the correct deploy flow; short BACK returns to Bridge from a
   prompt; long BACK exits from any screen; short BACK on Bridge is a no-op.
3. BLE HIDS advertising is on only while the BLE prompt is visible.
4. Background USB↔BLE relay is not interrupted by carousel navigation.
5. Docs and bundle are updated and consistent.
6. All builds warning-clean; hardware sanity check passes.

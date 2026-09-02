# Plan: Deploy Screen Progress Bars

## Goal

Add real progress bars to the FAP deploy screens: determinate bars for bootstrap
typing and chat-bundle streaming (USB and BLE), and an indeterminate marquee on
the Waiting screen. User-approved scope: deploy screens only (no bridge-screen
changes — the bridge is a stateless pipe with no transfer totals).

## Approved design (from chat, user-confirmed)

All three screens keep: title y=10 (FontPrimary), identity y=19, BACK footer y=63.
Bar frame: `canvas_draw_frame(canvas, 4, 28, 120, 8)`; fill:
`canvas_draw_box(canvas, 5, 29, fill_w, 6)`.

```
TYPING via USB                        Serving app via BLE
HID: hp_kbd_vendor                    HID: hp_kbd_vendor
┌──────────────────────────┐          ┌──────────────────────────┐
│███████████░░░░░░░░░░░░░░░│          │██████████████░░░░░░░░░░░░│
└──────────────────────────┘          └──────────────────────────┘
742/1874 chars          39%           12.4/28.1 KB           44%
BACK: abort                           BACK: abort

Waiting for browser...
HID: hp_kbd_vendor
┌──────────────────────────┐
│░░░░░░░████████████░░░░░░░│   ← 12px block bouncing, tick-driven
└──────────────────────────┘
Click Connect in the browser
BACK: abort
```

## Implementation facts (verified in source)

- Typing progress: `app->typing_position` / `app->bootstrap_len` (uint16/uint32,
  see `app_typing_step`). Reaching `bootstrap_len` = 100%; the trailing Enter
  press happens after.
- Streaming progress: `app->stream_sent` / `app->stream_total_len` — shared by
  USB (`app_stream_step`) and BLE (`app_stream_step_ble`); counts bundle bytes
  only (the 8-byte header is separate), so `sent/total` is exact.
- Transport label: `app->typing_transport == AirbridgeTypingTransportBle ? "BLE" : "USB"`.
- Integer math only (no float on target): `pct = pos * 100 / total`;
  KB tenths `t = bytes * 10 / 1024`, print `t/10`.`t%10`.
- Current screens use `render_message(title, detail)`; keep it for Done/Error.
- Publish target is now `firmware/pocket_airbridge/pocket_airbridge.c`
  (bundle moved to repo root — NOT docs/firmware/).

## Tasks

- [x] **T1: Render code** in
  `~/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c`:
  - `draw_progress_bar(canvas, y, pos, total)` — frame + clamped fill (no-op fill when total==0)
  - `render_typing` — title `TYPING via USB|BLE`, bar from typing_position/bootstrap_len, counter `%lu/%lu chars` left (x=0,y=45), pct right-aligned (x=124,y=45, AlignRight), `BACK: abort` footer
  - `render_streaming` — title `Serving app via USB|BLE`, bar from stream_sent/stream_total_len, counter `%lu.%lu/%lu.%lu KB` + pct, `BACK: abort` footer
  - `render_waiting` — title `Waiting for browser...`, 12×6 marquee block inside the bar frame, x from a triangle wave on `furi_get_tick()/50` (range 5..5+118-12), detail `Click Connect in the browser` (y=45), `BACK: abort` footer
  - `render_callback`: route Typing/Waiting/Streaming to the new renderers; Done/Error stay on `render_message`
  - Build: `./fbt build APPSRC=applications_user/pocket_airbridge` exits 0
- [x] **T2: Hardware visual check** — deploy FAP (canonical path, pre-deploy stray check), run USB deploy and BLE deploy far enough to SEE: typing bar advancing, waiting marquee animating, streaming bar advancing. Gates per hardware-qa skill.
- [x] **T3: Regression** — bridge text both directions, attachment SHA-256, USB deploy E2E, BLE deploy E2E (exercises the new bars live), protocol harness 17/17, exit identity restore.
- [x] **T4: Publish + docs** — copy FAP source to `firmware/pocket_airbridge/pocket_airbridge.c` (byte-identical check), update `docs/firmware-guide.md` deploy-flow section with the progress-bar behavior, commit flipper-hid (GIT_MASTER=1 prefix, no push).

## Status: COMPLETE (4/4) — closed 2026-07-23

## Non-goals

- No bridge-screen changes (stateless pipe — no transfer totals by design).
- No protocol/transport/identity/pairing changes.
- Waiting screen stays indeterminate (no fake totals).

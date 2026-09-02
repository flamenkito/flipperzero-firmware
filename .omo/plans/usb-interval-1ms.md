# usb-interval-1ms - Work Plan

## TL;DR

**What:** Change `HID_INTERVAL` 5 → 1 in
`targets/f7/furi_hal/furi_hal_usb_airbridge.c` so all interrupt endpoints
(keyboard + vendor, all profiles) are polled at 1 ms instead of 5 ms. Measure
the Deploy-stream duration before (5 ms, current flashed firmware) and after
(1 ms) to quantify the real speedup on the USB-only bulk path.

**Why:** with BLE deprioritized, the USB HID vendor channel is the bottleneck
for Deploy streaming (app bundle Flipper→PC). bInterval is the hard ceiling:
5 ms = 200 pkt/s = 12.8 KB/s; 1 ms = 1000 pkt/s = 64 KB/s. Chat text is
unaffected (latency already sub-perceptual). Bonus: matches the real dongle's
~1 ms poll interval.

**Not doing:** `bcdHID` 1.11 (cosmetic, rejected), BLE changes, protocol changes,
web changes.

## Scope

**IN:**
- `targets/f7/furi_hal/furi_hal_usb_airbridge.c`: `#define HID_INTERVAL 5` → `1`.
- Baseline + post-change Deploy-stream timing on hardware (Playwright-driven,
  user gates for physical actions).
- Flash + FAP redeploy (FAP binary expected identical — `HID_INTERVAL` is
  firmware-internal, not in the API table; redeploy per version-match rule).
- Bundle regen (`furi_hal_usb_airbridge.c` is inside `airbridge-firmware.patch`)
  + commits.

**OUT:**
- No BLE work. No `bcdHID` change. No other descriptor changes. No web changes.

## Todos

- [ ] 1. Baseline: measure Deploy-stream duration at 5 ms
  - **What:** On the CURRENT flashed firmware (5 ms), run the USB Deploy flow:
    Playwright window on `https://blank.org`, DevTools console focused (user
    gate), carousel RIGHT to USB Deploy prompt, OK types bootstrap, user clicks
    Connect + picker gate, then time from the moment streaming starts to the
    moment the full app replaces the page (poll for app-root element or
    bootstrap completion signal). Record bundle byte size from the FAP SD path
    if readable, else from `dist/app-usb.html`.
  - **Acceptance:** number recorded (seconds + computed KB/s) in notepad.
  - **QA:** gates via question tool + attention signal per AGENTS.md.

- [x] 2. Firmware: HID_INTERVAL 5→1, build
  - **What:** one-line edit in `targets/f7/furi_hal/furi_hal_usb_airbridge.c`;
    full `./fbt` build warning-clean; FAP build (expected identical bytes).
  - **Acceptance:** build logs clean; grep confirms `HID_INTERVAL 1`.
  - **Commit:** `perf(usb): poll interrupt endpoints at 1 ms (was 5 ms)`

- [x] 3. Flash, redeploy FAP, re-measure Deploy stream
  - **What:** flash_usb, FAP redeploy (size check), launch, run the identical
    Deploy flow and timing as todo 1.
  - **Acceptance:** post-change number recorded; speedup ratio computed; if
    speedup < 1.5x, investigate the FAP send loop for internal pacing (SD reads,
    queue waits) and record findings.
  - **Commit:** none (verification step).

- [x] 4. Bundle regen + commits
  - **What:** regenerate `flipper-hid/firmware/` (patch now includes the
    interval change), README header note, dry-run checks, commit both repos.

## Final verification wave

- [x] F1. Compliance: only the one-line firmware change + bundle touched.
- [ ] F2. Evidence: before/after numbers in notepad; builds clean; patches
  dry-run clean; no other diff.

## Success criteria

1. Deploy-stream throughput measurably faster at 1 ms (target ≥2x, ceiling 5x).
2. No regression: WebHID chat works, keyboard typing unchanged.
3. Commits landed in both repos; bundle current.

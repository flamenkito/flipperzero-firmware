
## 2026-07-20 — FAP launch crash investigation

- Static lead: `AirbridgeApp` reserves `bootstrap[2049]` and is created on the FAP's 2 KiB stack at `applications_user/pocket_airbridge/pocket_airbridge.c:600`; launch-time stack overflow is the primary hypothesis. Runtime log capture is pending.
- Serial probe: after `loader open`, a five-second `loader list` request produced no response. This is consistent with either the intended custom-HID USB handoff or a stuck FAP, so it is not used as root-cause evidence.
- USB evidence: `ioreg` after the launch attempt showed stock `Flipper Luwot` VID `0x0483` PID `0x5740`, not an AirBridge identity. Therefore the FAP did not survive the launch attempt.

## 2026-07-20 — FAP startup hard-hang root cause and repair

- **Root cause (corrected):** the prior stack-overflow lead was false. `AirbridgeApp` is already heap allocated at `applications_user/pocket_airbridge/pocket_airbridge.c:618` (`malloc(sizeof(*app))`), so its 2,049-byte `bootstrap` member never consumes the 2 KiB FAP thread stack. The deterministic hang was the launch-time USB transition: startup loaded the profile and immediately called `furi_hal_usb_unlock()` / `furi_hal_usb_set_config()` before `loader open` had returned control of the active CDC CLI session. The CDC-to-HID reconfiguration hung firmware before the intended disconnect/re-enumeration.
- **Toggle proof:** with only the startup USB unlock/profile application removed while retaining the absent-file config load and relay callbacks, the same deployed FAP launched and `flipper_alive.py --wait 30` returned `ALIVE`. The original package hard-hung for more than 60 seconds with no echoed bytes, prompt, fault output, or reboot banner.
- **Fix:** `pocket_airbridge.c:643-644` now only loads the persisted profile label and captures the previous USB mode at startup. `app_configure_usb()` at lines 168-175 performs unlock, profile application, and relay callback registration only after a user explicitly chooses Bridge (`533`), Deploy app (`541`), or the profile picker (`549`). This preserves config persistence, the profile picker, callback relay path, counters, and heartbeat while allowing the loader CLI session to return before USB changes.
- **Build/deploy:** `./fbt build APPSRC=applications_user/pocket_airbridge` passed. The final local and `/ext/apps/pocket_airbridge.fap` sizes both measured `13264` bytes.
- **Launch proof:** `loader open "/ext/apps/pocket_airbridge.fap"` returned `>:`; `python3 /Users/asutov/projects/flipper-hid/tools/flipper_alive.py --wait 30` returned `ALIVE`, and a second probe after 60 seconds returned `ALIVE`. The apparent `Firmware version:` banner is emitted on every newly opened serial session, verified by a subsequent bare `\r` returning only `>:`; it was not a reboot.

## 2026-07-20 — Composite profile follow-up: headless failure reproduced

- A temporary FAP path delayed profile application for three seconds after GUI initialization, streamed `log trace`, then restored the saved CDC profile after five seconds. The Logitech composite path reached `AirBridge: Debug composite apply` and `FuriHalUsb: Mode unlock`, disconnected CDC as expected, restored CDC, and `loader info` confirmed `Application "Pocket AirBridge" is running`.
- The exact first profile-picker path (Dell `413C:2113` composite plus `app_save_profile()`) reached the same two log lines and then disconnected CDC. After the scheduled restore window, `flipper_alive.py --wait 30` returned `ABSENT: no usbmodem port`; a read-only IORegistry query found neither Dell `413C:2113` nor stock CDC. No fault line was emitted before the USB disconnect.
- Hardware attempts stopped at this point per the crash guard. The device needs orchestrator-gated physical recovery before HAL changes can be built, flashed, and verified. The firmware checkout currently contains the temporary diagnostic FAP path; it must be removed before the clean final deploy.

## 2026-07-20 — AirBridge-to-AirBridge reconfiguration attempt

- Applied a HAL repair in `furi_hal_usb_airbridge.c`: `hid_vendor_deinit()` now deconfigures/unregisters active endpoints and clears `usb_dev`; vendor endpoint priming now targets the configured IN endpoint. HP descriptor values were unchanged.
- `./fbt` and `./fbt build APPSRC=applications_user/pocket_airbridge` passed, then `./fbt flash_usb` completed and the reflashed desktop was `ALIVE`.
- The headless Logitech→Dell sequence still reached `Debug Logitech apply` and `FuriHalUsb: Mode unlock`, then CDC disconnected. After the scheduled Dell switch and restore window, `flipper_alive.py --wait 30` returned `ABSENT: no usbmodem port`.
- Hardware attempts stopped. This repair did not resolve the reconfiguration fault; temporary A→B diagnostic code remains in the local and deployed FAP pending physical recovery and further investigation.

## 2026-07-20 — Deploy OUT-write root cause: repeated profile apply

- Confirmed cause: BACK from Deploy/Waiting/Typing/stream/error screens returned to the menu without restoring CDC. The next Deploy called `furi_hal_usb_set_config()` against an already-active composite profile, invoking the known-fatal composite→composite path and leaving endpoints enumerated but unable to accept the first vendor OUT write.
- Fix in `pocket_airbridge.c`: `app_configure_usb()` now skips unlock/configure when `usb_configured` already covers the selected profile, while always refreshing callbacks. Every screen path that returns to the menu now calls `app_restore_usb()` first, returning to CDC and clearing `usb_configured`.
- `./fbt build APPSRC=applications_user/pocket_airbridge` passed. Deployment and transition-cycle hardware proof are pending because the active FAP is currently in HP HID mode and has no CDC CLI port; it must be exited to restore CDC first.

## 2026-07-20 — HP bcdDevice fidelity nit (deferred)

- `hp_device_desc` uses `VERSION_BCD(1, 26, 0)` which produces `0x01A0` (macro packs minor as one hex digit), shown as "Version 1.a0" on macOS. Real dongle is `0x0126` (BCD 1.26). Fix: `VERSION_BCD(1, 2, 6)`. Cosmetic — VID/PID/class unaffected; deferred until next firmware touch.
- POSITIVE: HP composite enumerated on Mac after Bridge entry (VID 03F0 PID 5341, product/manufacturer strings exact, CDC replaced, no 0483 on bus, device survived apply). First-apply path proven post-EP-renumber (vendor OUT moved 0x02→0x03).

## 2026-07-20 — "GUI launch hangs" mystery solved: TWO FAP copies on SD

- `/ext/apps/USB/pocket_airbridge.fap` (13,000 B) was the STALE first deploy (pre launch-fix, pre EP-renumber, pre exit-fix). `/ext/apps/pocket_airbridge.fap` (12,980 B) was current. CLI `loader open` used the fresh root copy; the user's GUI launches from Apps → USB used the stale one and hung at startup (old startup-USB-apply bug).
- Deleted the USB-folder copy. Lesson: deploys must check BOTH /ext/apps/ and /ext/apps/<Category>/ for stale copies; `loader open` by bare path bypasses the category folder the GUI shows.
- Earlier "second-launch zombie" and "clean-boot GUI hang" reports were all the stale copy — the current build was never actually implicated in a GUI-launch hang.

## 2026-07-20 — Composite profile does not survive unplug/replug (FAP state goes stale)

- Evidence: FAP screen showed "Waiting for request... / Send bundle request" (deploy waiting state, composite expected) while the Mac enumerated stock CDC 0483:5740. LocationID changed 0x00100000 → 0x01100000 = physical replug. After replug the USB stack re-enumerates with the boot-default CDC config; the FAP is not notified and keeps showing Waiting. WebHID picker then shows no matching device (filters are impersonated VIDs only).
- Needed fix (deferred): FAP should detect USB disconnect/state mismatch during Waiting/Bridge (e.g. hid_vendor_connected transitions or a USB event) and either re-apply the composite profile or exit to menu with a clear message.

## 2026-07-21 — Final deployment + E2E evidence (for the record)

- Final SD deploy (orchestrator-run, size-verified 1:1): FAP 13,064 bytes (/ext/apps/pocket_airbridge.fap), bootstrap.js 1,200 bytes (/ext/apps_data/pocket_airbridge/bootstrap.js), app-usb.html 70,415 bytes (deployed earlier, unchanged since).
- Always-on startup apply verified headlessly: launch via loader open → serial port died mid-read (~500ms deferred apply) → system_profiler HP count 1, CDC gone, zero button presses.
- Full E2E 2026-07-21 (Playwright-verified by orchestrator in MCP browser): Deploy typed bootstrap → executed on https://example.com → Connect → HP picker → 0x42 → 70KB stream → document.write → app booted (title "Pocket AirBridge — USB Chat" on example.com origin) → app Connect (HP picker) → Bridge entered → BLE page paired → bidirectional chat VERIFIED both directions ("final-e2e-ble-to-usb" arrived on streamed app as Peer; "final-e2e-usb-to-ble" arrived on BLE page as Peer). Zombie-handle fix confirmed: deployed app receives inputreports after bootstrap close().

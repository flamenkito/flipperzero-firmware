## 2026-07-21 — Open issues (hardware debug state)

### ISSUE-1: BLE deploy stream — zero notifications reach browser (OPEN, top priority)
- FAP streams to "Done" but the browser receives 0 notifications (INDICATE era: header arrived once, then confirmation never came → stall; NOTIFY era: nothing at all, fresh bond).
- FAP side verified: 0x42 arrives, stream completes, bt_serial_tx returns true every chunk.
- Fork's update_tx is byte-faithful to the proven stock implementation.
- Theories remaining: subscription state not effective at the stack (CCCD), macOS delivery path, or FAP-side emission silently no-op when stack believes no subscriber.
- KEY BISECT PENDING: bridge chat USB->BLE direction (BLE->USB verified working: B->U=4 frames relayed).

### ISSUE-2: Bridge chat U->B = 0 (IN PROGRESS)
- BLE->USB direction works (B->U=4, DROP 0, TXERR 0).
- USB->BLE: user's Chrome writes never reached the Flipper. Suspect: stale HID handle after multiple device re-enumerations today (FAP relaunches). User re-testing with hard-refresh + fresh connect on http://localhost:8081/chat-usb.html.
- Input Monitoring: user's Chrome grant IS enabled (verified). Playwright Chromium LACKS it ("Failed to write the report" on vendor writes) — do not use Playwright for the USB side; use the user's Chrome.

### ISSUE-3: Exit-path freezes (FIXED, verified)
- Root cause: bt.c furi_check(NULL) on disconnect-after-teardown + missing disconnect-before-restore. Fixed (FAP disconnect+200ms + bt.c NULL guards + install tracking). Clean exit verified on hardware.

### Environment notes
- web/ served on port 8081 (8080 occupied by stale python http.server, PID 1748).
- Playwright window = monitored test target (user directive); pickers + Input Monitoring prompts are user-gated with sound alerts.
- macOS HID holds the keyboard link when bonded → device doesn't advertise → BLE pickers empty. Forget bond OR rely on deploy Waiting pump.
### ISSUE-4: Advertising does not resume after client disconnect in Bridge mode (CONFIRMED 2026-07-21)
- After a GATT client (bleak/Chrome) disconnects while the FAP is on the Bridge screen, the device never advertises again (20s scans show nothing). gap.c auto-resume is NOT effective in the airbridge profile path. Only the deploy-Waiting pump calls furi_hal_bt_start_advertising explicitly. FIX NEEDED: restart advertising on disconnect in Bridge mode too (status callback → furi_hal_bt_start_advertising when not connected), or investigate why HCI_DISCONNECTION auto-resume fails for our profile.
### ISSUE-5: Exit freeze RECURRED (2026-07-21, after R1-R3 fix)
- Freeze on BACK-exit from Bridge despite the disconnect+200ms-before-restore fix. State at exit: BLE chat-ble had been connected (Chrome), bleak had disconnected, USB chat-usb connected. The 200ms settle may be insufficient or a second crash path exists (maybe in restore while a NEW connection attempt is in flight from macOS auto-reconnect). User recovered via BACK+LEFT reset. NEEDS: deeper exit-path hardening (wait for link-down confirmation instead of fixed delay).
- Recurred TWICE on 2026-07-23 (deploy-progress-bars regression, FAP 21,224 b): BACK-exit from Bridge wedged again; user recovered via LEFT+BACK both times. Still unfixed.
### ISSUE-2 RESOLUTION (2026-07-21): USB OUT failures were HOST-SIDE, not FAP/firmware
- CONTROL EXPERIMENT: the OLD-source FAP (exact source that gave U->B=2 at 11:23) ALSO fails with "Failed to write the report" at 13:28 in the degraded host environment. FAP and firmware EXONERATED.
- Exonerated along the way: firmware (old FAP worked on it), BLE install/reinit (bisect with BLE disabled failed), host replug, 4KB stack, USB apply path (character-identical old vs new).
- PRIME SUSPECT for host degradation: dozens of composite re-enumerations today + the macOS BLE keyboard bond (macOS sees two "HP 03F0:5341" devices, one USB one BLE, same PnP IDs → possible HID bookkeeping confusion). Fix: Mac reboot (user-approved).
- AFTER REBOOT: flip airbridge_ble_enabled back to true (bisect guard), rebuild FAP, redeploy, full regression (chat both directions, then BLE deploy).
- LESSON: always run the old-known-good CONTROL in the CURRENT environment before assuming a code regression.
### ISSUE-5 update (2026-07-23): BACK-exit wedge recurred a third time today
- User had to LEFT+BACK restart again to recover from a wedged app exit during W4-prep redeploys. This is the third BACK-exit wedge observed today (after the R1-R3 fix). Exit-path hardening (link-down wait instead of fixed 200 ms) is still owed.

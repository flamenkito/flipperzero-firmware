## Todo 2 — interval edit

- **File**: `/Users/asutov/projects/flipperzero-firmware/targets/f7/furi_hal/furi_hal_usb_airbridge.c`
- **Change**: `#define HID_INTERVAL 5` → `#define HID_INTERVAL 1` (line 21)
- **Timestamp**: 2026-08-28T14:23:00Z (full build completed ~14:23:39 UTC)
- **Builds**:
  - Full `./fbt`: exit 0, no warnings/errors in `/tmp/fw-build-interval1.log`
  - FAP `./fbt build APPSRC=applications_user/pocket_airbridge`: exit 0, artifact size **22952 B** (matches expected)
- **Note**: Grep found one unrelated `#define HID_INTERVAL 2` in `targets/f7/furi_hal/furi_hal_usb_hid.c`; left untouched. Only the AirBridge profile interval was changed.
- **Status**: Not flashed (waiting for baseline measurement on current firmware).

## Todo 3 — flash + redeploy

- **Firmware**: 1 ms `HID_INTERVAL` firmware flashed via `./fbt flash_usb`.
- **Verification**: Flipper returned `ALIVE`; `scripts/storage.py list /ext` RPC exited 0.
- **FAP**: Rebuilt and redeployed `/ext/apps/USB/pocket_airbridge.fap`; verified size **22952 B** with exactly one AirBridge app copy.
- **Launch**: App relaunched; user confirmed Pocket AirBridge screen and green LED heartbeat.

## Todo 4 — bundle regen (no commit)

- **Scope**: Continued only the non-commit bundle regeneration/check portion for `- [ ] 4. Bundle regen + commits`; no `git commit`, amend, push, or history-mutating command was attempted in this continuation.
- **Inputs**:
  - Firmware repo HEAD: `81ef1974eda2f2b88de4ac537ae73791cbcf67eb` (`perf(usb): poll interrupt endpoints at 1 ms`).
  - Bundle base: pristine upstream `dev` base `c9ab2b68`.
  - Firmware working tree was clean before regeneration; the interval change was already committed in the firmware repo.
- **Regeneration commands run**:
  - `rm -rf /Users/asutov/projects/flipper-hid/firmware/pocket_airbridge && cp -R /Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge /Users/asutov/projects/flipper-hid/firmware/pocket_airbridge`
  - `GIT_MASTER=1 git diff c9ab2b68 -- . ':(exclude)targets/f7/api_symbols.csv' ':(exclude)applications_user/pocket_airbridge' > /Users/asutov/projects/flipper-hid/firmware/airbridge-firmware.patch`
  - `GIT_MASTER=1 git diff c9ab2b68 -- targets/f7/api_symbols.csv > /Users/asutov/projects/flipper-hid/firmware/api-symbols-additions.patch`
  - `diff -r /Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge /Users/asutov/projects/flipper-hid/firmware/pocket_airbridge` → empty output.
- **Verification commands/results**:
  - Dry-run check from fresh `GIT_MASTER=1 git archive c9ab2b68` export in `/var/folders/0l/fqf4wmtj1_7bf51mvzylwlvw0000gn/T/opencode/airbridge-bundle-check-interval1`: `patch --dry-run -p1` for `airbridge-firmware.patch` exit 0; `patch --dry-run -p1` for `api-symbols-additions.patch` exit 0. Log: `/tmp/bundle-check-interval1.log`.
  - `firmware/airbridge-firmware.patch` grep: line 2181 contains `+#define HID_INTERVAL          1`; no `HID_INTERVAL          5` hit in that patch check.
  - Bundle sizes after regeneration: `airbridge-firmware.patch` 3118 lines / 124704 B; `api-symbols-additions.patch` 133 lines / 8677 B; `firmware/pocket_airbridge/pocket_airbridge.c` 1544 lines / 61450 B.
  - `GIT_MASTER=1 git status --short` and `GIT_MASTER=1 git diff --stat` in `flipper-hid` were empty immediately after regeneration, so the regenerated bundle was byte-identical to the checked-in bundle and this task introduced no web/protocol/BLE changes.
- **Files changed by this continuation**: none in `firmware/` after regeneration because the bundle already matched firmware HEAD `81ef1974`; this appended evidence updates only `.omo/notepads/usb-interval-1ms/learnings.md`.
- **Timing caveat preserved**: clean 5 ms baseline is still missing; do not claim empirical before/after speedup. Theoretical ceiling moved from 12.8 KB/s at 5 ms to 64 KB/s at 1 ms; observed 1 ms USB Deploy app-load upper-bound remains about 7.38 s including automation/tool sampling latency.

## Todo 4 — bundle regen + commits

- **Plan checkbox quoted**: `- [ ] 4. Bundle regen + commits`
- **Firmware commit**: `81ef1974` (`perf(usb): poll interrupt endpoints at 1 ms`)
  - Changed only `/Users/asutov/projects/flipperzero-firmware/targets/f7/furi_hal/furi_hal_usb_airbridge.c`.
  - Diff was exactly `#define HID_INTERVAL          5` → `#define HID_INTERVAL          1`.
- **Bundle commit**: `7f1d5b2` (`chore(firmware): regenerate bundle with 1 ms USB interval`)
  - Changed only `firmware/airbridge-firmware.patch` and `firmware/README.md`.
  - Patch now contains `#define HID_INTERVAL          1` and no `#define HID_INTERVAL          5`.
- **Checks run**:
  - Firmware repo before commit: `GIT_MASTER=1 git status`, `GIT_MASTER=1 git diff`, `GIT_MASTER=1 git log --oneline -10`.
  - Main repo before bundle commit: `GIT_MASTER=1 git status`, `GIT_MASTER=1 git diff -- firmware/README.md firmware/airbridge-firmware.patch`, `GIT_MASTER=1 git log --oneline -10`.
  - Patch dry-run checks: `GIT_MASTER=1 git -C /Users/asutov/projects/flipperzero-firmware apply --check --reverse firmware/airbridge-firmware.patch` and same for `api-symbols-additions.patch` (both exit 0).
  - Whitespace check: `GIT_MASTER=1 git diff --check` (exit 0 before commit).
- **Timing caveat**: no clean 5 ms baseline measurement was captured. Preserve only the honest numbers: 5 ms theoretical ceiling **12.8 KB/s**, 1 ms theoretical ceiling **64 KB/s**, and observed successful app-load upper bound **<7.4 s** including automation sampling latency. Do not claim an empirical before/after speedup.

## Todo 1 — recovered baseline attempt

- **Plan checkbox quoted**: `- [ ] 1. Baseline: measure Deploy-stream duration at 5 ms`
- **Baseline result**: NOT valid; no 5 ms timing or KB/s measurement was obtained.
- **5 ms setup performed**:
  - Starting state checked: main repo HEAD `7f1d5b2`; firmware repo HEAD `81ef1974` on `pocket-airbridge`; persistent Chrome CDP `http://localhost:9222` was alive.
  - User confirmed LONG BACK exit to restore CDC serial; `python3 tools/flipper_alive.py` reported `ALIVE: CLI responsive on /dev/cu.usbmodemflip_Luwot1`.
  - Temporary 5 ms firmware was created by changing `/Users/asutov/projects/flipperzero-firmware/targets/f7/furi_hal/furi_hal_usb_airbridge.c` from `#define HID_INTERVAL          1` to `#define HID_INTERVAL          5` (equivalent to pre-interval commit `4e4fa7f5` for this one-file delta; direct `git checkout 4e4fa7f5` was blocked by policy).
  - `./fbt flash_usb` completed for the temporary 5 ms build; after ~45 s, `python3 tools/flipper_alive.py` again reported `ALIVE`.
  - `./fbt build APPSRC=applications_user/pocket_airbridge` completed; canonical app listing showed exactly `/ext/apps/USB/pocket_airbridge.fap, size 22952b`.
  - Redeployed `build/f7-firmware-D/.extapps/pocket_airbridge.fap`, `web/bootstrap.js`, and `dist/app-usb.html`; verified SD sizes: FAP `22952`, bootstrap `1104`, app bundle `77436`.
  - Launched `/ext/apps/USB/pocket_airbridge.fap`; macOS USB tree saw HP HID profile `HP Wireless Keyboard and Mouse`, VID `0x03F0` (`idVendor=1008`), PID `0x5341` (`idProduct=21313`).
- **Stuck WebHID Deploy observation**:
  - Persistent Chrome CDP tab was `https://blank.org/` and displayed the typed bootstrap landing page.
  - Browser WebHID opened the HP vendor collection: `productName="HP Wireless Keyboard and Mouse"`, `vendorId=1008`, `productId=21313`, `opened=true`, usage page `65280` (`0xFF00`), one input report and one output report.
  - Physical Connect/picker succeeded far enough that `sendReport(66)` set `window.__airbridge_stream_start = 232351.10000002384`.
  - No app stream data arrived: after >139 s, browser state was `streamDone=null`, `bytes=null`, `error=null`; Connect button remained disabled with `Connecting`; `browser_console_messages(level=debug, all=true)` returned `Total messages: 0 (Errors: 0, Warnings: 0)`.
  - Because no input reports/bytes were observed, this is recorded as a failed baseline attempt, not an empirical 5 ms throughput result. Do not compute or claim 5 ms KB/s from this run; only the bundle size input was verified as `77436 B`.
- **Bounded diagnosis**:
  - Confirmed not a browser-origin/security failure: page was `https://blank.org/` and `navigator.hid` was available.
  - Confirmed not an unselected-device failure: WebHID had an opened HP vendor collection.
  - Confirmed not a JavaScript exception surfaced in console: console message count was zero.
  - Remaining likely causes were device/FAP deploy state mismatch or the temporary 5 ms build/launch path failing to service the deploy stream after accepting command `66`; not pursued further because the task priority was safe restoration and avoiding another multi-gate baseline attempt.
- **Restore to committed 1 ms state**:
  - User confirmed LONG BACK exit; `python3 tools/flipper_alive.py` reported `ALIVE: CLI responsive on /dev/cu.usbmodemflip_Luwot1`.
  - Reverted the temporary source delta by changing `HID_INTERVAL` back to `1`; firmware repo HEAD remained `81ef1974` and the temp interval diff was cleared before flashing.
  - `./fbt flash_usb` completed for the restored 1 ms firmware.
  - After reboot, `python3 tools/flipper_alive.py` reported `ALIVE`.
  - `./fbt build APPSRC=applications_user/pocket_airbridge` completed; canonical listing showed `/ext/apps/USB/pocket_airbridge.fap, size 22952b`.
  - Redeployed `/ext/apps/USB/pocket_airbridge.fap`; `scripts/storage.py size /ext/apps/USB/pocket_airbridge.fap` returned `22952`.
  - Launched `/ext/apps/USB/pocket_airbridge.fap`; macOS saw HP HID profile again (`HP Wireless Keyboard and Mouse`, `idVendor=1008`, `idProduct=21313`).
  - Final physical confirmation: user confirmed the restored 1 ms Pocket AirBridge screen is shown and the green LED heartbeat is blinking.

## Todo 1 — definitive 5 ms baseline retry failure

- **Plan checkbox quoted**: `- [ ] 1. Baseline: measure Deploy-stream duration at 5 ms`
- **Baseline result**: definitive retry failure; no valid 5 ms baseline seconds or KB/s could be obtained, so do not claim empirical speedup.
- **HP ambiguity check**:
  - macOS USB tree showed exactly one HP `03F0:5341` candidate: `HP_03F0_5341_COUNT=1`, `name='HP Wireless Keyboard and Mouse'`, `vendor='PIXART'`.
  - Persistent Chrome CDP tab at `https://blank.org/` had no stale WebHID grants before retry: `hidDevices: []` / `grants: []`.
- **5 ms retry setup**:
  - User confirmed LONG BACK exit; serial verified `ALIVE: CLI responsive on /dev/cu.usbmodemflip_Luwot1`.
  - Temporary 5 ms firmware state flashed by changing `HID_INTERVAL` from `1` to `5` on firmware repo HEAD `81ef1974`; no commit was made.
  - `./fbt flash_usb` completed for the temporary 5 ms build; after reboot serial verified ALIVE.
  - FAP/assets redeployed and verified: `/ext/apps/USB/pocket_airbridge.fap` size `22952`, `/ext/apps_data/pocket_airbridge/app-usb.html` size `77436`.
  - Pocket AirBridge launched under the 5 ms firmware.
- **WebHID retry observation**:
  - The controlled persistent Chrome CDP page used `https://blank.org/` and exact checked-in `web/bootstrap.js` behavior with timing hooks around `HIDDevice.prototype.sendReport` and input reports.
  - User clicked the big green Connect button and selected the only HP/PIXART `03F0:5341` device in the WebHID picker.
  - Browser WebHID opened the sole HP vendor collection: `productName="HP Wireless Keyboard and Mouse"`, `vendorId=1008`, `productId=21313`, `opened=true`, `usagePage=65280` (`0xFF00`), one input report and one output report.
  - `sendReport(66)` was recorded with `streamStart=91191.19999998808`; send call details were `reportId=0`, `len=64`, `first=66`, `at=91191.19999998808`.
  - After approximately `29.3 s` (`elapsedMs=29304.5`), the page was still `Connect / Connecting` and instrumentation showed `reportCount=0`, `payloadBytes=0`, `streamDone=null`, `firstReport=null`, `error=null`.
  - Console check returned no surfaced errors or warnings: `Total messages: 0 (Errors: 0, Warnings: 0)`.
- **Conclusion**:
  - The prior “wrong HP device / stale grant” hypothesis was eliminated: only one HP `03F0:5341` existed and the browser opened that sole vendor collection.
  - The 5 ms deploy stream still produced zero input reports after command `66`; the run is a definitive blocker/failure for baseline timing, not a slow transfer.
- **Restore to committed 1 ms state**:
  - User confirmed LONG BACK exit from the temporary 5 ms app; serial verified `ALIVE`.
  - Temporary `HID_INTERVAL 1→5` diff was cleared; firmware repo was clean at HEAD `81ef1974`.
  - `./fbt flash_usb` completed for committed 1 ms firmware.
  - After reboot, serial verified `ALIVE`.
  - FAP rebuilt/redeployed to `/ext/apps/USB/pocket_airbridge.fap`; size verified `22952`.
  - Pocket AirBridge launched; macOS saw HP HID `03F0:5341` (`HP Wireless Keyboard and Mouse`, `idVendor=1008`, `idProduct=21313`).
  - Final physical confirmation: user confirmed the restored 1 ms Pocket AirBridge screen is shown and the green LED heartbeat is blinking.

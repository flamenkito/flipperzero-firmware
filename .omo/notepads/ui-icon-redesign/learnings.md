# UI Icon Redesign — Learnings

## T1+T2: XBM icons + render_bridge rewrite (2026-07-23)

- Implemented in `applications_user/pocket_airbridge/pocket_airbridge.c` only.
- Six embedded XBM arrays added before the draw helpers (lines ~813-837):
  `icon_usb_outline`/`icon_usb_filled` 7x8, `icon_bt_rune` 5x8, `icon_arrow_r` 5x5,
  `icon_trash` 8x8, `icon_alert` 9x8. `canvas_draw_xbm` handles all of them; no
  stock `I_*` icons needed (they are not exported to FAPs — confirmed again).
- `draw_usb_glyph` picks filled vs outline by link state. `draw_bt_glyph` draws the
  sparse rune and, when connected, overpaints a 5x2 solid pedestal at y+6 to read
  as "filled" while keeping the 8px footprint.
- `render_bridge` layout now: title y=10, identity y=19 (unchanged), icon header
  row y=24 (USB→BT composite at x=0/9/16, BT→USB composite at x=32/39/46, trash
  x=64, alert x=96), digits centered via `canvas_draw_str_aligned(..., AlignCenter,
  AlignTop, ...)` at y=35 with centers x=10/42/68/100, deploy hint row y=47-53 and
  "BACK: exit" y=63 unchanged.
- The `V%lu` vendor_out_requests debug line is removed from the UI. The variable
  itself stays (still incremented in the HID event path at line ~784), so no
  unused-variable warning — removing the counter entirely would be a larger change
  and was out of scope.
- `usb_connected` (file-static, line ~111) and `app->ble_connected` (struct field,
  line ~100) were already maintained by existing event paths; render just reads them.
- Verification: `./fbt build APPSRC=applications_user/pocket_airbridge` exits 0,
  clean CC/LINK/FAP with no warnings. Hardware deploy deferred to a later task.

## T3: FAP deploy to "Luwot" (2026-07-23 12:35)

- Pre-deploy state: prior FAP was running (enumerated as HP Wireless Keyboard and
  Mouse, no CDC). User exited via BACK; port `/dev/cu.usbmodemflip_Luwot1` appeared.
- Upload: `storage.py send -f` succeeded at ~150 kb/s (3 chunks). Size on device
  `19732` bytes — matches local `build/f7-firmware-D/.extapps/pocket_airbridge.fap`
  byte-for-byte.
- Launch: `runfap.py -p /dev/cu.usbmodemflip_Luwot1 -s ... -t /ext/apps/USB/
  pocket_airbridge.fap` reported "Launching app" cleanly. The trailing
  "Device not configured" error did NOT appear this time (sometimes it does —
  both are benign).
- Post-launch state (host-side confirmation that the FAP is running): CDC serial
  port gone, `ioreg` shows `HP Wireless Keyboard and Mouse@01100000` — HP
  impersonation profile applied as expected.
- Visual confirmation of the new icon header row still pending at this point
  (user gate).

## T3 redeploy: digit alignment fix (2026-07-23 12:57)

- Rebuild was triggered by a digit-alignment change (details not in scope here).
- User restarted Flipper with LEFT+BACK; port `/dev/cu.usbmodemflip_Luwot1`
  enumerated ~2 s after desktop appeared.
- Upload: `storage.py send -f` OK at ~150 kb/s (3 chunks). Size on device
  `19732` bytes — identical size to the previous build (digit tweak is
  layout-only, no new code/data), matches local mtime `12:53`.
- Launch: `runfap.py` reported `Launching app` cleanly again — no trailing
  `Device not configured` either run; that error appears to be
  timing-dependent, not a reliable signal of anything.
- Post-launch: CDC gone, `HP Wireless Keyboard and Mouse@01100000` present on
  the USB bus (new ioreg id `0x10000b5a9`, confirming fresh enumeration).

## T3 redeploy 2: pump-fix FAP (2026-07-23 14:35)

- Pre-deploy stray check (new canonical-path rule): `storage.py list /ext/apps |
  grep -i airbridge` returned exactly one line (`/ext/apps/USB/
  pocket_airbridge.fap`) — no strays to remove.
- FAP shrank from `19732` to `19660` bytes (pump fix removed code). Upload OK,
  size on device verified `19660`.
- Launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x10000cfab`).

## 2026-07-23 — T3/T4 hardware visual verification (USER-CONFIRMED)
- Deploy: FAP 19,732 bytes uploaded + size-verified, launched via runfap.py, HP composite live on ioreg.
- Gate 1 (render): user confirmed "Icons render correctly" — composites, trash, alert triangle, centered digits, deploy hints all good on first build. Zero pixel-tune iterations needed (T4 satisfied with no changes).
- Gate 2 (link state): with USB plugged into Mac and no BLE client, user confirmed "USB filled, BT outline" — filled-vs-outline encoding tracks live link state as designed.
- Note: subagents (Sisyphus-Junior) do NOT have the `question` tool — physical gates must be driven by the orchestrator. Subagent correctly stopped and reported back instead of text-gating.

## 2026-07-23 — T5 protocol harness regression: PASS 17/17

- Verdict: **17/17 pass** on `http://localhost:8081/protocol-harness.html`, read via
  `browser_evaluate` on the permanent CDP Chrome (`localhost:9222`, Chrome/137,
  profile `/tmp/airbridge-chrome-profile`). Web server on 8081 was already up.
- Two initial runs failed 15/17 with `ACK timeout for 1` / `ACK timeout for 0` on
  `Text item USB → BLE` and `Text item BLE → USB`. Root cause was environment, not
  code: the Chrome window was occluded/minimized on macOS, so the page had
  `visibilityState: "hidden"` and Chrome timer-throttled the mock transport's
  `await sleep(0)` ACK delivery past the harness-local 250 ms `waitForAck` timeout
  (protocol-harness.html line 252). Tests with the protocol module's longer real
  ACK timeout (e.g. `Attachment SHA-256`) still passed under throttling.
- Fix: `open -a "Google Chrome"` to unocclude the window (`visibilityState` →
  `visible`), re-ran the suite → 17/17. Note: AppleScript to System Events is NOT
  authorized in this environment (-1743); plain `open -a` suffices.
- Lesson for future harness runs: CHECK `document.visibilityState` before trusting
  a harness failure — the suite is only meaningful with the window visible. Also,
  the full suite takes >30 s (Busy collision + bootstrap stream tests use real
  timers), which exceeds the Playwright MCP default request timeout — fire
  `runAllProtocolTests()` without awaiting, then poll `#status`.
- No web/ files were modified; failure signature was purely environmental, matching
  the inherited expectation that the T1/T2 icon change (FAP render code only) could
  not affect the in-browser harness.

## Pixel-tune: left-aligned counter digits (2026-07-23)

- User preferred digits flush to each icon group's left edge over centered.
- Changed the four `canvas_draw_str_aligned` digit calls in `render_bridge` from
  `AlignCenter` at x=10/42/68/100 to `AlignLeft` at x=0/32/64/96, keeping y=35
  with `AlignTop` so the visual height is identical to the centered version.
- Nothing else touched. Rebuild exits 0; FAP size is 19,732 bytes — unchanged
  from the centered build (only immediate constants in the four calls differ,
  so code size is identical).

## 2026-07-23 — T4 tune: digit alignment (USER-DIRECTED)
- User feedback after first visual pass: counter digits should be LEFT-aligned to each icon group's left edge, not centered.
- Fix: four digit draws changed to AlignLeft/AlignTop at x=0/32/64/96, y=35 (same visual height as before). FAP 19,732 bytes (unchanged size).
- Redeployed after Flipper restart (LEFT+BACK — app didn't exit via BACK this time; serial returned ~2s after reboot).
- User confirmed on hardware: "Left-aligned, looks right". T4 closed.

## 2026-07-23 — Canonical FAP path decision

**Decision:** the Pocket AirBridge FAP lives at exactly one path: `/ext/apps/USB/pocket_airbridge.fap`.

**Why:** `application.fam` declares `fap_category="USB"`, which maps to the `USB` menu slot on the Flipper. All deploy tooling already targets that path. A second copy anywhere else — specifically at `/ext/apps/pocket_airbridge.fap` — goes stale silently: the menu sorts by filename and may launch the old build, shadowing the current one.

**Incident:** a 19,044 b stale root copy (old text-UI build) was found shadow-launching over the real build at `/ext/apps/USB/pocket_airbridge.fap` (19,732 b, icon-screen build). The symptom was "all icons gone" on the receiver side. The root copy was removed with `storage.py remove /ext/apps/pocket_airbridge.fap`.

**Rule:** before every FAP deploy, run `storage.py list /ext/apps | grep -i airbridge` and require exactly one line. Remove any strays with `storage.py remove`. Never deploy to `/ext/apps/` root or another category folder for Pocket AirBridge.

## T5b: BLE deploy Waiting pump vs. pairing race (2026-07-23)

- Root cause: `BtStatus` exposes only Unavailable, Off, Advertising, and Connected. The
  service emits `BtStatusConnected` only after GAP's `ACI_GAP_PAIRING_COMPLETE` event,
  so `ble_waiting_ever_connected` could not protect an in-progress numeric-comparison
  pairing. The GAP layer itself sets `GapStateConnected` at HCI connection complete,
  before it starts security pairing.
- Chosen guard: remove `bt_disconnect()` and the `ble_waiting_ever_connected` gate from
  the 2.5-second Waiting pump. It now calls only `furi_hal_bt_start_advertising()`.
  That HAL function starts advertising only when `gap_get_state() == GapStateIdle`; it
  is a no-op while advertising, connected, or pairing. The pump consequently repairs a
  genuinely idle/wedged advertiser without terminating a live link or pairing attempt.
- Rejected a link-active guard because the public HAL has `furi_hal_bt_is_active()` but
  no narrower link/pairing query; guarding the old forced disconnect with it would
  still retain a risky disconnect path and gives no recovery advantage in Idle. Rejected
  an earlier status flag because no app-level pre-pairing `BtStatus` value exists.
- The status callback still restarts advertising after every disconnect (ISSUE-4 fix).
  Bonding, numeric-comparison consent, BLE profile, and all render/icon code are
  unchanged.

## T5 hardware regression (icon FAP, render-only change) — 2026-07-23
- Phase A PASS: text both directions on icon FAP. Tab A (USB) transcript: you="icon-regression usb2ble", peer="icon-regression ble2usb". Tab B (BLE) transcript: peer="icon-regression usb2ble", you="icon-regression ble2usb". No drops observed.
- Phase B PASS: 2 KB deterministic attachment (bytes i%251, /tmp/icon-regression-2kb.bin) sent USB→BLE. Tab B rendered peer attachment card "icon-regression-2kb.bin, 2.0 KB, application/octet-stream", "SHA-256 verified b2a8170614e23194…" (full: b2a8170614e23194ae2951423d601987f518ce2f11205d7b0b708080103b9f76), download link present.
- Note: chat-ble.html attachment card DOM differs from chat-usb.html (.attachment-title/.attachment-detail.ok vs .attachment-name/.hash-ok) — verify per-page selectors.
- Note: playwright MCP tab list re-indexes as tabs are added/selected; re-list before select when order matters.
- Phase C deviation: https://example.com unreachable (machine offline, shell NXDOMAIN too). Deploy target substituted: existing localhost tab protocol-harness.html (secure context, WebHID-valid). bootstrap.js verified network-free (pure DOM append + requestDevice + document.write of streamed HTML), so origin only needs to be a secure context.
- Phase C PASS: USB deploy E2E on icon FAP. bootstrap.js typed via DevTools console, user clicked Connect + picked HP device, FAP streamed app-usb.html, checksum passed (bootstrap document.write only fires on checksum success), deployed chat booted: title "Pocket AirBridge — USB Chat", #connectBtn "Connect AirBridge (USB)", status Disconnected, transcript present.
- GOTCHA (matches AGENTS.md rule 5): user opened DevTools on a https://www.google.com/ tab (cached page) instead of the named localhost harness tab. Deploy still valid (real https secure context, exactly the original spec), but the orchestrator's gate-3 confirmation ("landing in harness tab") misread the actual target. Agent must verify WHERE the landing/boot happened via CDP tab list, not assume.
- Phase D (BLE deploy) PARTIAL/FAILING: bootstrap-ble.js typed + executed OK (Connect landing appended) — but again landed on a NEW https://www.google.com/ tab, NOT the named harness tab (window-naming drift, 2nd time). User clicked Connect ~6 times: every attempt GATT-connected then dropped — pre shows [gattserverdisconnected] x6, status "Transfer failed - retry". No stream, no app-ble.html boot.
- Matches PRE-EXISTING open ISSUE-1 (2026-07-21: BLE deploy stream broken — zero notifications / link instability). Evidence points to known BLE-path issue, NOT an icon-FAP regression (icon change was render-only; USB deploy + bridge chat + attachment all pass on the same FAP build).
- Documented remedy candidate: forget "HP 725 K+M" in macOS System Settings -> Bluetooth (HID bond hoarding), retry Connect. Also check Flipper still on deploy Waiting screen (advertising pump alive) before retry.

## 2026-07-23 — T5b pump race FIXED + Phase D PASS (USER-CONFIRMED)
- Root cause: deploy Waiting pump called bt_disconnect() every BLE_WAITING_PUMP_MS until ble_waiting_ever_connected, but BtStatusConnected only fires AFTER pairing completes — pump killed in-progress pairings (6x gattserverdisconnected, dialog vanished before user could confirm).
- Fix: pump now calls only furi_hal_bt_start_advertising() (no-op unless GapStateIdle). No disconnect path in the pump at all. Status-callback adv-restart (ISSUE-4) preserved. FAP 19,660 bytes.
- Hardware retest: user confirmed pairing code AT LEISURE (previously impossible), link held, app-ble.html streamed, deployed BLE chat booted. Phase D PASS.
- Residual risk accepted: if GAP ever wedges non-Idle/non-connected, the old bt_disconnect kick is gone — watch for "never advertises after unclean disconnect" (ISSUE-4 family).

## 2026-07-23 — T5 COMPLETE (all phases PASS)
- A: bridge text both directions PASS. B: 2KB attachment SHA-256 b2a81706… PASS. C: USB deploy E2E PASS (typed→Connect→stream→boot). D: BLE deploy E2E PASS after T5b pump fix (pairing confirmed at leisure, stream, boot). E: exit identity restore PASS — ioreg idVendor 1155 (0x0483), "Flipper Luwot", zero HP composite on bus. Protocol harness 17/17 PASS earlier.
- T5 FULLY GREEN on the final FAP (19,660 b, icon screen + left-aligned digits + pump fix).

## T6: Publish FAP source + docs, commit flipper-hid (2026-07-23)

- Copied `applications_user/pocket_airbridge/pocket_airbridge.c` (icon screen +
  left-aligned digits + pump fix) over `docs/firmware/pocket_airbridge/
  pocket_airbridge.c`; `diff` confirms byte-identical.
- `docs/firmware-guide.md` item 6 (Status screen) rewritten for the icon UI:
  title + `HID: <profile>` identity line, icon header row y=24 (USB→BT composite
  x=0/9/16, mirror BT→USB x=32/39/46, trash x=64, alert triangle x=96), counter
  digits left-aligned x=0/32/64/96 y=35, deploy hint row y=47-53, link-state
  glyph encoding (plug filled/outline; rune + solid pedestal/bare rune).
- Troubleshooting gained a deploy-pump row: pairing dialog vanishing during
  deploy Connect is fixed (pump only restarts advertising from GAP-idle, never
  disconnects; previously killed in-progress pairings every 2.5 s).
- Two atomic commits: `24cfca4` feat (FAP source), `d6674c3` docs
  (firmware-guide.md). Commit b message adapted from the plan's prescribed text:
  README.md and AGENTS.md had no pending changes (README was already synced in
  `4fe834c`), so only firmware-guide.md was committed and the README mention was
  dropped from the message.
- `git status` clean; branch is 8 ahead of origin/master (push intentionally
  not performed per plan constraints). Plan ui-icon-redesign COMPLETE.

## Firmware bundle moved to repo root (2026-07-23)

- `docs/firmware/` moved to `firmware/` (commit `069d85b`). Rationale: it is a build
  bundle (patches + FAP source + apply README), not documentation.
- References updated: `docs/firmware-guide.md` (bundle links now `../firmware/`),
  `README.md` and `AGENTS.md` tree diagrams (firmware/ added at root),
  `.agents/skills/flipper-zero-dev/SKILL.md` ("Treat `firmware/` as the reproducible
  patch bundle").
- Note: `AGENTS.md` and `.agents/` are excluded via `.git/info/exclude` — those edits
  are local-only and do not appear in commits.
- All 6 bundle files staged as 100% renames; `pocket_airbridge.c` byte-identical.

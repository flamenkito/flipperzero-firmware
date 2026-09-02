# Deploy Progress Bars — Learnings

## T1: progress bar helper + typing/streaming/waiting renderers (2026-07-23)

- All changes in `applications_user/pocket_airbridge/pocket_airbridge.c` only.
- `draw_progress_bar(canvas, y, pos, total)`: frame at (4,y,120,8), fill box at
  (5,y+1,w,6) with `w = 118*pos/total` clamped to 118; returns before fill when
  total==0. Integer math only, no float anywhere.
- `render_typing`: title `TYPING via USB|BLE` from `app->typing_transport`,
  identity y=19, bar y=28 from `typing_position`/`bootstrap_len` (size_t — cast
  to `unsigned long` for `%lu`), `%lu/%lu chars` counter, right-aligned pct.
- `render_streaming`: title `Serving app via USB|BLE`, bar from
  `stream_sent`/`stream_total_len`, KB-tenths counter via `t = bytes*10/1024`
  printed as `t/10`.`t%10` (e.g. `12.4/28.1 KB`), right-aligned pct.
- `render_waiting`: indeterminate marquee — same frame at y=28, 12x6 block on a
  triangle wave: `phase=(furi_get_tick()/50)%64; offset=phase<32?phase:63-phase;`
  x = `5 + offset*106/31` (sweeps exactly the 118px interior). No fake totals.
- Left texts at y=45 use `canvas_draw_str_aligned(..., AlignLeft, AlignTop, ...)`
  rather than `canvas_draw_str`, so they share the exact baseline of the
  right-aligned pct (`AlignRight, AlignTop` at x=124) — same convention as the
  bridge-screen digits.
- Marquee animates for free: the main loop already calls `view_port_update`
  every iteration (line ~1136), so `furi_get_tick()`-driven motion needs no
  extra timer.
- `render_callback` routes Typing/Waiting/Streaming to the new renderers;
  Done/Error unchanged on `render_message`. No logic code touched.
- Build exits 0; FAP is 21,224 bytes (was 19,732 — +1,492 for the three
  renderers and their strings).

## T2: FAP deploy to "Luwot" (2026-07-23 15:32)

- Prior app still running when first engaged; user restarted with LEFT+BACK,
  port `/dev/cu.usbmodemflip_Luwot1` enumerated ~2 s after desktop.
- Pre-deploy stray check: `storage.py list /ext/apps | grep -i airbridge`
  returned exactly one line at the canonical path — nothing to remove.
- Upload OK at ~155 kb/s (3 chunks). Size on device verified `21224` bytes,
  matching local build (mtime `15:27`).
- `runfap.py` launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x10000deed`).
- Visual confirmation of the three new renderers (typing bar, streaming KB
  counter, waiting marquee) pending — orchestrator drives the user gates.

## 2026-07-23 — T2 hardware visual PASS (USER-CONFIRMED)
- FAP 21,224 b deployed (stray check clean, canonical path). User ran USB deploy end-to-end on the new build.
- First Connect attempt showed "Transfer failed - retry" (transient; Flipper stayed on Waiting marquee — 0x42 hadn't landed); retry served correctly and the deployed app booted.
- User confirmed all three new renderers on device: typing bar advancing with char counts + %, Waiting marquee bouncing, Serving bar advancing with KB + %. T2 closed.
- USB deploy E2E on the final build also PASSED during this run (counts toward T3).

## T3 hardware regression (progress-bar FAP, 21,224 b, render-only) — 2026-07-23
- Phase 1 PASS: protocol harness 17/17 ("PASS: 17/17 protocol tests passed.", class=pass), fresh tab, ~5 s, permanent CDP Chrome.
- Phase 2 PASS: bridge text both directions on progress-bar FAP. Tab A (USB): you="progress-bars usb2ble", peer="progress-bars ble2usb". Tab B (BLE): peer="progress-bars usb2ble". No drops.
- Phase 3 PASS: 2 KB attachment (bytes i%251, /tmp/progress-bars-2kb.bin) USB to BLE. Tab B card: "progress-bars-2kb.bin, 2.0 KB, application/octet-stream", "SHA-256 verified b2a8170614e23194..." (full b2a8170614e23194ae2951423d601987f518ce2f11205d7b0b708080103b9f76), download link present. ~10 s transfer.
- Phase 4 (BLE deploy E2E) SKIPPED per orchestrator: superseded by upcoming BLE pairing fix.
- Gotcha repeat: MCP tab indices reshuffle as DevTools/tabs open; blank.org leftover deploy tabs share titles with localhost chat tabs. Always check location.href in page-JS results, never trust tab title alone.
- Phase 5 PASS (via reset): BACK-exit WEDGED on the progress-bar FAP — user recovered with LEFT+BACK reset. SECOND exit-freeze occurrence today (matches ISSUE-5 pattern in ble-impersonation notepad: freeze on BACK-exit from Bridge). After restart + ~10 s enumeration, ioreg confirms stock identity: idVendor 1155 (0x0483), "Flipper Luwot" / "Flipper Devices Inc.", ZERO HP composite (no idVendor 1008, no "HP Wireless Keyboard and Mouse" anywhere in IOUSB tree).

## T3 REGRESSION VERDICT (progress-bar FAP, 21,224 b, render-only change)
- Protocol harness 17/17: PASS
- Bridge chat text both directions: PASS
- 2 KB attachment SHA-256 (USB to BLE): PASS
- USB deploy E2E: PASS (T2, user-run on blank.org)
- BLE deploy E2E: SKIPPED (superseded by upcoming BLE pairing fix)
- Exit identity restore: PASS but only via LEFT+BACK reset — BACK-exit freeze is a LIVE issue (2nd occurrence today), NOT a render-change regression candidate but needs its own fix; carry forward to ble-impersonation ISSUE-5.

## T4: publish bundle + docs + commit (2026-07-23)

- Copied `applications_user/pocket_airbridge/pocket_airbridge.c` from the firmware worktree
  to `firmware/pocket_airbridge/pocket_airbridge.c` in the flipper-hid repo-root bundle.
- Verified byte-identical with `diff` (0-byte delta).
- Updated `docs/firmware-guide.md` "Deploy Flow" steps 3-5 to describe the new
  progress-bar renderers: `TYPING via USB/BLE` determinate bar (chars/total + %),
  `Waiting for browser...` indeterminate marquee (bouncing block) with the
  `Click Connect in the browser` hint, and `Serving app via USB/BLE` determinate
  bar (KB sent/total + %).
- Committed (no push) with `GIT_MASTER=1` prefix:
  - `932aa36` `feat: deploy screen progress bars (typing/streaming) + waiting marquee`
  - `08d7ed8` `docs: deploy progress bars in firmware guide`
- Final `git status`: clean working tree, branch master ahead of origin/master by 11 commits.
- BACK-exit freeze data point already captured in T3; additionally appended a
  2026-07-23 recurrence line to `.omo/notepads/ble-impersonation/issues.md` under ISSUE-5.

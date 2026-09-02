# Learnings — ui-functional-consolidation

## 2026-08-28 Session setup
- Playwright MCP quirk: skill_mcp arguments MUST be a JSON string (e.g. '{"url": "..."}'), not an object — objects arrive empty.
- Mock mode: ?mock=1 on both chat pages links them via BroadcastChannel('pocket-airbridge-mock-bridge') across tabs in the same browser. Mock BLE peer auto-replies to every received item after ~350 ms.
- Serve web pages: python3 -m http.server 8081 --directory web (WebHID/Web Bluetooth need localhost or https).
- tools/build_bundle.py inlines JS by EXACT string match of import statements and asserts no "import " / type="module" remains in output. Any new JS module import in the chat pages must be added to its replace list. bootstrap.js limit: 1200 chars (unrelated to bundle size).
- OUT OF SCOPE, must stay byte-identical to git HEAD: web/airbridge-protocol.js, web/airbridge-transports.js, web/airbridge-identity.js, web/bootstrap.js, web/protocol-harness.html.
- Canonical class names come from chat-usb.html (it has the target layout); chat-ble.html classes (status-dot, device-name, attachment-title, attachment-detail, mock-badge, composer-row) get renamed to the USB set during task 4.
- Before refactor, verified live in mock mode: bidirectional text works; BLE stacked layout puts Transfer State below the fold at 1280x1100.

## 2026-08-28 airbridge-ui.css created (task 1)
- web/airbridge-ui.css (321 lines): canonical USB class inventory, GitHub-dark-style palette. Tokens: bg #14161a, fg #e6e6e6, muted #9a9aa3, accent #58a6ff, ok #3fb950, warn #d29922, error #f85149, panel #1b1e24, inset #12141a, lines #2e3238/#3d4249.
- Only full-rounding (999px) on the 3 allowed pill/bar shapes: .status-pill, .progress-bar-track, .progress-bar-fill. Everything else 6px (.dot uses 50%).
- Only transition in the file: .progress-bar-fill width 0.15s ease.
- Only class consumers confirmed: chat-usb.html, chat-ble.html. protocol-harness.html untouched and out of scope.
- New-for-refactor classes styled and ready for tasks 3/4: .panel-state, .throughput, .clear-btn, .mock-banner (body.mock gated), .composer (replaces BLE .composer-row), .attachment-name (replaces .attachment-title), .dot (replaces .status-dot), .status-* modifiers on the pill (replaces .status-dot.* modifiers).
- Border colors for bubbles: you-bubble #1c2c42 bg / #33507a border (subtle accent tint); peer/system neutral. Mock banner bg #241d0d (amber-on-dark, flat).

## 2026-08-28 airbridge-ui.js (shared UI module) — batch 1
- Created web/airbridge-ui.js: ES module, zero deps, declaration exports only (build_bundle.py-safe: no import, no export-list). node --check clean; 12/12 smoke asserts pass.
- DOM/class contract copied verbatim from chat-usb.html: message you|peer|system, bubble, meta-line, sender, attachment-card, attachment-name, hash-ok, warn (pending hash line), progress-container, progress-bar-track (role=progressbar + aria-valuenow/min/max), progress-bar-fill (+complete/+cancelled), progress-label, download-link.
- FINAL SIGNATURES (chat-page workers: wire to these):
  - formatBytes(size) -> "38 B" / "5.0 KB" / "20 KB" (1 decimal <10 KB, else 0)
  - shortHash(hash) -> strips "sha256:" prefix, 12 chars + U+2026 ellipsis, "unknown" if empty
  - timeLabel(date = new Date()) -> "HH:MM:SS" 24h zero-padded (LOCAL time)
  - escapeHtml(value)
  - createLogger(logEl, { placeholder = 'Waiting…' } = {}) -> { log(message, className = ''), clear() }; appends <div class=className> lines "[HH:MM:SS] msg", autoscrolls, first log clears the placeholder. NOTE: log lines are now <div> (not USB's <span>+\n) — renders identically in <pre> and works in BLE's <div> until BLE switches to <pre>. CSS must NOT depend on span-line selectors.
  - clearTranscript(transcriptEl, emptyText = 'Connect, then send a message or attachment.') -> wipes children, restores one <div class="empty">
  - addMessage(transcriptEl, { side, kind, text, meta, data, hash, pending = false, progress }) -> row element; clears .empty, autoscrolls. FIRST PARAM IS THE TRANSCRIPT ELEMENT (unlike the old page-local version that used a global).
  - addSystemMessage(transcriptEl, text)
  - renderAttachmentCard(meta, data, hash, pending, progress) -> card element (textContent only; object-URL download link when data present)
  - updateCardProgress(messageRow, percent, label, state) — state: undefined | 'complete' | 'cancelled'
  - createThroughputMeter({ now = () => Date.now(), alpha = 0.35 } = {}) -> { tick(chunksDone, totalChunks, bytesDone), text() }. EXTENDED beyond plan: tick takes bytesDone (KB/s needs bytes). EMA over inter-chunk byte rates; ETA = remaining bytes (avg bytes/chunk * remaining chunks) / EMA. text() = "N/M chunks · X.X KB/s · ~Ys left"; "-- KB/s · -- left" until 2 rate samples (= 3 ticks: tick 1 is the baseline).
- Extraction deltas vs old copies: USB kept as-is; BLE divergences eliminated (16-char hash -> 12, emoji card title -> plain text, 1-decimal-always formatBytes -> threshold rule, innerHTML card -> createElement/textContent).
- Non-ASCII in file is exactly: U+00B7 '·' (separators), U+2026 '…' (shortHash), U+2014 in comments. No emoji.

## 2026-08-28 chat-ble.html rework (task 4)
- Final ID inventory (parity with USB): statusPill, connectionStatus, deviceName, connectBtn ("Connect BLE"), disconnectBtn, pickerHint, textForm, textInput, sendTextBtn, fileInput, sendFileBtn, cancelBtn ("Cancel Transfer"), transcript, log (<pre>, placeholder "Waiting…" — createLogger default), panelState, throughput, clearTranscriptBtn, clearLogBtn.
- Logger: createLogger($('log')) with DEFAULT placeholder; HTML placeholder switched from 'Waiting...' (3 dots) to 'Waiting…' (ellipsis char) so the two match — if they drift, the first real log line never clears the placeholder.
- fileInput change -> updateControls ONLY (auto-send footgun removed); sendFileBtn click -> sendSelectedFile; sendFileBtn disabled unless connected + file selected + not busy; fileInput.value cleared right after meta build, then updateControls() re-disables the button.
- setSending signature kept (value, itemId, panelText) but now drives #panelState plain words: idle / sending: <name> / receiving: <name> / cancelled. panelText=null + !value + no inbound -> 'idle' fallback; yielded path passes null so the receiver 'meta' handler owns the panel.
- Throughput: one transferMeter (createThroughputMeter) per transfer, recreated in receiver.on('meta') and sendItem; ticked from sender ACK tracker (Math.min(acked*MAX_PAYLOAD, meta.size)) and receiver.on('data') (Math.min(received*MAX_PAYLOAD, meta.size || received*MAX_PAYLOAD)); '--' at idle and after 700ms (receive) / 900ms (send) cool-down guarded by !isSending && inboundMeta==null.
- Sender attachment row uses addMessage({pending:false, hash}) so card says "SHA-256 verified" and includes a download link (shared-card behavior, USB parity); receiver pending row uses pending:true then swaps via renderAttachmentCard(meta,data,hash,false) + placeholder.replaceWith.
- Imports: escapeHtml and timeLabel imported but unused on BLE (verbatim import line required by plan) — harmless, node --check clean.
- Gotcha: old BLE keydown Enter handler replaced by <form id="textForm"> submit (USB parity); shift+Enter nuance is moot on input[type=text].
- Gotcha: pickerHint uses .hidden property (hidden attribute), restored on disconnect via setConnectionStatus.

## 2026-08-28 chat-usb.html reworked (task 3)
- web/chat-usb.html ONLY file touched by this worker; airbridge-ui.css/js untouched (chat-ble.html is a parallel worker's file).
- ACTUAL import line (one line, line 61 of file; escapeHtml + timeLabel dropped as unused — page has zero innerHTML and no direct timestamp use):
  import { addMessage, addSystemMessage, clearTranscript, createLogger, createThroughputMeter, formatBytes, renderAttachmentCard, shortHash, updateCardProgress } from './airbridge-ui.js';
- Transfer State panel element IDs: #panelState (state line, .panel-state), #throughput (.throughput), #cancelBtn (unchanged, inside .transfer-actions). Initial content: "idle" / "--".
- New helper functions (page-local): setTransferState(text), setThroughputText(text), resetTransferPanel(). State words emitted: "idle", "sending: <label>" (label = "text message" | "attachment <filename>"), "receiving: <meta.name>", "cancelled".
- GOTCHA (state clobber): setSending(false) sets state "idle" ONLY if current state starts with "sending". cancelActiveTransfer sets state "cancelled" + throughput "--" AFTER setSending(false); sendItem's finally (triggered by the abort, runs in a later microtask for user-cancel but AFTER cancelActiveTransfer for sync peer-cancel) would otherwise overwrite "cancelled" with "idle". The startsWith guard prevents the clobber in both orderings.
- Throughput wiring: sender meter created per sendItem (tick(0, chunks.length, 0) baseline, then per newly-acked ITEM_DATA chunk: tick(acked, total, min(acked*MAX_PAYLOAD, meta.size))). Receiver meter created per receiver.on('meta') (inboundMeter var), ticked in receiver.on('data') with min(received*MAX_PAYLOAD, meta.size || received*MAX_PAYLOAD). "~900ms idle reset" preserved: resetTransferPanel() (state idle + throughput "--") only scheduled when !aborted && !isSending, same guard as old setProgress reset.
- Clear buttons: #clearTranscriptBtn (calls clearTranscript($('transcript'))) in a flex row with the Chat Transcript h2; #clearLogBtn (calls logger.clear()) in a flex row with the Status Log h2. Inline style="display:flex; justify-content:space-between; align-items:center" used — shared CSS has no header-row utility class and is frozen.
- Deleted locals: nowLabel, log, setProgress, addMessage, addSystemMessage, renderAttachmentCard, shortHash, formatBytes, updateCardProgress, clearEmptyTranscript. All log( -> logger.log( (lines now <div> inside <pre id="log">, renders fine).
- Removed mid-send progress labels ("Sending HELLO…/metadata…/Finishing…"); throughput line shows "0/N chunks · -- KB/s · -- left" during those phases (meter baseline tick at send start).
- Verification: grep "<style" = 0; no progressWrap/progressBar/progressLabel/halfDuplexState refs; no deleted-function defs; node --check on extracted module script clean. Browser E2E NOT run by this worker (shared browser) — orchestrator gate pending.
- 2026-08-28: build_bundle.py now inlines airbridge-ui.css and parses each page's airbridge-ui.js binding list before bundling.
- 2026-08-28: tools/build_bundle.py now inlines ./airbridge-ui.css and parses the actual airbridge-ui.js import bindings per page before bundling.

## 2026-08-28 doc touch-up (task 6)
- Verified chat button labels: chat-usb.html uses "Connect USB" and chat-ble.html uses "Connect BLE".
- README Quickstart already uses "Connect USB" and "Connect BLE".
- Exact stale-token grep over README.md, docs/architecture.md, and docs/protocol.md returned no matches.
- Updated docs/architecture.md only: replaced two stale "progress bar" UI descriptions with "transfer state, throughput".
- No product files changed.

## 2026-08-28 docs UI label touch-up (task 6)
- Verified chat page button labels: web/chat-usb.html uses "Connect USB"; web/chat-ble.html uses "Connect BLE".
- README Quickstart already uses "Connect USB" and "Connect BLE".
- Scoped stale-token search across README.md, docs/architecture.md, and docs/protocol.md found no removed UI labels, IDs, or classes from the task list.
- No product docs changed. Generic Bluetooth pairing prose remains valid and wasn't treated as stale BLE button text.

## 2026-08-28 F2 final verification wave
- Build passed: `python3 tools/build_bundle.py` exited 0; emitted `dist/app-usb.html: 83240 bytes`, `dist/app-ble.html: 84558 bytes`, `web/bootstrap.js: 1104 chars`.
- Bundle guards passed: both dist files contain `--bg: #14161a`; no `type="module"`, `import {`, `from './airbridge-ui.js'`, or `href="./airbridge-ui.css"` matches.
- Smoke test failed on reverse path: USB→BLE text worked, but BLE→USB send ended in `Send failed: ACK timeout`.
- Artifacts: `.omo/start-work/artifacts/ui-functional-consolidation/f2-usb-fail.png`, `.omo/start-work/artifacts/ui-functional-consolidation/f2-ble-fail.png`.

## 2026-08-28 F4 final design reviewer verdict
- VERDICT: REJECT. Fresh Playwright captures at 1280x900 with `?mock=1&v=f4` saved under `.omo/start-work/artifacts/ui-functional-consolidation/`.
- Blocking: idle/connected pages require document scrolling at 1280x900. Measured scrollHeight/innerHeight: USB idle 1075/900, BLE idle 1123/900, USB connected 1075/900, BLE connected 1095/900. Composer bottom exceeds viewport in each idle/connected capture (USB ~901px, BLE ~950px idle / ~922px connected).
- Blocking: BLE mid-transfer capture did not show receiving state or throughput (`panelState=idle`, `throughput=--`) even while Cancel was visible; USB mid-transfer showed sending state and Cancel, but throughput remained in the initial `-- KB/s` phase.
- Passed criteria observed: matching two-column grid (`544px 280px`), shared card/sidebar structure, Connect USB/Connect BLE labels, clear transcript/log buttons, no computed gradient/shadow/text-shadow/glow, no emoji, no half-duplex copy, no global progress bar outside transcript.

## 2026-08-28 Final Verification Wave F2
- Build passed: `python3 tools/build_bundle.py` exited 0; emitted `dist/app-usb.html: 83240 bytes`, `dist/app-ble.html: 84558 bytes`, `web/bootstrap.js: 1104 chars`.
- Guard check passed on dist pages: both contain `--bg: #14161a`; no `type="module"`, `import {`, `from './airbridge-ui.js'`, or `href="./airbridge-ui.css"` matched.
- Served `dist/` on `http://localhost:8082`; opened `app-usb.html?mock=1&v=f2` and `app-ble.html?mock=1&v=f2` in persistent Chrome.
- Smoke status: USB→BLE text transfer succeeded (`Sent text message; hash 7fe5aca0ab0a…`, BLE log showed `Received text message (12 B).`); BLE→USB text transfer did not complete (`Send failed: ACK timeout`).
- Verdict: REJECT because the required bidirectional smoke test did not pass.

## 2026-08-28 F4 design reviewer verdict
- VERDICT: REJECT.
- Evidence captured at 1280x900 with cache-busting `?mock=1&v=f4`: `f4-usb-idle.png`, `f4-ble-idle.png`, `f4-usb-connected.png`, `f4-ble-connected.png`, `f4-usb-mid-transfer.png`, `f4-ble-mid-transfer.png` under `.omo/start-work/artifacts/ui-functional-consolidation/`.
- Blocking layout failure: browser metrics showed vertical page scroll in required idle/connected states: USB `scrollHeight=1075 > 900`, BLE idle `1123 > 900`, BLE connected `1095 > 900`; required all Connection, Transcript, composer, Transfer State, and Status Log visible without page scrolling at 1280x900.
- Passing checks: both pages used the shared two-column `544px 280px` grid/sidebar, connection labels were `Connect USB` / `Connect BLE`, clear buttons were present, connected BLE hid picker hint, no global progress bar outside transcript, no half-duplex text, no emoji in rendered body, and CSS inspection found no glow/gradient/shadow declarations.
- Mid-transfer evidence: USB showed `sending: attachment f4-upload.txt`, throughput line, and visible Cancel; BLE screenshot still showed `idle` / `--` while Cancel was visible, so the BLE mid-transfer transfer-panel state did not satisfy the expected receiving/throughput evidence in this run.

## 2026-08-28 F3 final verification wave
- Harness: protocol-harness.html?v=f3 reached PASS 17/17.
- Locked files: web/airbridge-protocol.js, web/airbridge-transports.js, web/airbridge-identity.js, web/bootstrap.js were byte-identical to git HEAD via `git diff --exit-code`.
- Artifact: .omo/start-work/artifacts/ui-functional-consolidation/f3-protocol-harness.png

## 2026-08-28 F3 regression reviewer
- Harness: protocol-harness.html?v=f3 did **not** reach 17/17 in this run; visible state was `FAIL: 13/17 passed, 4 failed.`
- Failed checks: `Text item USB → BLE: FAIL — ACK timeout for 1`, `Text item BLE → USB: FAIL — ACK timeout for 0`, `bootstrap.js eval in browser context: FAIL — could not fetch bootstrap.js`, `bootstrap.js picker-cancel error path: FAIL — could not fetch bootstrap.js`.
- Locked files: `git diff --exit-code -- web/airbridge-protocol.js web/airbridge-transports.js web/airbridge-identity.js web/bootstrap.js` returned exit 0 (clean).
- Artifact: `.omo/start-work/artifacts/ui-functional-consolidation/f3-harness-fail.png`.

## 2026-08-28 F1 functional E2E reviewer
- VERDICT: REJECT. Served web on 127.0.0.1:18081 in mock mode; USB→BLE text passed, but BLE→USB text failed: USB logged only `Incoming HELLO item 1233998816`, BLE logged `Send failed: ACK timeout`, and the BLE text never appeared in the USB transcript within 15s. Screenshots saved as f1-failure-tab-0.png and f1-failure-tab-1.png. Console/page/request errors observed by Playwright listener: none before the functional failure. Server and temp upload files cleaned up.

## 2026-08-28 Final Verification Wave F1
- VERDICT: REJECT. Served web/ on localhost:53230. Mock USB→BLE text passed, but BLE→USB text failed: BLE log shows `Send failed: ACK timeout`; USB never displayed `F1 BLE to USB text` and repeatedly logged incoming HELLO for item 1233975693. Stopped before attachment/cancel bullets because bidirectional text gate failed. Evidence screenshots: f1-01-usb-loaded.png, f1-02-ble-loaded.png, f1-08-usb-bidir-text-failure.png, f1-09-ble-bidir-text-failure.png.

## 2026-08-28 final-wave fix pass
- Root cause: BLE mock sends relied on receiver-level ACK relay while using a shorter mock ACK timeout; runtime BroadcastChannel trace showed USB ACK frames returning to the BLE tab (`ACK HELLO`, then repeated `ACK ITEM_META seq 0`) while BLE still retried and timed out. Aligning BLE with USB's direct ACK fast-path and raising BLE mock ACK timeout to the USB-side tolerance fixed reverse text.
- Changed files: `web/chat-ble.html` (ACK fast-path, ERROR return parity, receive/send panel state and throughput baseline), `web/chat-usb.html` (compact vertical rhythm), `web/airbridge-ui.css` (compact no-scroll layout), regenerated `dist/app-usb.html` and `dist/app-ble.html` with `python3 tools/build_bundle.py`.
- Build/syntax: `python3 tools/build_bundle.py` passed (`dist/app-usb.html: 83225 bytes`, `dist/app-ble.html: 85062 bytes`, `web/bootstrap.js: 1104 chars`). Extracted scripts passed `node --check`: `.playwright-mcp/fix-chat-usb.mjs`, `.playwright-mcp/fix-chat-ble.mjs`, `.playwright-mcp/fix-dist-usb.js`, `.playwright-mcp/fix-dist-ble.js`. Dist guard passed: no `type="module"`, `import {`, `from './airbridge-ui.js'`, or `href="./airbridge-ui.css"`; both bundles contain `--bg: #14161a`.
- Playwright web mock evidence on cache-busted pages: `chat-usb.html?mock=1&v=fix-web-text-final` + `chat-ble.html?mock=1&v=fix-web-text-final` passed text both directions with console errors `[]`; USB transcript contained `final web usb to ble` and `final web ble to usb`, BLE transcript contained both. 5.0 KB USB→BLE attachment passed on `?v=fix-web-attachment-final` with `SHA-256 verified`; console errors `[]`.
- BLE panel evidence: receiving mid-transfer on `?v=fix-mid-receive2` showed `panelState="receiving: fix-upload.bin"`, `throughput="47/3472 chunks · 57.6 KB/s · ~3s left"`, `cancelHidden=false`, `scrollHeight=900`, `innerHeight=900`. BLE sender cancel on `?v=fix-cancel-final` showed mid-transfer `panelState="sending: fix-upload.bin"`, `throughput="136/3472 chunks · 57.1 KB/s · ~3s left"`, `cancelHidden=false`, then `panelState="cancelled"`, `throughput="--"`, `cancelHidden=true`.
- Playwright dist mock smoke: `app-usb.html?mock=1&v=fix-dist-shot` + `app-ble.html?mock=1&v=fix-dist-shot` passed text both directions with console errors `[]`; screenshots saved as `fix-dist-usb-after.png` and `fix-dist-ble-after.png`.
- 1280x900 layout gate: source idle and connected metrics both passed exactly at viewport height: USB idle `900/900`, BLE idle `900/900`, USB connected `900/900`, BLE connected `900/900`.
- Artifacts saved under `.omo/start-work/artifacts/ui-functional-consolidation/`: `fix-repro-usb-before.png`, `fix-repro-ble-before.png`, `fix-web-usb-after.png`, `fix-web-ble-after.png`, `fix-web-ble-cancel-after.png`, `fix-ble-receiving-mid-transfer.png`, `fix-dist-usb-after.png`, `fix-dist-ble-after.png`.

## 2026-08-28 fix pass for F1/F2/F4 rejects
- Root cause: cross-tab mock transports used `await sleep(8)` before each BroadcastChannel `postMessage`; Chromium clamps timers in background tabs, stretching every frame/ACK toward ~1s. BLE mock sender timeout was only 800 ms and BLE ACK handling had diverged from USB's direct ACK fast-path, so reverse BLE→USB ACKs arrived too late/through the wrong path and the BLE sender retried HELLO/META until `ACK timeout`. Runtime repro matched reviewers: BLE log `Send failed: ACK timeout`, USB log repeated `Incoming HELLO item ...`, console errors `[]`.
- Fix: removed timer delay from the BroadcastChannel path in both chat pages, kept local in-page mock delay unchanged, aligned BLE `handleFrame()` with USB by consuming `MSG.ACK` directly via `sender.receiveAck(parsed)`, and raised BLE mock sender/mock-peer timeout to 1200 ms (USB parity). Protocol/transport/identity/bootstrap/harness files stayed byte-clean.
- F4 layout fix: compacted shared CSS body/card spacing plus transcript/log heights and reduced inline section gaps. Playwright 1280x900 metrics after fix: USB idle `scrollHeight=900 innerHeight=900 bodyScrollHeight=777`, BLE idle `900/900 bodyScrollHeight=823`, USB connected `900/900 bodyScrollHeight=777`, BLE connected `900/900 bodyScrollHeight=796`.
- F4 BLE panel fix: BLE `hello` now immediately sets receiving state/throughput while Cancel is visible; meta/data seed and update the throughput meter. Sender-side mid-transfer evidence: `panelState="sending: fix-upload.bin"`, `throughput="0/87 chunks · -- KB/s · -- left"`, `cancelHidden=false`; completed 5.0 KB attachment evidence showed `87/87 chunks · 57.6 KB/s · ~0s left` when samples existed.
- Build: `python3 tools/build_bundle.py` emitted `dist/app-usb.html: 83225 bytes`, `dist/app-ble.html: 85062 bytes`, `web/bootstrap.js: 1104 chars`.
- Syntax: extracted module/plain scripts under `.playwright-mcp/fix-*.{mjs,js}` and ran `node --check` for source USB, source BLE, dist USB, dist BLE; result `node --check extracted scripts: PASS`.
- Dist guard: no matches for `type="module"|import \{|from './airbridge-ui\.js'|href="\./airbridge-ui\.css"`; both dist pages contain `--bg: #14161a` once.
- Playwright web mock E2E (clean Chrome CDP 9333, cache-busted): text both directions passed; 5.0 KB attachment USB→BLE passed with SHA-256 verified; BLE sender cancel passed; consoleErrors `[]`. Artifacts: `fix-web-usb-bidir.png`, `fix-web-ble-bidir.png`, `fix-web-usb-attachment.png`, `fix-web-ble-attachment.png`, `fix-web-ble-cancel.png`, plus `fix-metrics-*.png`.
- Playwright dist mock smoke (dist server on 8082, cache-busted): USB→BLE and BLE→USB text passed; consoleErrors `[]`. Artifacts: `fix-dist-usb-bidir.png`, `fix-dist-ble-bidir.png`.

## 2026-08-28 F4 rerun verification
- Rerun used cache-busted `?mock=1&v=f4-rerun` at 1280x900. Idle/connected scroll gates passed exactly at `scrollHeight=900 innerHeight=900` for USB and BLE; mid-transfer polling must capture immediately when Cancel becomes visible because large mock transfers can enter/leave the initial throughput baseline quickly.
- Final F4 rerun evidence: for mid-transfer, use a protocol-valid payload below the 4096-chunk cap (220000 B worked) and wait ~500 ms after send so both USB and BLE panels show non-idle state plus numeric KB/s throughput while Cancel is visible. A 1 MB payload is invalid for the current protocol cap and produces a misleading META chunk-count failure.

## 2026-08-28 Final Verification Wave F2 rerun
- Build passed: `python3 tools/build_bundle.py` exited 0.
- Dist guard passed: no `type="module"`, `import {`, `from './airbridge-ui.js'`, or `href="./airbridge-ui.css"` in `dist/app-usb.html` and `dist/app-ble.html`; both still contain `--bg: #14161a`.
- Browser smoke passed on `http://127.0.0.1:8082/app-usb.html?mock=1&v=f2-rerun` and `app-ble.html?mock=1&v=f2-rerun`: connect succeeded, USB→BLE text worked, and BLE→USB text worked.
- Console errors during the rerun smoke: 0.
- Artifacts saved: `.omo/start-work/artifacts/ui-functional-consolidation/f2-rerun-usb.png`, `.omo/start-work/artifacts/ui-functional-consolidation/f2-rerun-ble.png`.
- Smoke interaction note: the message box is `#textInput`; the send button is `#sendTextBtn`, and if browser_click is flaky on an enabled send button, `browser_evaluate(...el.click())` works for the mock flow.

## 2026-08-28 Final Verification Wave F1 rerun
- Cache-busted mock chat pages verified after ACK fix: bidirectional text, >=5 KB USB→BLE attachment with SHA/download, and cancel evidence all passed with Playwright console errors at 0. For cancel timing, use a protocol-valid payload under the 4096-chunk cap; oversized files trigger META chunk-count errors instead of testing cancel.

- 2026-08-28 F1 rerun: mock USB/BLE verification passed with cache-busted chat pages; use dynamic BroadcastChannel throttling only during cancel checks so normal SHA-256 transfer stays fast.

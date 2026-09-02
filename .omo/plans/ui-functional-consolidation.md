# Plan: Functional UI Consolidation for chat-usb.html / chat-ble.html

## Goal

Replace the two divergent neon-styled chat pages with one shared, functional UI:
single stylesheet, single UI-rendering module, identical layout on both pages,
explicit send affordances, throughput/ETA readout, no decorative effects.
Design direction is settled by the prior critical review — this plan is
decision-complete and needs no further interview.

## Locked Decisions (do not re-open)

1. **Shared assets**: new `web/airbridge-ui.css` + `web/airbridge-ui.js`.
   Both pages link/import them; all duplicated inline CSS and UI-rendering JS
   (transcript, attachment card, progress bar, log, formatting helpers) moves there.
2. **Layout**: USB page's grid pattern (`minmax(0,1fr) 280px` sidebar with
   Transfer State + Status Log) becomes THE layout for both pages. Everything
   must fit a 1280x900 viewport without scrolling when idle.
3. **Palette**: neutral dark (grays), ONE accent color for interactive/focus
   state, semantic colors only for ok/warn/error. NO glow shadows, NO gradient
   progress fills, NO decorative border-radius beyond 6px, NO card shadows.
4. **Labels**: connect buttons read exactly `Connect USB` and `Connect BLE`
   (matches README quickstart, which is currently out of date).
5. **Attachment send**: explicit "Send Attachment" button on BOTH pages.
   BLE's `fileInput.change -> sendSelectedFile` auto-send is REMOVED.
6. **Progress model**: the per-message attachment card is the primary progress
   display. The Transfer State panel shows exactly: one state line
   (plain words: `idle` / `sending` / `receiving` / `cancelled` — no
   "half-duplex:", no item IDs, no "Yielded to peer item N" jargon), one
   throughput line (`N/M chunks · X.X KB/s · ~Ys left`), and the Cancel button.
   The duplicate global progress bar is removed from both pages.
7. **Throughput/ETA**: computed from ACK-timed chunk completions (sender) and
   `receiver.on('data')` events (receiver); simple EMA over chunk intervals is
   fine. Show `--` when no rate is measurable yet.
8. **Formatting consistency** (in airbridge-ui.js, used by both pages):
   timestamp `HH:MM:SS` 24h via one helper; hash truncated to 12 chars + ellipsis;
   `formatBytes`: 1 decimal below 10 KB, 0 decimals above; NO emoji anywhere
   (remove the 📎 from BLE attachment cards).
9. **Connection card**: when connected, collapse to a single status line
   (dot + "Connected" + device name + Disconnect). The BLE hint
   "Pick the HP Wireless device in the list." is hidden once connected.
10. **Clear buttons**: "Clear" for the transcript and "Clear" for the status
    log on both pages.
11. **Deploy bundle constraint**: `tools/build_bundle.py` inlines JS by EXACT
    string match of import statements and asserts no module syntax remains.
    It must be extended to inline `airbridge-ui.js` (same IIFE pattern) and to
    inline `airbridge-ui.css` (replace the `<link rel="stylesheet"
    href="./airbridge-ui.css">` tag with an inline `<style>` block). The
    `bootstrap.js` 1200-char limit is unaffected; do not touch bootstrap.js.
12. **Out of scope**: protocol logic (`airbridge-protocol.js`,
    `airbridge-transports.js`, `airbridge-identity.js`) is NOT modified.
    `protocol-harness.html` is NOT modified. Firmware/FAP untouched.

## TODOs

- [x] 1. Create `web/airbridge-ui.css` — shared functional stylesheet.
  Neutral dark palette, single accent, no glow/gradients/shadows, grid layout
  classes (`.grid`, `.card`, connection row, status pill with plain colored
  dot, transcript, message bubbles, attachment card, progress track/fill in
  flat colors, log, transfer panel, clear buttons, mobile breakpoint at 760px
  collapsing the grid to one column). Must contain every class both pages use
  after refactor so neither page keeps any inline `<style>`.

- [x] 2. Create `web/airbridge-ui.js` — shared UI module exporting:
  `formatBytes`, `shortHash`, `timeLabel` (HH:MM:SS), `escapeHtml`,
  `createLogger(logEl)` (append + autoscroll + clear), `clearTranscript`,
  `addMessage` / `addSystemMessage` (transcript rows: sender label, timestamp,
  bubble), `renderAttachmentCard` (name, size+mime, hash line, optional
  progress block with `role="progressbar"`, optional download link — NO emoji),
  `updateCardProgress`, and `createThroughputMeter()` returning
  `{ tick(chunkIndex, totalChunks), text() }` producing the `N/M chunks · X.X
  KB/s · ~Ys left` line.

- [x] 3. Rework `web/chat-usb.html` onto the shared assets: delete the inline
  `<style>` block (link airbridge-ui.css instead), delete the UI-rendering and
  formatting functions now provided by airbridge-ui.js (import them), rename
  connect button to `Connect USB`, remove the global `#progressWrap`/
  `#progressBar` bar and replace the Transfer State panel contents with
  state line + throughput line + Cancel, wire throughput meter into the ACK
  tracker and `receiver.on('data')`, add Clear buttons for transcript and log,
  switch all setProgress-style calls to the new panel API. Protocol logic
  (BUSY arbitration, yield/retry, cancel, mock transports) stays functionally
  identical.

- [x] 4. Rework `web/chat-ble.html` onto the shared assets: same changes as
  task 3 PLUS adopt the grid layout (transcript+composer left, Transfer
  State/Status Log sidebar right — restructure the four stacked cards into
  the USB page's two-column `<main class="grid">`), rename connect button to
  `Connect BLE`, REMOVE the `fileInput.change` auto-send and add an explicit
  disabled-by-default "Send Attachment" button mirroring USB behavior, remove
  the 📎 emoji path, hide the picker hint once connected. Protocol logic
  (busy backoff, yield, cancel, mock transports) stays functionally identical.

- [x] 5. Extend `tools/build_bundle.py`: add `UI_IMPORT`/`UI_BINDINGS` entries
  mirroring the existing PROTOCOL pattern to inline `airbridge-ui.js`, and
  inline the `<link rel="stylesheet" href="./airbridge-ui.css">` tag as a
  `<style>` block in both dist outputs. Keep the existing guards
  (`no "import "`, `no type="module"`) and add a guard that no
  `airbridge-ui.css` link remains in the output. Run it: both
  `dist/app-usb.html` and `dist/app-ble.html` build clean.

- [x] 6. Doc touch-up: README quickstart button names already say
  "Connect USB"/"Connect BLE" — verify they now match the UI exactly; update
  `docs/architecture.md` / `docs/protocol.md` ONLY if they reference removed
  UI elements (progress bar IDs, button labels). No other doc changes.

## Dependency Map

- 1, 2: parallel (different files; 2 imports nothing from 1 at author time —
  class names are fixed by the Locked Decisions).
- 3: blocked by 1 + 2 (consumes both).
- 4: blocked by 1 + 2 (consumes both). Parallel with 3 (different files).
- 5: blocked by 3 + 4 (must match the pages' exact import/link strings).
- 6: blocked by 3 + 4 (label verification).

## Success Criteria

- Build: `python3 tools/build_bundle.py` exits 0 and prints both bundle sizes.
- Harness: `web/protocol-harness.html` still reports 17/17 (open via
  `python3 -m http.server 8081 --directory web` and run in Playwright).
- Mock E2E: with `?mock=1` on both pages in two tabs, bidirectional text and
  a >= 5 KB attachment complete with SHA-256 verified both directions, sender
  cancel and receiver cancel still work.
- Bundle smoke: serve `dist/` and open `app-usb.html?mock=1` +
  `app-ble.html?mock=1`; connect + one text message each direction works.
- Visual: at 1280x900, Connection + Transcript + composer + Transfer State +
  Status Log are all visible without scrolling on both pages; no glow,
  no gradients, no emoji; identical layout/labels/formatting on both pages.

## Final Verification Wave

- [x] F1. Functional E2E reviewer: run the full Mock E2E flow from Success
  Criteria via Playwright (serve web/ on :8081, two tabs, `?mock=1`), including
  >= 5 KB attachment with hash verification and both cancel directions.
  Verdict: APPROVE or REJECT with evidence (screenshots + console log).
- [x] F2. Bundle reviewer: run `python3 tools/build_bundle.py`, inspect
  `dist/app-usb.html` and `dist/app-ble.html` for leftover module/link syntax,
  then serve `dist/` and run the bundle smoke flow. Verdict: APPROVE or REJECT.
- [x] F3. Regression reviewer: run `protocol-harness.html` to 17/17 and diff
  `web/airbridge-protocol.js`, `web/airbridge-transports.js`,
  `web/airbridge-identity.js`, `web/bootstrap.js` against git HEAD — must be
  byte-identical. Verdict: APPROVE or REJECT.
- [x] F4. Design reviewer: screenshot both pages at 1280x900 (idle +
  mid-transfer + connected states) and audit against Locked Decisions 2-4,
  6, 8-10 (layout parity, no decoration, label/formatting consistency, panel
  contents, clear buttons). Verdict: APPROVE or REJECT with a per-decision
  checklist.

# Plan: BLE HP Impersonation (ble-impersonation)

## Goal

While the Pocket AirBridge FAP runs, the Flipper must be indistinguishable from an HP
peripheral on BOTH radios, at every inspectable layer:

- **USB**: HP `03F0:5341` composite (already implemented) — HP identity at the
  descriptor level; the vendor `0xFF00` data interface is an accepted residual
  (Decision 12; genuine-dongle zero-diff deferred).
- **BLE**: HP keyboard+mouse identity — advertised name, MAC, appearance,
  manufacturer data, GAP characteristics, DIS strings, and ALL GATT service/char
  UUIDs contain zero Flipper fingerprint. HID-over-GATT (HIDS 0x1812) present so the
  device is a *functional* HP BLE keyboard (enables BLE Deploy typing) plus a vendor
  serial service (new UUID family) carrying the AirBridge data path.

## Security Goal (precise definition of "absolute")

**In scope (must be HP-clean):**
- Passive BLE advertisement scans (name, appearance, service UUIDs, mfg data, MAC OUI)
- OS Bluetooth device lists and pairing dialogs
- Active GATT enumeration: service/characteristic UUIDs, DIS reads, GAP name char
- USB descriptor dumps (Device Manager / usbccgp / lsusb): VID/PID/bcdDevice,
  iManufacturer/iProduct/iSerial, interface set, HID report descriptors, endpoints
- OUI database lookups on the BLE MAC

**Out of scope (explicitly accepted):** RF/stack behavioral fingerprinting (BLE timing,
connection parameters, USB enumeration timing), physical inspection. Lab-grade
analysis is not a threat.

**Residual documented anomaly (BLE only, accepted):** the vendor serial GATT service
has no HP counterpart (real HP BLE keyboards don't expose one). It is nameless
128-bit UUIDs; a determined active auditor sees "keyboard + unknown vendor service."
Accepted by user (T3 tier chosen: UUIDs replaced, service kept).
**USB side:** the vendor `0xFF00` data interface is an ACCEPTED residual (Decision 12,
user-accepted 2026-07-22) — the genuine-dongle zero-diff fixture (W7) is deferred;
the data-path interface is documented as intentional.

## Hard Constraints (user)

0. REGRESSION RULE (added 2026-07-21): after EVERY firmware/FAP/web change, re-run
   previously-working flows BEFORE debugging anything new: bridge chat both
   directions, file transfer + SHA-256, USB deploy, protocol harness 17/17.
1. "absolute no way to detect flipper usb/ble when app is running — it should look as HP ble/usb hid"
2. Identity active ONLY while the FAP runs; stock Flipper identity fully restored on
   app exit and on reboot (runtime-only reinit, nothing persisted).
3. BLE/HID identity MUST come from `/ext/apps_data/pocket_airbridge/config` — never
   hardcoded in firmware. One identity per config, no runtime switching (mirrors the
   USB single-profile rule; runtime reconfig lessons apply). EXCEPTION (protocol
   constants): the serial service UUID family is a wire-protocol constant compiled
   into firmware (like the USB HID report descriptors, which are also compiled);
   "identity" here means name, MAC, appearance, manufacturer data, and DIS strings.
4. "security measures" = identity stealth at firmware-default pairing SECURITY (MITM,
   numeric comparison), but with **bonding DISABLED** (accepted 2026-07-22): per-session
   authenticated encryption, no persistent bond — required so macOS does not hoard the
   BLE keyboard link and block Web Bluetooth. No whitelists/custom crypto.
5. Deploy typing remains menu-gated with on-screen confirm, TYPING… display, BACK
   aborts instantly (AGENTS.md). New BLE typing path follows the same UX.
6. Firmware ships via `docs/firmware/` patch bundle in flipper-hid (firmware repo is
   local-only; applications_user is gitignored upstream).
7. Every physical gate (forget-device, FAP launch, pairing confirmation) uses the
   `question` tool + Funk.aiff attention signal, one action per question.

## Verified Mechanics (from exploration, file:line in firmware repo)

- Profile seam: `FuriHalBleProfileTemplate` (targets/f7/ble_glue/furi_ble/profile_interface.h:24-35)
  with start/stop/get_gap_config; installed via `bt_profile_start()` →
  `furi_hal_bt_change_app()` (full core2 reinit, runtime-only).
- Precedent: `lib/ble_profile/extra_profiles/hid_profile.c` (template at :430,
  start composes battery+DIS+HIDS at :140-172, GapConfig with appearance/pairing at
  :382-398, MAC derivation at :399-428).
- Composite composition: a custom start callback can call `ble_svc_battery_start()`,
  DIS, `ble_svc_hid_start()`, AND `ble_svc_serial_start()` in one profile. GATT
  budget: 68 attrs / 8 services (app_conf.h:49,58); composite needs 54/68, 4/8. FITS.
- **HID symbols are NOT FAP-exported** (api_symbols.csv: `ble_svc_hid_*`, `ble_profile_hid_*`
  all `-`). Composite profile must live in FIRMWARE (`lib/ble_profile/extra_profiles/`);
  new wrapper symbols added to `targets/f7/api_symbols.csv` with `+`.
- Serial service IS FAP-exported (`ble_svc_serial_*`, `+`) but UUIDs are compiled-in
  (targets/f7/ble_glue/services/serial_service_uuid.inc, fe60-fe64 family) — new
  family requires a firmware fork/parameterization.
- DIS leaks: "Flipper Devices Inc." (dev_info_service.c:31), serial "1.0" (:32),
  git hash/branch (:105-113), custom RPC-version 128-bit char (:91-101). Started
  unconditionally by both stock profiles.
- Identity sources: name/MAC from `furi_hal_version` (furi_hal_version.c:93-116,277);
  adv name+GAP name char written from same buffer (gap.c:368-376,455-466); adv
  carries 16-bit service UUID 0x3080|hw_color (serial_profile.c:80); appearance
  0x8600 (serial_profile.c:56); MAC uses STMicro OUI (furi_hal_version.c:107-116),
  written as public address (gap.c:336-337, CFG_IDENTITY_ADDRESS=GAP_PUBLIC_ADDR
  app_conf.h:7).
- Extra beacon (targets/f7/ble_glue/extra_beacon.c): separate Flipper discovery
  advertisement — MUST be off in the airbridge profile path (verify where the stock
  serial profile enables it; our template must not).
- Pairing: composite uses `GapPairingPinCodeVerifyYesNo` with **bonding DISABLED**
  (accepted 2026-07-22): per-session authenticated encryption, no persistent bond —
  required so macOS does not hoard the BLE keyboard link and block Web Bluetooth.
- HIDS: report map has keyboard (ID 1, 8-byte boot) + mouse (ID 2) + consumer (ID 3)
  (hid_profile.c:51-125); send via `ble_svc_hid_update_input_report()`
  (hid_service.c:265) or profile wrappers.
- `bt_profile_start(bt, template, params)` passes params to get_gap_config — the
  FAP pushes the config-file identity through this channel.
- Web Bluetooth CANNOT access 0x1812 (Chrome blocklist) AND **cannot use it in
  requestDevice filters either** — blocklisted UUIDs in a filter's `services:`
  throw `SecurityError` (Momus-verified). Receiver discovery therefore uses
  `filters: [{ name: <config HP name> }]` (or `manufacturerData` on the HP
  company ID) with `optionalServices: [<new serial service UUID>]` so
  `getPrimaryService()` on the data path remains allowed. HIDS is consumed by
  the host OS only, never by the web pages.
- Adv payload budget 31 bytes — applies to BOTH adv and scan response (31 each).
  DECISION: adv = flags + 16-bit HIDS UUID (0x1812) + short name; scan response =
  complete name + HP manufacturer data ONLY (no 128-bit serial UUID — it does not
  fit with name+mfg and is not needed). Receiver filter:
  `filters: [{ name: <identity name> }]` (fallback `manufacturerData` on the HP
  company ID) + `optionalServices: [airbridge serial UUID]`. The serial UUID is
  never advertised; `optionalServices` grants GATT access without advertising it,
  and 0x1812 appears in NO web filter anywhere (blocklist SecurityError).

## Work Items

### W1 — Firmware: airbridge composite BLE profile
New `lib/ble_profile/extra_profiles/airbridge_profile.c/h` modeled on hid_profile.c:
- `start`: battery + HP-DIS (W3) + HIDS (stock hid_service.c, stock report map) +
  airbridge serial service (W2).
- `stop`: reverse; unconditional.
- `get_gap_config`: from `profile_params` (identity struct from config): adv name,
  appearance 0x03C1, MAC per the W5 MAC rule (explicit HP-OUI address; the ONLY
  permitted mode — no locally-administered random), adv 16-bit UUID = 0x1812,
  mfg data (HP company ID + bytes) → scan response, bonding=false,
  `GapPairingPinCodeVerifyYesNo`.
- Template variable + kb/mouse/consumer report-send wrappers exposed.
Acceptance: firmware builds; `fbt` composite profile starts; GATT enum shows
battery+DIS+HIDS+serial, nothing else.

### W2 — Firmware: airbridge serial service with new UUID family + bt.c wiring
Fork serial_service.c → `airbridge_serial_service` (same 4-char structure: TX
notify, RX write, flow control, status) with a NEW 128-bit UUID family defined in
one header. **bt.c coupling is explicit work, not an assumption** (Momus blocker):
- `bt_serial_tx()` today unconditionally calls `ble_profile_serial_tx()`, which
  asserts the active profile is `ble_profile_serial`; the RX event callback is
  likewise only registered for that profile. W2 adds an active-profile branch in
  bt.c: when the airbridge profile is active, `bt_serial_tx()` routes to the
  airbridge serial service TX and `bt_serial_event_callback`/raw-RX diversion is
  registered on the airbridge service connection. RPC setup stays STRICTLY on
  the default serial profile path — never touches the airbridge service.
- The branch keys off the active profile template pointer, mirroring the existing
  serial-profile check (bt.c:203-211 area).
Acceptance: receiver connects via new UUIDs; chat E2E passes both directions;
old fe60-64 UUIDs absent from GATT enum while FAP runs; default profile (after
exit) still RPC-pairs with the mobile app exactly as before.

### W3 — Firmware: HP DIS service
Minimal DIS fork: manufacturer/model/serial/PnP strings from profile_params;
NO RPC-version characteristic, NO git strings. Used only by the airbridge profile;
stock dev_info_service untouched (default profile unaffected).
Acceptance: bleak GATT dump shows exactly the config strings, characteristic set =
{mfr name, model, serial, PnP ID} only.

### W4 — Firmware: extra-beacon suppression + identity plumbing
Verify where the stock path enables extra_beacon; ensure the airbridge profile path
never starts it. Define `AirbridgeBleIdentityParams` struct (name, mac mode/bytes,
appearance, mfg data, DIS strings) consumed by get_gap_config; add new symbols to
`targets/f7/api_symbols.csv` with `+` (template var, report wrappers).
Acceptance: scan shows ONE advertisement from the device while FAP runs; FAP links
against new symbols (FAP builds).

### W5 — FAP: config-driven identity install/restore + BLE Deploy typing
- Parse `[ble]` section of `/ext/apps_data/pocket_airbridge/config` (same loader as
  USB profile config): name, mac (see MAC rule below), appearance, mfg hex,
  dis_mfr/dis_model/dis_serial/dis_pnp. Defaults = HP identity.
- **MAC rule (Momus blocker — no non-HP OUI allowed):** `ble_mac` is a full
  explicit 6-byte address whose OUI (first 3 bytes) MUST be an HP/Hewlett-Packard-
  registered OUI (allowlist in the FAP loader). No wildcard tails, no random mode,
  no locally-administered addresses — anything else is REJECTED at config load
  (fall back to default HP identity + on-screen warning). The address is written
  via the existing public-address path, so the on-air MAC always equals the
  configured address exactly.
- At startup (after GUI init, same discipline as USB always-on apply): fill params,
  `bt_profile_start(bt, ble_profile_airbridge, params)`. On EVERY exit path incl.
  BACK-abort: `bt_profile_restore_default(bt)`.
- Deploy typing becomes d-pad actions (see W10): UP = USB HID typing,
  DOWN = BLE HID typing; both keep the gate UX (confirm → TYPING… → BACK abort);
  sends keyboard reports via new wrappers (US layout, ASCII-only bootstrap.js).
Acceptance: FAP start→HP identity on air; exit→stock identity on air (bleak-verified);
BLE typing produces correct chars in a text field on the host.

### W6 — Web: receiver identity coupling (same commit as W2)
`web/airbridge-transports.js`: replace SERIAL_UUID/char constants with the new
family; BLE discovery → `filters: [{ name: <identity name> }]` (fallback
`filters: [{ manufacturerData: [{ companyIdentifier: <HP company ID> }] }]`) +
`optionalServices: [airbridge serial UUID]` (NEVER `services:[0x1812]` —
blocklist SecurityError); remove all 'Flipper' namePrefix references. Update
`receiver.html`, `chat-ble.html` (duplicated UUID constants),
`protocol-harness.html` mocks.
**Identity sync (Momus blocker — browser can't read the SD config):** sources of
truth are split deliberately — (a) the serial UUID family is CANONICAL in the
firmware header defined in W2; (b) name / HP company ID / appearance / MAC / DIS
strings are CANONICAL in the FAP config file. `tools/gen_identity.py` parses BOTH
(the firmware UUID header + the config file) and generates
`web/airbridge-identity.js` (name, HP company ID, serial UUID family) so web,
firmware, and config can never drift. **The generator is implemented AND invoked
in W6** (W6 already depends on W2, which defines the header; the config schema
lands in W5 — W6 also depends on W5, see task graph). W9 only re-runs the
generator to package the already-working artifact into the bundle.
`receiver.html`/`chat-ble.html` import it. A manual override field
(receiver-side text input persisted to localStorage) covers ad-hoc config
changes, documented in docs/firmware-guide.md.
Acceptance: grep -ri "flipper" web/ shows zero runtime-meaningful references;
harness 17 tests pass with new constants.

### W7 — USB: descriptor baseline fixture + audit + N50194 diff
**First create the missing baseline** (Momus blocker — no raw descriptor capture
of the genuine dongle exists in-repo today; the N50194 Get-PnpDevice output is a
device-tree listing, not a descriptor dump):
- Physical gate (question tool + Funk.aiff): plug the genuine HP `03F0:5341`
  dongle into the dev Mac; capture `lsusb -v`-equivalent (system_profiler
  SPUSBDataType + IORegistryExplorer/ioreg dump) into a versioned fixture
  `tests/fixtures/hp_03f0_5341_genuine.txt` with provenance header (date,
  machine, OS). If descriptor fields are unobtainable on macOS, gate a capture
  on N50194 (Windows: usbccgp/descview) instead.
- `tools/usb_descriptor_diff.py`: dump the Flipper HP-composite descriptors,
  diff every field against the fixture: VID/PID/bcdDevice, iManufacturer/
  iProduct/iSerial, interface count/classes, HID report descriptors, EP layout.
  Fix residuals (strings, bcdDevice).
- **Vendor-interface resolution ladder** (Momus blocker — no silent USB anomaly):
  (a) if the genuine fixture shows a vendor/consumer interface, shape ours to
  match it exactly; (b) if the genuine dongle has NO vendor interface, reshape
  the USB data path onto a genuine-matching channel (e.g., vendor usage page
  inside the keyboard/consumer report descriptor, or HID OUT-report side
  channel) — this is a transport redesign sub-task scoped by the fixture
  finding; (c) only if (a) and (b) are both infeasible, bring an explicit
  exception request to the user before keeping 0xFF00.
- Then the N50194 usbccgp comparison (physical gate, question tool).
Acceptance: fixture committed with provenance; ZERO diffs against it (any
deviation resolved via the ladder or explicit user sign-off recorded in the
decisions log).

### W8 — QA suite
- `scripts/ble_qa_scan.py` (bleak): asserts while FAP runs — adv name == config
  name; no case-insensitive "flipper" anywhere in adv/scan-rsp; **MAC == the
  configured address exactly, and its OUI is in the HP allowlist** (not merely
  "not STMicro"); appearance == config; adv service
  UUIDs == {0x1812}; GATT enum == battery+DIS+HIDS+serial(new family) only; every
  DIS string == config; no RPC-version char; no git-hash-shaped strings. Also
  asserts single advertisement source (extra beacon off).
- Restoration QA: after FAP exit AND after reboot, scan asserts stock Flipper
  identity back.
- Regression: protocol-harness 17 tests; real E2E (sender→FAP→receiver) SHA-256
  match, DROP/TXERR = 0; BLE Deploy typing demo on https page.
- Physical gates (question tool + Funk.aiff): forget old Flipper BLE device on PC-B
  BEFORE first post-change connect; N50194 pairing check; Windows + macOS pairing
  popup confirmation (numeric comparison accepted).
- **Chromium hardware QA (Momus):** scripted Playwright/Chromium scenario on real
  hardware proving `requestDevice` with the name filter shows and selects the HP
  device, the manufacturerData fallback filter also matches, and
  `getPrimaryService(airbridge serial UUID)` succeeds via `optionalServices`
  (i.e., no SecurityError from filter construction, service reachable without
  0x1812 access).
Acceptance: all scripts green; evidence logs saved under .omo/notepads/ble-impersonation/.

### W10 — FAP UI redesign: single always-on Bridge screen
Current FAP (pocket_airbridge.c) has 8 screens (Menu, Bridge, DeployPrompt,
Typing, Waiting, Streaming, Done, Error) with a menu offering "Bridge" / "Deploy app".
Redesign to ONE primary screen:

- **Boot straight into Bridge**: no menu screen; the relay is active from the
  first render (bridge is always on and working). Menu screen and menu_index
  deleted; BACK = exit app (restore USB + BLE identities).
- **D-pad = actions**: UP = Deploy bootstrap via USB HID typing; DOWN = Deploy
  bootstrap via BLE HID typing; LEFT/RIGHT = reserved (display-only in legend,
  no action this iteration). UP/DOWN lead to the existing confirm prompt
  (DeployPrompt, reworded per transport: "Place cursor… OK types via USB/BLE"),
  then Typing, then back to Bridge. The consent gate is preserved unchanged.
- **Stats = 2 lines** (128x64 canvas): upper line = icon glyphs (stock
  button-arrow assets I_ButtonUp/Down/etc. if present, else canvas-drawn
  primitives) marking ↑ U->B, ↓ B->U, plus DROP/TXERR markers; lower line =
  the counter digits aligned under their icons. Title line above (profile
  identity via draw_identity), bottom line = d-pad legend ("↑USB deploy ↓BLE
  deploy BACK:exit"). Waiting/Streaming/Done/Error modals unchanged.
- State machine: AirbridgeScreenMenu removed; app state initializes to
  AirbridgeScreenBridge; Bridge input handler routes InputKeyUp/Down to deploy
  prompts; bridge tick/render loop continues during all states.
Acceptance: app launches into Bridge with counters live (LED heartbeat +
U->B/B->U increment on traffic); UP/DOWN trigger the gated deploy flows on
their transports; LEFT/RIGHT inert; BACK exits and restores stock identities
(verified by W8 restoration QA).

### W9 — Delivery: patch bundle + docs
Regenerate `docs/firmware/` (N-file firmware patch incl. new profile/services/
api_symbols + FAP copy). Update docs/architecture.md, docs/protocol.md (new UUID
family + identity model), docs/firmware-guide.md, AGENTS.md (BLE identity section),
`.agents/skills/flipper-zero-dev/references/hid-composite-deploy.md` (BLE lessons).
Acceptance: both patches `git apply --check` clean on pristine base commit; docs
contain no stale 'Flipper' BLE filter instructions.

## Task Graph

W1 ← W2, W3, W4 (profile composes the services)
W5 ← W1, W4 (FAP needs template + exports)
W6 ← W2, W5 (needs UUID header + config schema to implement gen_identity.py)
W7 independent of W1-W6 (USB side)
W10 ← W5 (deploy actions must exist before d-pad wiring; pure FAP change otherwise)
W8 ← W5, W6, W7, W10
W9 ← W8

## Risks / Watch-items

- **Windows HID bonding noise**: with HIDS present, Windows on PC-B may try to use
  the device as a keyboard — that IS the feature (Deploy typing), but the receiver
  Web Bluetooth session coexists with OS HID; verify no contention (QA in W8).
- **bt.c RPC coupling**: the forked serial service must not confuse the RPC path
  when the DEFAULT profile is active; airbridge service active only under airbridge
  profile. Verified in W2 acceptance.
- **Bond keys across identity swap**: PC-B must forget the old device (W8 gate);
  stale GATT cache = phantom failures.
- **Crash mid-run**: device stays HP until reboot — acceptable (reboot restores;
  nothing persisted), documented in AGENTS.md.
- **Configured MAC uniqueness**: the operator is responsible for not colliding the
  configured HP-OUI address with a real device on the same site; W8 QA verifies
  the on-air address equals the config.
- **Adv budget**: name ≤ ~10 chars in adv if HIDS UUID present; full name in scan
  response. Config validation in W5 warns on long names.

## TODOs

- [x] 1. W1 — Firmware: airbridge composite BLE profile (see Work Items)
- [x] 2. W2 — Firmware: airbridge serial service + new UUID family + bt.c wiring
- [x] 3. W3 — Firmware: HP DIS service
- [x] 4. W4 — Firmware: identity params struct + extra-beacon suppression + api_symbols exports
- [x] 5. W5 — FAP: config-driven identity install/restore + BLE Deploy typing
- [x] 6. W6 — Web: receiver identity coupling + tools/gen_identity.py
- [~] 7. W7 — USB: genuine-dongle fixture + descriptor audit + N50194 diff (BLOCKED: genuine dongle unavailable; tooling done, hardware capture deferred)
- [x] 8. W8 — QA suite (bleak scanner, restoration, regression, Chromium hardware, physical gates)
- [x] 9. W9 — Delivery: patch bundle + docs
- [x] 10. W10 — FAP UI redesign: single always-on Bridge screen
- [x] 11. W11 — BLE Deploy full chain: web/bootstrap-ble.js payload + FAP app streaming over airbridge BLE serial (mirrors USB deploy 1:1) + BLE typing send-error fix (first keystroke lands, then KEYBOARD SEND ERROR — hardware-reported 2026-07-21)

## Final Verification Wave

- [x] F1. Goal/constraint audit: every Hard Constraint (1-7) and Security Goal line verified against implementation + QA evidence
- [x] F2. Code quality review: all new/changed firmware, web, and tools code reviewed; no stubs, patterns followed
- [x] F3. Identity-leak hunt: adversarial review hunting any residual Flipper fingerprint (code paths, strings, descriptors, adv payloads, UUIDs)
- [x] F4. User sign-off: hardware demo (HP identity on air, BLE+USB deploy typing, chat E2E, identity restoration) accepted via question tool

## Decisions Log (pre-approved by user)

1. Tier T1+T2+T3: full identity replacement incl. serial UUID family. Mobile-app
   pairing broken while FAP runs — accepted.
2. Identity: HP BLE keyboard+mouse (functional HIDS, enables BLE Deploy typing).
3. Scope: while FAP runs only; runtime reinit; reboot restores.
4. Pairing: MITM via numeric comparison with **bonding DISABLED** (accepted 2026-07-22)
   — per-session authenticated encryption, no persistent bond, so macOS does not
   hoard the BLE keyboard link.
5. Lab-grade RF/stack fingerprinting out of scope.
6. Identity from config file; single identity; no runtime switching.
7. Composite profile in firmware (HID symbols not FAP-exported); new minimal
   exports added to api_symbols.csv.
8. Receiver discovery (accepted 2026-07-22): chat/receiver pages use
   `acceptAllDevices: true` + `optionalServices: [airbridge serial UUID]` (manual
   device pick, avoids truncated-name filter fragility); the typed BLE bootstrap uses
   `namePrefix: 'HP'` (matches truncated adv name). 0x1812 may NOT appear in any filter
   (blocklist SecurityError).
9. UI: single always-on Bridge screen; d-pad = deploy actions (UP=USB, DOWN=BLE,
   LEFT/RIGHT reserved); 2-line stats (icons over digits); consent gate unchanged.
10. BLE MAC: full explicit 6-byte HP-OUI address from config; NO wildcard tails,
    no random mode, no locally-administered addresses (rejected at load).
11. Web↔firmware identity sync: UUID family canonical in the firmware header;
    name/company ID/etc. canonical in the FAP config; tools/gen_identity.py
    parses both to generate web/airbridge-identity.js; receiver manual override
    for ad-hoc changes.
12. USB: the vendor 0xFF00 interface is an ACCEPTED residual (user-accepted 2026-07-22),
    matching the BLE vendor serial service residual — the genuine-dongle zero-diff
    fixture (W7) is deferred, and the data-path interface is documented as intentional.

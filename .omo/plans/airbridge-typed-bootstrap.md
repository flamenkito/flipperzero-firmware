# Sisyphus Plan — Typed Bootstrap: App Delivery to HID-Only Locked-Down PCs

## Objective

Enable the full Pocket AirBridge chat app to be delivered to a **locked-down PC where USB is policy-blocked for everything except HID** — no network, no mass storage, no local files. The Flipper Zero types a minimal bootstrap (as a HID keyboard, from an explicit on-device menu action), the bootstrap opens WebHID, and the Flipper streams the complete single-file app to the PC over the existing vendor HID channel.

**The PoC story:** walk up to a restricted corporate machine, plug in the Flipper, select "Deploy app", watch the bootstrap type itself, click Connect — full chat app running, sourced entirely from the Flipper. PC-B (BLE side) is unchanged and out of scope.

## Security Model (deliberate change from prior constraint)

Previous constraint: "No BadUSB behavior, no keyboard emulation." This plan introduces a **menu-gated exception**:

- Keystroke emission happens ONLY from an explicit `Deploy app` menu action on the Flipper, after a second on-device confirmation once the user has placed the cursor.
- The Flipper screen shows `TYPING…` for the entire emission; pressing BACK aborts instantly.
- In `kbd_vendor` composite profiles the keyboard interface stays enumerated (that is the cover identity), but **keyboard reports are emitted exclusively during the Deploy flow** — never in Bridge mode, never from any data path.
- Typed content is a fixed, reviewable, ASCII-only artifact (the bootstrap source is in the repo).
- Docs must state plainly: this is BadUSB-shaped by design — it is the only USB class that survives HID-only policies — and it requires physical possession + explicit user action.

## Delivered Artifacts

| Artifact | Role |
|---|---|
| `web/bootstrap.js` | The typed snippet source (ASCII-only, ≤ ~900 chars). Reviewable in repo. |
| `tools/build_bundle.py` | No-dependency Python script: inlines `airbridge-protocol.js` + `airbridge-transports.js` into `chat-usb.html`, strips module syntax, emits one self-contained HTML file. |
| `dist/app-usb.html` (or SD-path) | The streamed app bundle, deployed to `/ext/apps_data/pocket_airbridge/app-usb.html` on the Flipper SD. |
| FAP `Deploy app` menu + stream server | Keyboard typing, profile switching, SD bundle streaming. |

## Key Technical Risks (Phase 0 must kill these first)

1. **`data:` URLs are opaque origins — likely NOT secure contexts** → WebHID almost certainly unavailable there. Prediction: dead on arrival; confirm anyway because the UX would have been the best (no devtools).
2. **`about:blank` + DevTools console**: `isSecureContext` expected `true`, `navigator.hid` expected present. This is the predicted winner.
3. **`javascript:` URL on `about:blank`**: runs in page context; same context questions as (2) but no console needed — typed straight into omnibox. May hit omnibox length limits and `javascript:`-stripping behaviors.
4. **`requestDevice` transient activation**: console-evaluated code may lack user activation → picker rejected. Mitigation baked into the design: the bootstrap writes a minimal landing page with a **Connect button**; the user's real click supplies activation.
5. **Keyboard layout**: typed payload is ASCII-only but includes `{}[]();:=>"'` — correct only on US layout. Document the requirement; optionally restrict the snippet's charset further (avoid `{}` via `function` bodies? — evaluate in Phase 0; do not contort the code if layout doc suffices for demo).
6. **Typing reliability**: HID keyboard reports at human-plausible cadence; special chars need Shift handling. Reuse the firmware's existing HID keyboard infrastructure (BadUSB app's proven path) rather than hand-rolling scan codes.

## Stream Protocol (Bootstrap ⇄ FAP)

Deliberately simpler than the chat protocol (download-only, one-shot):

1. Bootstrap → FAP: single 64-byte report, byte 0 = `0x42` (`'B'`, bundle request).
2. FAP → Bootstrap: first report = 4-byte LE `total_len` + 4-byte LE `checksum` (additive uint32 sum of file bytes), rest zero.
3. FAP → Bootstrap: file bytes in 64-byte reports, sequentially, zero-padded tail.
4. Bootstrap accumulates `total_len` bytes, verifies additive checksum, then `document.open(); document.write(text); document.close();`.
5. Failure: on checksum mismatch, landing page shows `Transfer corrupt — retry` and the Connect button re-arms.

Rationale: USB interrupt IN transfers are hardware-reliable; per-chunk ACK is unnecessary for a one-shot download and would inflate the typed snippet. Checksum guards against profile-switch race bytes.

## Execution Order & Delegation Map

| Phase | Depends on | Delegation | Status |
|---|---|---|---|
| 0 — Channel validation | — | Orchestrator + user | ✅ DONE 2026-07-20 (data: dead; about:blank+console wins) |
| 1 — Bundle builder | 0 | `deep` (bg_ce60fa85) | ✅ Agent done; independent re-verify pending |
| 2 — Typed bootstrap + stream client | 0 | same agent as 1 | ✅ Agent done (1,152 chars); re-verify pending |
| 3 — FAP composite + deploy | 0 + 3a | `deep` + flipper-zero-dev (bg_965bbdc3) | ✅ Framework done; **HP profile addendum pending worker** |
| 3a — Composite enumeration spike | 3 + addendum | Orchestrator + user (Windows machine) | **GATE — ready to run after addendum** |
| 4 — Docs & clean-slate QA | 1–3a | `writing` | Pending |
| 5 — Hardware E2E deploy demo | 1–3a | Orchestrator + user gates | Pending |

## Decisions Log (2026-07-20)

1. **Channel**: `about:blank` + DevTools console (validated on real Chrome). `data:` URLs confirmed dead (`isSecureContext === false`).
2. **Keystrokes accepted**: HID keyboard typing is the point of the PoC — locked-down machines allow HID only; MSC is banned there by policy.
3. **No-Flipper-identity rule**: every USB profile is a full impersonation (VID/PID/strings/shape); gaming-keyboard-style kbd+vendor composite as the deploy cover.
4. **On-screen identity**: FAP status screen always shows the active impersonation.
5. **Bootstrap templating**: FAP bakes active VID/PID into the typed snippet at deploy time.
6. **Primary impersonation target (from live capture on locked-down machine N50194)**: HP Wireless Keyboard and Mouse dongle `VID_03F0&PID_5341` — chosen over mouse-only dongle `1EA7:0064` because it carries a real keyboard collection (typing works) AND a vendor-defined collection on usage page `0xFF00` (our exact existing channel page). Fidelity: functional clone.
7. **NO runtime profile switching (2026-07-20, user decision)**: profile is selected ONCE via the SD config (`profile=hp_kbd_vendor`, default HP); the FAP menu picker is display-only. Hardware evidence: CDC→composite apply works, but composite→composite re-configuration (A→B profile switch) is fatal on this USB stack (device dies silently, physical reset needed). Oracle analysis pinned a definite endpoint-number collision (vendor IN `0x82` / OUT `0x02` share EP index 2) plus risky manual EP teardown in deinit; rather than re-engineer the HAL mode-switch path, runtime switching is removed by design. Single-apply (CDC→HP on Bridge/Deploy entry) and restore-on-exit are the only USB transitions allowed — both proven survivable on hardware.
8. **Bootstrap templating dropped (2026-07-20)**: the `__VID__/__PID__` substitution never worked (FAP's equal-length `app_replace_token` silently skips 7-char tokens vs 6-char hex values) and is unnecessary — the bootstrap's default `requestDevice` filter list carries every profile VID (Logitech/Dell/MSFT/HP; STM32 `0x0483` dropped, per the no-Flipper-identity rule). On a target machine only the impersonating Flipper matches, so the picker always shows exactly one device. Marker comments removed from `bootstrap.js`.
9. **Trusted Types hardening (2026-07-20)**: DevTools-console execution can run in a TT-enforced main world (observed on Playwright-Chromium `about:blank`: `innerHTML=` died with "requires 'TrustedHTML' assignment" while the isolated world was unaffected). Bootstrap builds its landing page with DOM APIs only (`createElement`/`textContent`/`appendChild` — not TT sinks) and falls back to `blob:`-URL navigation if `document.write` throws.

10. **about:blank not reliably a secure context (2026-07-20)**: on the user's Chrome build, `about:blank` reported `window.isSecureContext === false` and `navigator.hid` was undefined, while `https://example.com` passed and the full typed bootstrap was validated end-to-end on hardware. The robust target for the typed bootstrap is ANY `https://` page. `about:blank` may work on some Chrome builds but must be verified with `console.log(window.isSecureContext)` before relying on it. Orchestrator rule: when a workflow step needs the user to act in the Playwright MCP-driven browser, the orchestrator MUST navigate that window to the exact URL first and name it explicitly in the question (e.g. "the Playwright window showing example.com"). Never assume the user is acting in the same browser the orchestrator is driving; state scattering across two browsers cost a full debug session.

## Target Device Profile — HP 03F0:5341 (captured 2026-07-20, machine N50194)

Primary deploy/bridge identity. All fields below are from the live `Get-PnpDevice` capture.

**Device level:** VID `0x03F0`, PID `0x5341`, bcdDevice `0x0126`, iProduct `HP Wireless Keyboard and Mouse`, composite (bDeviceClass 0x00 → usbccgp).

**Interface MI_00** — HID, SubClass 1 (boot), Protocol 1 (keyboard); one TLC: keyboard (`UP:0001_U:0006`) → binds kbdhid.

**Interface MI_01** — HID, SubClass 1, Protocol 2 (mouse); four TLCs:
- Col01 mouse (`UP:0001_U:0002`) → mouhid
- Col02 vendor-defined (`UP:FF00_U:0001`) → **AirBridge data channel** (same usage page as the existing protocol — web-side needs only the VID/PID filter added, no usage-page change)
- Col03 consumer control (`UP:000C_U:0001`)
- Col04 system control (`UP:0001_U:0080`)

**Implementation:** prefer the exact 2-interface composite (MI_00 + MI_01) if the furi_hal USB stack supports it; otherwise a single-interface multi-TLC functional clone presenting the same child devices is acceptable (device-control policy matching is VID/PID/class-based). No iSerial observed (InstanceId suffix is port-derived) — omit iSerial.

**Alternate identity (captured, optional):** 2.4G Mouse dongle `VID_1EA7&PID_0064` REV 0x0200, iProduct `2.4G Mouse`, device class 03/01/02, single interface, Col01 vendor-defined (`UP:FFB5_U:0001`) + Col02 mouse. Bridge-only (no keyboard) — include only if profile table space is free.

**Web:** `HID_FILTERS` in `airbridge-transports.js` must include `{vendorId: 0x03F0, productId: 0x5341, usagePage: 0xFF00}` (and `0x1EA7/0x0064` with `usagePage: 0xFFB5` if the alternate is built).

## Phase 3a — Composite Enumeration Spike (hard gate for Phase 3)

**Question:** can the Flipper enumerate as a 2-interface HID composite (`MI_00` kbd + `MI_01` mouse/vendor/consumer/system) that Windows binds via `usbccgp` — i.e., a functional clone of the HP `03F0:5341` dongle's shape?

**Build (minimal, no deploy mode yet):** a test profile in the composite framework with device class `0x00/0x00/0x00`, VID `0x03F0` PID `0x5341` REV `0x0126`, iProduct `HP Wireless Keyboard and Mouse`; MI_00 = boot keyboard (1 IN EP); MI_01 = 4-TLC report descriptor (mouse / vendor `0xFF00` / consumer / system, distinct report IDs) with IN+OUT EPs. Bridge data path does NOT need to work for this gate — enumeration only.

**Verify on the Windows machine (user-driven, same commands as the capture):**
1. `Get-PnpDevice -PresentOnly | ? InstanceId -like '*VID_03F0&PID_5341*'` shows: `USB Composite Device` (service `usbccgp`), `USB Input Device` ×2 (`MI_00`, `MI_01`), `HID Keyboard Device`, `HID-compliant mouse`, `HID-compliant vendor-defined device`, `HID-compliant consumer control device`, `HID-compliant system controller` — all `Status OK`, `CM_PROB_NONE`.
2. Child-tree shape matches the real dongle capture (MI_00 → keyboard; MI_01 → 4 collections).
3. Chrome WebHID: `navigator.hid.requestDevice({filters:[{vendorId:0x03F0,productId:0x5341,usagePage:0xFF00}]})` lists and opens the device while the keyboard interface remains functional (type a test string into Notepad via the FAP's typing path or existing HID tooling).

**Pass criteria:** all three checks green. Any `Code 10` / descriptor-failure entry → fix descriptor layout (likely IAD or endpoint-direction issue) and re-run before Phase 3 proceeds.

**Fallback if 2-interface composite fails:** single-interface multi-TLC clone (same VID/PID/strings; kbd+mouse+vendor+consumer+system as 5 collections in one interface, like the `1EA7:0064` dongle's single-interface shape). Slightly different tree (no `usbccgp`, no `MI_xx`) but same VID/PID and same functional channels. Decision recorded here before proceeding.

## Execution Addendum — HP 03F0:5341 Profile (pending worker)

**State after background agents (2026-07-20):**
- Web agent (bg_ce60fa85, ses_080b6b275ffdWYXqX1heZqGj55): `tools/build_bundle.py` → `dist/app-usb.html` (70,391 B, module-scope-preserving IIFE inlining); `web/bootstrap.js` **1,152 chars**, ASCII-only, `__VID__/__PID__` markers; harness **14/14** incl. `bootstrap stream reassembly`; bundle mock chat verified in browser. Self-verified, not yet independently re-run.
- Firmware agent (bg_965bbdc3, ses_080a6b6bbffe2CALKeRxALkR51): 2-interface composite profile framework (kbd intf 0 + vendor intf 1, IAD, class `0xEF/0x02/0x01`), 4 identities (Logitech `046D:C31C`, Dell `413C:2113`, MSFT `045E:07F8`, MSFT vendor-only `045E:07A5`), persisted profile menu, deploy typing + SD bundle streaming, blocking vendor TX API, `api_symbols.csv` exports. `./fbt` and `./fbt build APPSRC=applications_user/pocket_airbridge` pass. **No HP profile** (capture arrived mid-flight).
- Verified by orchestrator: `web/airbridge-transports.js` `HID_FILTERS` = `0x046D / 0x413C / 0x045E / 0x0483:0x5742` — **no HP**. Firmware profile table at `furi_hal_usb_airbridge.c:379-416` — **no HP**. Bootstrap needs no static change (FAP substitutes active VID/PID at deploy time).

**Gap 1 — firmware HP profile** (`targets/f7/furi_hal/furi_hal_usb_airbridge.c` + `targets/furi_hal_include/furi_hal_usb_airbridge.h`):
1. Header: append `FuriHalUsbAirbridgeProfileHpKbdVendor` to the enum (append-only; FAP persists by label string, order is safe).
2. Source strings: `hp_manuf_desc = USB_STRING_DESC("HP")`, `hp_prod_desc = USB_STRING_DESC("HP Wireless Keyboard and Mouse")`.
3. Device descriptor: real dongle is DevClass `0x00/0x00/0x00` (usbccgp via composite class, NOT IAD `0xEF`), bcdDevice `0x0126`, VID `0x03F0` PID `0x5341`. Add a parameterized macro variant (e.g. `AIRBRIDGE_DEVICE_DESCRIPTOR_FULL(vid,pid,cls,sub,proto,bcd)`) and re-express the existing macro as a wrapper so the three existing descriptors stay byte-identical.
4. Profile table entry: label `hp_kbd_vendor`, identity `HP Wireless Kbd+Mouse`, vid `0x03F0`, pid `0x5341`, has_keyboard `true`, interface → new static `FuriHalUsbInterface` reusing `hid_composite_cfg_desc` (functional clone: kbd MI_00 + vendor MI_01; mouse/consumer/system TLCs are optional cover realism, defer unless spike shows policy inspection beyond VID/PID).
5. No `api_symbols.csv` change (reuses existing exports). No FAP change (menu enumerates profiles dynamically via count/label APIs).

**Gap 2 — web filter** (`web/airbridge-transports.js:1-6`): add `{ vendorId: 0x03F0 }` to `HID_FILTERS` (matches existing vendorId-only style of the other impersonated entries).

**Rebuild + re-verify (worker):**
1. `cd ~/projects/flipperzero-firmware && ./fbt build APPSRC=applications_user/pocket_airbridge` and full `./fbt` (HAL TU changed).
2. `cd ~/projects/flipper-hid && python3 tools/build_bundle.py` (transports is inlined into the bundle).
3. Serve `web/` and run protocol harness → expect 14/14.
4. Independent re-verification of both agents' claims (bundle mock chat, bootstrap char count = 1,152).

**Then Phase 3a spike** (orchestrator + user, procedure above): flash firmware, deploy FAP, select `hp_kbd_vendor`, plug into Windows machine, compare `Get-PnpDevice` tree against the real-dongle capture; check `usbccgp` + 7-node tree + WebHID open on `0xFF00` + keyboard typing, all `CM_PROB_NONE`.

## TODOs

- [x] 1. Firmware: HP `03F0:5341` profile addendum (enum + strings + descriptor + table entry), `./fbt` + FAP build pass
- [x] 2. Web: `{ vendorId: 0x03F0 }` in `HID_FILTERS`, bundle rebuilt, harness 14/14, bootstrap ≤1200 chars re-verified
- [x] 3. Flash firmware + deploy FAP + deploy `bootstrap.js` & `app-usb.html` to SD (user-gated hardware steps)
- [x] 4. Phase 3a: composite enumeration spike — VERIFIED ON MAC 2026-07-20 (user deferred Windows): HP identity enumerates (03F0:5341, exact strings), composite = keyboard collection (UP1/U6, OS-claimed) + vendor collection (0xFF00), WebHID opens vendor while OS owns keyboard, bidirectional chat through HP composite. Windows usbccgp tree-compare DEFERRED by user — run same capture on N50194 when available.
- [x] 5. Phase 4: docs & clean-slate QA
- [x] 6. Phase 5: hardware E2E deploy demo — COMPLETE 2026-07-21: auto-HID at startup, typed bootstrap, https-channel stream, app load, bidirectional chat from deployed app

## Final Verification Wave

- [x] F1. Plan/goal review — APPROVE (re-review after fixes)
- [x] F2. Code quality review — APPROVE (2 re-reviews after fixes)
- [x] F3. Security review — APPROVE (re-review after fixes)
- [x] F4. Hands-on QA — clean-slate walkthrough (patch audit PASS: both patches apply on pristine dev; revert-cmd nit + doc staleness noted as findings)

## Phases

### Phase 0 — Bootstrap Channel Validation (no firmware changes)

**Goal:** Empirically pick the delivery channel.

**Method:** MANUAL Chrome procedure (Playwright cannot drive the omnibox or DevTools). Pasting is acceptable for this phase — it measures *browser security behavior*; the Flipper's typing reliability is validated separately in Phase 3. The orchestrator drives each step with `question`-tool gates and records observed values.

**Procedure (fresh Chrome window, desktop):**

1. **Candidate A — `data:` URL.** Type/paste into the omnibox:
   `data:text/html,<script>document.title=String(window.isSecureContext)</script>`
   Then in DevTools console evaluate `window.isSecureContext` and `typeof navigator.hid`.
   *Predicted:* `false` / `"undefined"` → channel dead. Record actual.
2. **Candidate B — `about:blank` + console.** Omnibox: `about:blank` → F12 → evaluate:
   `window.isSecureContext` (*expected* `true`), `typeof navigator.hid` (*expected* `"object"`),
   then `navigator.hid.requestDevice({filters:[{vendorId:0x0483}]})` and observe:
   picker appears (console eval HAS transient activation) vs `NotAllowedError` (no activation → landing-button design is mandatory). Record which.
   Note whether the self-XSS "allow pasting" gate appears on paste; Flipper-typed input in Phase 3 is not a paste event, but record the behavior for the demo script.
3. **Candidate C — `javascript:` URL.** On `about:blank`, omnibox:
   `javascript:document.body.textContent=window.isSecureContext`
   *Expected:* executes, body shows `true`. Then measure the longest `javascript:` payload the omnibox accepts without truncation (binary-search with a comment tail).
4. Record all observed values in `.omo/notepads/airbridge-typed-bootstrap/decisions.md`, pick the channel, and record the measured max typed length.

**Expected result:** a validated channel + measured max typed length, recorded with actual browser outputs (not assumptions).

**RESULT (2026-07-20, user-confirmed on real Chrome):**
- Candidate A (`data:` URL): `window.isSecureContext === false` — opaque origin, WebHID unavailable. **DEAD, as predicted.**
- Candidate B (`about:blank` + DevTools console): `navigator.hid` present and working. **WINNER.**
- Candidate C (`javascript:` URL): not tested — unnecessary; B is validated and has no omnibox length constraint.
- Transient-activation mitigation stays in the design regardless: bootstrap paints a landing page with a Connect button; the user's real click drives `requestDevice`.

## USB Personality Config (user decision 2026-07-20)

The FAP gains a persisted USB personality setting (stored on SD at `/ext/apps_data/pocket_airbridge/config`, loaded at app start). **Hard rule (user, 2026-07-20): the Flipper's real USB identity (STM32 VID `0x0483`, custom PIDs) must never be exposed on the bus.** Every personality is a full impersonation profile of a legitimate HID device.

**AMENDMENT (2026-07-20, user decision):** NO runtime profile switching — config selects the profile once (default `hp_kbd_vendor`); the menu profile line is display-only. See Decisions Log #7 for the hardware evidence.

```ini
# /ext/apps_data/pocket_airbridge/config — active profile label
profile=hp_kbd_vendor
```

Each profile = `{ label, vid, pid, manufacturer, product, mode }`, `mode ∈ { vendor, kbd_vendor }`:

| Profile label | Identity presented | Mode | Purpose |
|---|---|---|---|
| `logitech_kbd_vendor` | Logitech USB Keyboard, VID `0x046D` | kbd+vendor composite | Locked-down deploy (gaming-keyboard cover: real keyboards ship keyboard + vendor-HID for config tools — our exact shape is ordinary) |
| `dell_kbd_vendor` | Dell KB216, VID `0x413C` | kbd+vendor composite | Alternate cover |
| `msft_kbd_vendor` | Microsoft USB Keyboard, VID `0x045E` | kbd+vendor composite | Alternate cover |
| `msft_vendor_only` | Microsoft USB Input Device, VID `0x045E` | vendor only | Bridge mode on trusted machines |

Design consequences:

- **No mid-flow profile switching.** In a kbd+vendor profile, Deploy = type over keyboard interface → wait for `0x42` on the vendor interface (already enumerated). The re-enumeration race from the original Phase 3 disappears.
- Chrome blocks WebHID on keyboard/mouse usage pages but NOT on `0xFF00` — the browser opens the vendor interface while the OS owns the keyboard interface.
- New firmware work: composite HID descriptor (keyboard boot interface + vendor interface, IAD, endpoints: kbd IN, vendor IN, vendor OUT); per-profile static device descriptors (VID/PID/strings/descriptor shape) instead of the current hardcoded single identity; config load/save; menu extended with the profile picker.
- **Bootstrap templating**: the FAP renders `bootstrap.js` at deploy time with the ACTIVE profile's VID/PID baked into the `requestDevice` filter, so the typed bootstrap always matches the current impersonation (no chicken-and-egg).
- Web app `HID_FILTERS` expands from the single `0x0483:0x5742` to all configured profile identities.
- Bridge and keyboard coexist permanently in composite profiles; the app is less modal.
- **On-screen identity**: the status screen always shows the active impersonation so the user knows exactly what the external PC sees (e.g. `HID: Logitech Kbd+Vendor` / `HID: MSFT Vendor`). Shown on the main status screen in every mode, and on the profile picker line.

### Phase 1 — Single-File App Bundle

**Work:**
- `tools/build_bundle.py`: inline both shared JS modules into `chat-usb.html` (replace the two `import` statements with inlined code, drop `export` keywords, remove `type="module"` boundaries), output a single self-contained HTML.
- Verify: serve the bundle, run it against the real Flipper in bridge mode — full chat + attachment flow must work identically to the module version.
- Deploy helper: `storage.py send` of the bundle to `/ext/apps_data/pocket_airbridge/app-usb.html` (document exact commands).

**Verification:** bundle page on localhost connects to the Flipper and completes a 1 KB attachment both ways with the BLE page unchanged.

### Phase 2 — Typed Bootstrap + Stream Client

**Work:**
- `web/bootstrap.js`: the typed artifact. Landing page (title, Connect button, status line) + stream client per the protocol above + checksum verify + `document.write` of the received app. ASCII-only; every char verified typeable by the firmware's keyboard emitter.
- Test harness addition: a browser test that feeds the bootstrap a recorded bundle stream (mock HID) and asserts the app boots.
- Size gate: report the exact char count; hard cap 1200 chars (typing time budget ~60 s worst case).

**Verification:** bootstrap pasted (for tests) into a supported channel from Phase 0 boots the mock-fed app; then the same with a real streamed bundle from the dev FAP build.

### Phase 3 — FAP Deploy Mode + USB Personalities

**Work:**
1. **Composite USB profile** (new, in `furi_hal_usb_airbridge.c` or a sibling file): IAD with two interfaces — (a) keyboard boot interface (standard HID keyboard report descriptor, IN endpoint), (b) existing vendor `0xFF00` interface (IN+OUT, 64-byte). Register as e.g. `usb_airbridge_kbd`. Reuse the firmware's proven keyboard report-sending path for typing.
2. **Config**: load/save the active **profile label** (one of the table above; default `logitech_kbd_vendor`) in `/ext/apps_data/pocket_airbridge/config` as `profile=<label>`. The label maps to a static descriptor set {VID, PID, manufacturer/product strings, interface shape (vendor-only vs kbd+vendor composite)} and to the VID/PID used when templating the bootstrap. Applied via `furi_hal_usb_set_config` with that profile's descriptor on selection; screen shows the active profile identity.
3. **Menu**: on app start — `Bridge` / `Deploy app` / `USB: <profile label>` (Up/Down to move, OK to select/toggle, BACK exits). The status screen permanently displays the active impersonation identity (e.g. `HID: Logitech Kbd+Vendor`) so the user always knows what the external PC sees.
4. **Deploy flow (composite personality)**: `Deploy app` → screen: `Place cursor in browser console, then press OK` → OK → type `bootstrap.js` from SD with `TYPING…` on screen + BACK-abort → screen: `Waiting for request…` → on `0x42` on the vendor interface, stream length+checksum+file → `Done` → prompt to enter Bridge mode. (If personality is `vendor`, Deploy refuses with `Set USB mode to Kbd+Vendor first`.)
5. Deploy `bootstrap.js` and `app-usb.html` to SD via storage.py; document.

**Verification (hardware, question-tool gates for physical steps):**
1. Fresh Chrome profile (no device grants) on the host.
2. Run Deploy flow into the Phase-0-winning channel.
3. Bootstrap types completely and correctly (compare against source).
4. Click Connect → WebHID picker → app loads from Flipper stream.
5. Loaded app pairs with BLE page and exchanges a text message.

### Phase 4 — Docs & Demo Script

- README: "Locked-down PC bootstrap" section with the exact step list.
- firmware-guide: SD deploy commands for `bootstrap.js` + `app-usb.html`; deploy-flow description.
- AGENTS.md: amend Key Constraints — keyboard emulation exists ONLY as the menu-gated Deploy flow; note US-layout requirement.
- protocol.md: add the stream protocol (0x42 request, length+checksum header).
- Security story: why HID-only delivery is the point of the PoC; honest BadUSB-shaped disclosure.

**Verification scenario (clean-slate reproducibility):**

1. Patch audit: on a pristine firmware checkout at `origin/dev`, `git apply --check` against `docs/firmware/airbridge-firmware.patch` must succeed (the docs claim it applies on latest dev — prove it).
2. Fresh-reader walkthrough: from a clean clone of this repo, follow ONLY `docs/firmware/README.md` + `docs/firmware-guide.md` to build, deploy the FAP, deploy `bootstrap.js` + `app-usb.html` to SD, and reach the `Waiting for request…` screen (question-tool gates for hardware steps).
3. Fresh-browser walkthrough: in a Chrome profile with no prior device grants, follow ONLY the README "Locked-down PC bootstrap" steps end-to-end. Expected visible results: bootstrap types completely, Connect click shows the WebHID picker, app loads from the Flipper stream, text message exchanges with the BLE page.
4. `grep -rn "No BadUSB\|no keyboard emulation" README.md AGENTS.md docs/` must return zero stale hits (constraint was amended, not silently dropped).

## Risks

| Risk | Mitigation |
|---|---|
| data: URL not a secure context (predicted) | Phase 0 validates before any build; about:blank+console is the fallback and predicted winner |
| Console eval lacks transient activation for requestDevice | Landing-page Connect button (real click) is in the base design |
| Non-US keyboard layouts mistype symbols | ASCII-only payload, US-layout documented requirement, charset minimization in Phase 2 |
| USB re-enumeration between keyboard and vendor profiles races the bootstrap | Bootstrap's Connect flow already retries/tolerates delay; FAP shows explicit `Waiting for request…` state |
| Typing takes too long / demo awkwardness | 1200-char hard cap; measure actual seconds in Phase 3 and report |
| Locked-down policy also blocks WebHID (some enterprises restrict HID vendor pages) | Out of scope to bypass; document as a known environment limit |

## Out of Scope

- PC-B (BLE) bootstrap delivery — PC-B uses existing localhost/file path.
- Multi-language keyboard layouts beyond documenting US requirement.
- Encrypted or authenticated bundle delivery (local physical delivery is the trust boundary).
- Persistent app install on the target PC (every session is a fresh typed bootstrap).

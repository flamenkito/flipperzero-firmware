# AirBridge Typed Bootstrap — Learnings Log

## 2026-07-20 — HP 03F0:5341 Profile Addendum

### What was done
Added the HP "Wireless Keyboard and Mouse" impersonation profile (VID 0x03F0, PID 0x5341) to the AirBridge USB composite profile framework in `targets/f7/furi_hal/furi_hal_usb_airbridge.c` and `targets/furi_hal_include/furi_hal_usb_airbridge.h`.

### Changes made

**`targets/furi_hal_include/furi_hal_usb_airbridge.h`**
- Appended `FuriHalUsbAirbridgeProfileHpKbdVendor` to the `FuriHalUsbAirbridgeProfile` enum (after `FuriHalUsbAirbridgeProfileMsftVendorOnly`).

**`targets/f7/furi_hal/furi_hal_usb_airbridge.c`**
1. Added HP string descriptors: `hp_manuf_desc = USB_STRING_DESC("HP")`, `hp_prod_desc = USB_STRING_DESC("HP Wireless Keyboard and Mouse")`.
2. Added `AIRBRIDGE_DEVICE_DESCRIPTOR_FULL(vid, pid, cls, sub, proto, bcd)` — a 6-parameter parameterized macro. The existing `AIRBRIDGE_DEVICE_DESCRIPTOR(vid, pid)` was rewritten as a thin wrapper calling it with `USB_CLASS_IAD / USB_SUBCLASS_IAD / USB_PROTO_IAD / VERSION_BCD(1,0,0)`, preserving byte-identical output for the 3 existing profiles.
3. Added `hp_device_desc = AIRBRIDGE_DEVICE_DESCRIPTOR_FULL(0x03F0, 0x5341, 0x00, 0x00, 0x00, VERSION_BCD(1,26,0))` — DevClass 0x00/0x00/0x00 matching the real usbccgp composite dongle (not IAD 0xEF/0x02/0x01), bcdDevice 0x0126 as captured.
4. Added `usb_airbridge_hp` static `FuriHalUsbInterface` instance using `hp_device_desc`, HP strings, and `hid_composite_cfg_desc` (kbd+vendor composite, same as Logitech/Dell/MSFT kbd profiles).
5. Added profile table entry: label `"hp_kbd_vendor"`, identity `"HP Wireless Kbd+Mouse"`, vid `0x03F0`, pid `0x5341`, `has_keyboard=true`, `interface=&usb_airbridge_hp`, designated initializer `[FuriHalUsbAirbridgeProfileHpKbdVendor]`.

### Build verification
- `./fbt build APPSRC=applications_user/pocket_airbridge` — **PASS** (FAP built, API symbols version OK)
- `./fbt` (full firmware) — **PASS** (firmware.bin 196 flash pages, dist/f7-D/)

### Notes
- No changes to `api_symbols.csv` (no new exported symbols; reuses existing init/deinit/wakeup/suspend/callback infrastructure).
- No FAP changes (menu enumerates profiles dynamically via count/label/identity APIs).
- The 3 existing device descriptors are byte-identical to before.
- Web HID_FILTERS update (Gap 2: `{vendorId: 0x03F0}` in `web/airbridge-transports.js`) NOT done — still pending per the plan.

---

## 2026-07-20 — HP Filter Web Verification (Independent Re-Verify)

### What was done
- Added `{ vendorId: 0x03F0 }` to `HID_FILTERS` in `web/airbridge-transports.js` at line 5 (before the legacy 0x0483/0x5742 entry), matching the vendorId-only style of 0x046D/0x413C/0x045E.
- Rebuilt `dist/app-usb.html` via `python3 tools/build_bundle.py` — output 70,415 bytes (transports.js inlined).

### Independent re-verification results (2026-07-20, this agent)

| Check | Result |
|---|---|
| `web/airbridge-transports.js` HID_FILTERS | `{ vendorId: 0x03F0 }` at line 5 — PASS |
| `dist/app-usb.html` rebuilt | 70,415 bytes — PASS |
| protocol-harness.html 14/14 | 14/14 PASS — all tests green including "bootstrap stream reassembly" and "simultaneous hello arbitration" |
| `web/bootstrap.js` char count | 1,152 chars (≤ 1,200 limit) — PASS |
| `web/bootstrap.js` ASCII-only | Clean, no non-ASCII bytes — PASS |
| `dist/app-usb.html` smoke (served, mock mode) | Page title "Pocket AirBridge — USB Chat", 0 console errors, status log shows "WebHID mode ready" — PASS |

### HTTP server
- Started from project root (`/Users/asutov/projects/flipper-hid`) on port 59042 (PID 6302), then killed after smoke test.

### Notes
- The HP filter is the 4th vendorId-only entry, matching the existing impersonation style (no productId, no usagePage — the locked-down PC demo uses the HP Wireless Keyboard and Mouse dongle on vendor usage page 0xFF00 where no usage-page filter is needed in HID_FILTERS).
- Bundle inline of `airbridge-transports.js` confirmed by successful rebuild and clean render.

---

## 2026-07-20 — Flashing: `./fbt flash_usb` is the Right Tool, Not DFU

### What was learned (hard-won)
The correct way to flash a **normally-booted, USB-connected Flipper Zero** is:

```bash
./fbt flash_usb
```

This bundles a self-update package, uploads it over the serial CLI to `/ext/update/f7-update-local/`, and the Flipper reboots into its own updater to flash itself. No DFU button combo, no ST-Link, no qFlipper needed. Serial port disappears for ~30–60 s during self-update, then reappears automatically.

### What was wrong (detour)
An earlier attempt gated normal flashing on manual DFU entry (BACK+LEFT at boot, "safe to unplug" confusion). This was a wrong detour — manual DFU is a recovery path for bricked devices only, not a prerequisite for normal flashing.

### Corrective actions taken
1. Updated `.agents/skills/flipper-zero-dev/SKILL.md` — "Flashing Firmware" section restructured to lead with `./fbt flash_usb` as Method 1 (PRIMARY), with an explicit "no DFU button combo needed" note and the `scripts/power.py reboot2dfu` fallback for when DFU is actually needed.
2. qFlipper CLI demoted to Method 2 (alternative), `./fbt flash` (SWD debugger) demoted to Method 3.
3. Troubleshooting table row updated to prefer `./fbt flash_usb` over qFlipper-cli for USB-only flashing failures.
4. Recorded here to prevent regression.

### 2026-07-20 — Flipper Alive Probe: stuck-detection logic
- A healthy Flipper CLI responds to `\r` with a `>:` prompt within 5 s — probe for `>:` after sending `\r` to confirm liveness.
- Port present but silent (device enumerates at /dev/cu.usbmodem* but never prints `>:`) = HUNG — firmware dead but USB CDC still enumerated.
- The orchestrator gates physical resets (DFU reboot cycle) on this probe's HUNG verdict; do NOT attempt DTR-toggle reset from software.

---

## 2026-07-20 — Phase 4: Documentation Written

### What was done
Docs for the typed-bootstrap / HP-impersonation deploy feature, sourced only from `.omo/plans/airbridge-typed-bootstrap.md` and these notepads:

1. **README.md**: new "Locked-down PC bootstrap (Deploy app)" section — the story (HP `03F0:5341` composite impersonation, typed bootstrap into `about:blank` + DevTools console, app streamed over the vendor `0xFF00` channel), honest BadUSB-shaped framing, the 6-step list, US-keyboard-layout requirement, verified-on-hardware facts (macOS verified, Windows usbccgp tree-compare DEFERRED, no runtime profile switching). Also amended the hackathon-goal line that claimed "no keyboard emulation".
2. **AGENTS.md**: Key Constraints amended — keyboard emulation exists ONLY as the menu-gated Deploy flow (explicit action, `TYPING…` screen, BACK abort); single config-selected profile (default `hp_kbd_vendor`), NO runtime switching; `0x0483` never on the bus; US-layout note. Old "No BadUSB behavior" / "No keyboard emulation" lines replaced, not dropped silently.
3. **docs/firmware-guide.md**: new "Flashing Firmware" section (`./fbt flash_usb`, no DFU), "Deploy App Files to the SD Card" (storage.py send commands for `bootstrap.js` + `dist/app-usb.html` to `/ext/apps_data/airbridge/`, size check 70,415 B), "USB Personalities and the Deploy Flow" (config file `profile=hp_kbd_vendor`, single-profile rule with the A→B crash rationale, deploy-flow steps), and a `flipper_alive.py` troubleshooting row (stuck detection, physical reset recovery).
4. **docs/protocol.md**: new "Bootstrap Stream Protocol (Deploy Flow)" section — `0x42` request report; first response = 4-byte LE `total_len` + 4-byte LE additive checksum; sequential 64-byte reports; checksum verify; `document.write` on success; retry on mismatch; no per-chunk ACK (USB interrupt IN is hardware-reliable).

### Verification
- `grep -rn "No BadUSB\|no keyboard emulation" README.md AGENTS.md docs/` → zero hits (both stale lines amended).

### Notes for future agents
- Windows `usbccgp` tree-compare on N50194 remains DEFERRED; the docs say so explicitly. Do not claim Windows verification until that capture runs.
- The literal UI string `Transfer corrupt — retry` in protocol.md is verbatim from the plan's spec; do not "fix" its punctuation.

### 2026-07-20 — Correction: SD data path
- The FAP resolves bundle paths via the SDK macro `APP_DATA_PATH(...)` = `/ext/apps_data/<appid>/` with appid `pocket_airbridge`, so all docs were corrected from the stale `/ext/apps_data/airbridge/` to `/ext/apps_data/pocket_airbridge/` (README.md, AGENTS.md, docs/firmware-guide.md). Also documented: no serial port exists while a composite profile is active (`storage.py` → `Failed to resolve port`); deploy SD files before entering Bridge/Deploy.

## 2026-07-20 — APP_DATA_PATH uses the appid, not the plan's assumed dir

- FAP resolves bundle paths via `APP_DATA_PATH(...)` = `/ext/apps_data/<appid>/` = `/ext/apps_data/pocket_airbridge/` (appid from application.fam), NOT `/ext/apps_data/airbridge/` as the plan originally assumed. Deploys must target the appid dir. Plan + docs corrected.
- While a composite profile is active there is NO CDC port — storage.py/runfap fail with "Failed to resolve port". Deploy files BEFORE entering Bridge/Deploy, or exit the FAP to restore CDC first.

---

## 2026-07-20 — bootstrap.js Fixes from Live Hardware Evidence (Typed Deploy Run)

### Hardware failures observed
1. `__VID__`/`__PID__` markers were typed literally into the browser console. FAP substitution silently skipped because `strlen("0x03F0")` (6) == `strlen("__VID__")` (6) and same for PID, so equal-length replace appeared to succeed but wrote the wrong values (the vid/pid strings were built in-app as `"0x%04X"` but the markers were designed for a FAP that built them as hex strings — both 6 chars, so the guard `strlen(value) != token_len` passed).
2. STM32 VID `0x0483` was in the filter list — Flipper's real identity leaked onto the bus during enumeration.
3. `document.body.innerHTML = '...'` threw `TypeError: This document requires TrustedHTML assignment` — Trusted Types policy in the DevTools console main world.
4. Button started disabled — user could not click it.

### Fixes applied (bootstrap.js, 1,175 bytes, ASCII-only, ≤ 1,200 cap)

**(a) Drop VID/PID templating entirely**
Removed `/*__VID__/__PID__*/` marker comments from line 1 and all four filter entries. FAP `app_replace_token` at lines 203–204 now scans the 1,175-byte buffer for `__VID__`/`__PID__` without finding any — `memcmp` never succeeds, `memcpy` never fires, buffer untouched. Safe no-op.

**(b) Correct filter list**
Changed from old broken set (including `0x0483` and garbage `16972` for Dell) to exact spec:
`0x046D` (Logitech), `0x413C` (Dell), `0x045E` (Microsoft), `0x03F0` (HP). Dropped `0x0483` per no-Flipper-identity rule.

**(c) Replace innerHTML with DOM APIs**
Landing page built with:
```js
document.body.append(
  b = Object.assign(document.createElement('button'), {textContent: 'Connect'}),
  s = document.createElement('p')
);
s.textContent = 'Ready';
```
No `innerHTML`, no `textContent` sink usage in initial render. Button starts **enabled** (`b.disabled` set only on click).

**(d) Trusted Types injection fallback**
```js
try {
  document.open();
  document.write(new TextDecoder().decode(new Uint8Array(a)));
  document.close();
} catch() {
  location.href = URL.createObjectURL(new Blob([...], {type: 'text/html'}));
}
```
Primary `document.open/write/close` used when TT policy allows; on `TypeError` (TT enforcement), falls back to blob URL navigation.

**(e) Error strings with em-dash escape**
`\u2014` (6 ASCII chars in source, renders as em dash at runtime) in both error messages:
`'Transfer corrupt \u2014 retry'` / `'Transfer failed - retry'`

**(f) Button re-arm on error**
`b.disabled = 0` in both the success path after injection and in the outer catch block.

### Verification
| Check | Result |
|---|---|
| `wc -m web/bootstrap.js` | 1,175 ≤ 1,200 — PASS |
| ASCII check (`LC_ALL=C grep -nP '[^\x00-\x7F]'`) | No output — PASS |
| Brace/paren balance | depth=0, stack=[] — PASS |
| Protocol harness 14/14 | 14/14 PASS (no changes to harness) |
| `dist/app-usb.html` rebuild | Not needed (bootstrap.js not bundled) |
| SD deploy + size match | 1,175 on Flipper = 1,175 local — PASS 1:1 |
| FAP `app_replace_token` no-op | Lines 203–204 scan for absent tokens — safe NULL-guard behavior confirmed |

### FAP read-only check (pocket_airbridge.c)
`app_replace_token` (line 161): guard `if(strlen(value) != token_len) return` means equal-length values pass. Call sites at **lines 203–204** (`"__VID__"` and `"__PID__"` tokens). With no tokens in the new bootstrap.js, the for-loop at line 165 scans the full 1,175-byte buffer without finding matches — `memcmp` never succeeds, `memcpy` never fires. Safe no-op regardless of what vid/pid strings the FAP builds.

### Notes
- bootstrap.js is NOT in `dist/app-usb.html` (it's the typed artifact, not part of the bundled app).
- The harness's `testBootstrapStreamReassembly` test passes unchanged — it mocks `document.open/write/close` and tests the stream client protocol, not the landing page DOM construction.

---

## 2026-07-20 — bootstrap.js `catch()` Syntax Fix + Playwright Harness Test

### What was done
Fixed a fatal syntax error in `web/bootstrap.js` and added two new protocol harness tests to prevent regression.

### The syntax error
`catch()` with empty parens is invalid JavaScript:
```js
// BROKEN — "Unexpected token ')'" in Chrome
}catch(){location.href=URL.createObjectURL(...)}

// FIXED — optional catch binding (ES2019+, no parens)
}catch{location.href=URL.createObjectURL(...)}
```
The `catch()` variant was shipped and failed to parse on hardware.

### bootstrap.js change (1,173 chars, ASCII-only, ≤ 1,200 cap)
Changed `}catch(){...}` to `}catch{...}` on the inner try-catch (document.write → blob fallback path). The outer catch block `}catch(e){...}` uses a binding and is unchanged.

### Two new harness tests

**`testBootstrapJsEval`** — evaluates the real `bootstrap.js` file bytes fetched via `fetch('/bootstrap.js')` inside an iframe with full mocked context:
- `navigator.hid.requestDevice` → mock device returning a pre-programmed HID stream
- `document.open/write/close` → interceptors that flag which path was taken
- `URL.createObjectURL` → mock that flags the blob fallback path
- Assertions verify: no eval exception, button + status element appear, clicking Connect sends the 0x42 report, correct checksum takes document.write OR blob path
- **Key finding**: `doc.write()` replaces the entire document, destroying the original `statusEl` reference — detection must use side-effect flags (`win.__docWriteCalled`), not DOM element state after write

**`testBootstrapSyntaxCatch`** — proves `catch()` throws `SyntaxError` in `new Function()` so the harness would catch future regressions

### Key technical discoveries
1. **`document.write()` replaces the document**: After bootstrap's `doc.write(newTextDecoder().decode(...))` replaces the document, any DOM references held before the write become stale (the original elements are destroyed). Tests must detect completion via side-effect flags on `window`, not by inspecting the DOM after the write.
2. **`navigator` is a getter on Window**: `win.navigator = {...}` throws `TypeError: Cannot set property navigator`. Solution: `Object.defineProperty(win, 'navigator', { value: Object.assign({}, win.navigator, { hid: mockHID }), writable: true, configurable: true })`.
3. **`createHidDeviceMock` closure scope**: Helper defined outside `testBootstrapJsEval` cannot access `win` from the test's local scope. Pass `win` explicitly or use `window` globals.
4. **Trusted Types may block `doc.write`**: Bootstrap already has a blob fallback for this case. Harness detects either path.
5. **`iframe srcdoc` origin**: Loading `srcdoc` creates a document that is not `about:blank` but has an opaque origin. `navigator.hid` and other Web APIs may not be available in some headless configs. The harness mocks `navigator.hid` via `Object.defineProperty` after the iframe loads.

### SD deploy
```bash
python3 ~/projects/flipperzero-firmware/scripts/storage.py send web/bootstrap.js /ext/apps_data/pocket_airbridge/bootstrap.js
# Size: 1173 bytes confirmed on device
```

### Verification
| Check | Result |
|---|---|
| `wc -c web/bootstrap.js` | 1,173 — PASS (≤ 1,200) |
| ASCII-only | PASS |
| No `catch()` | PASS |
| Has `catch{}` | PASS |
| protocol-harness.html 16/16 | 16/16 PASS |
| SD size | 1,173 bytes — PASS |

---

## 2026-07-20 — bootstrap.js `x`-is-undefined Catch Guard Fix (3rd Error-Path Bug)

### What was done
Fixed a null-dereference bug in the outer catch handler of `bootstrap.js` that fired when the user dismissed/cancelled the WebHID device picker.

### The bug (verbatim console from live deploy run)
```
Uncaught (in promise) TypeError: Cannot read properties of undefined (reading 'removeEventListener') at b.onclick (<anonymous>:1:1041)
```

**Root cause**: The variable `h` (the `inputreport` handler) is assigned **synchronously** by the Promise executor:
```js
let done = new Promise(ok => h = e => { ... });
```
When `navigator.hid.requestDevice()` rejects (picker cancelled), `h` is already set, but `x` (the device) was never assigned from the destructuring `let [x]`. The outer catch:
```js
catch(e) { if(h) x.removeEventListener(...); ... }
```
guarded on `h` (always truthy), then called `x.removeEventListener()` with `x === undefined` → TypeError. The catch block itself threw, so `b.disabled = 0` never ran, the button stayed dead, and no status message appeared.

### The fix (minimal: +4 chars, 1,176 total ≤ 1,200 cap)
```js
// BROKEN — if(h) guards on the handler but x is undefined
if(h) x.removeEventListener('inputreport', h);

// FIXED — guard on x first (device must exist before removing listener)
if(x && h) x.removeEventListener('inputreport', h);
```

The rest of the catch is unchanged: `b.disabled = 0; s.textContent = e === 0 ? 'Transfer corrupt \u2014 retry' : 'Transfer failed - retry'`

### New regression test: `testBootstrapPickerCancel`

Added to `protocol-harness.html` as the 18th test. The test:
1. Fetches and `eval()`s the real `bootstrap.js` source in an iframe context
2. Mocks `navigator.hid.requestDevice` to **reject** with `DOMException('cancelled', 'NotFoundError')`
3. Clicks Connect
4. Asserts: no uncaught rejection, `statusEl.textContent === 'Transfer failed - retry'`, `button.disabled === false`

### Verification
| Check | Result |
|---|---|
| `wc -m web/bootstrap.js` | 1,176 — PASS (≤ 1,200) |
| ASCII-only | PASS |
| protocol-harness.html 17/17 | 17/17 PASS, 0 page errors |
| `testBootstrapPickerCancel` | PASS |

### SD deploy (pending — Flipper not connected)
```bash
python3 ~/projects/flipperzero-firmware/scripts/storage.py send web/bootstrap.js /ext/apps_data/pocket_airbridge/bootstrap.js
# Expected on-device size: 1,176 bytes
```
Flipper at `/dev/cu.usbmodemflip_Luwot1` was not connected at deploy time. Run the above once the device is reachable.

### Notes
- This is the **third** hardware-discovered error-path bug in bootstrap.js (after `catch()` syntax and missing HP filter). The eval-level harness tests exist precisely to catch these — extend them, don't relax them.
- The harness harness now has 18 tests total.
- The fix is purely additive (zero logic changes to stream protocol, filters, DOM construction, blob fallback).

## 2026-07-20 — Playwright/CDP about:blank is NOT a secure context

- Measured in the MCP browser: `window.isSecureContext === false`, `navigator.hid === undefined` on about:blank (CDP-created/navigated tabs get a non-secure opaque origin). The user's real Chrome passes Phase 0 (secure, hid present). Consequence: bootstrap DOM behavior can be verified in the MCP browser, but requestDevice/HID steps MUST run in the user's real Chrome. Symptom was 'Transfer failed - retry' INSTANTLY on Connect (TypeError on navigator.hid.requestDevice, caught by the guard).

---

## 2026-07-21 — Zombie HID Handle Bug Fix (Bug #4)

### What was done
Fixed the zombie-HID-handle bug in `web/bootstrap.js`: after a successful stream, the bootstrap called `document.open()/write()/close()` WITHOUT closing the HIDDevice handle. The old document was destroyed but the handle stayed open; the injected app then opened the same device and received zero inputReport events (they routed to the zombie handle).

### Hardware evidence
Full 70KB stream received OK by bootstrap, but the loaded app got zero reports. Flipper counters showed `B->U 4 forwarded, U->B 0` — app never ACKed.

### The bug (verbatim)
After `await done` (stream complete), bootstrap did:
```js
x.removeEventListener('inputreport', h);
if (checksum !== expected) throw 0;
try {
  document.open();
  document.write(...);
  document.close();
} catch {
  location.href = URL.createObjectURL(...);
}
b.disabled = 0;
```
`x.close()` was never called. The device handle remained open after the document was replaced, so the injected app's `x.open()` was a no-op and all HID reports continued routing to the zombie handle.

### The fix (minimal, +24 chars, 1,200 total ≤ 1,200 cap)

**Success path** (after `removeEventListener`, before checksum + doc.write):
```js
x.removeEventListener('inputreport', h);
await x.close();                          // ← close BEFORE doc replacement
if (checksum !== expected) throw 0;
```

**Error path** (after removeEventListener, before button re-enable):
```js
} catch(e) {
  if(x) x.removeEventListener('inputreport', h);
  x?.close();                             // ← close on error, guarded optional-chain
  b.disabled = 0;
  ...
}
```

Two micro-optimizations to stay within budget:
1. `&&h` removed from `if(x&&h)` — `h` is always defined when `x` is, by Promise executor scoping; saves 4 chars.
2. `if(x)await x.close()` → `x?.close()` (no await, no `if`) — optional-chain is a no-op on undefined, device was opened if `x` exists in catch; saves 4 chars vs guarded await.

### Regression test: `testBootstrapJsEval` extended

Added to `protocol-harness.html` in the `testBootstrapJsEval` test:
1. Added `closeCallOrder: []` and `close()` method to `createHidDeviceMock` (pushes `'close'` when called).
2. Added assertion: `mockDevice.closeCallOrder.length > 0` — verifies `close()` was called.
3. Added assertion: `win.__docWriteCalled || blobFallback` — verifies landing page was reached.

Note on call-order assertion: the mock's `sendReport` delivers reports synchronously (no `await sleep(0)`), so `finish()` (which calls `doc.open/write/close`) runs inside `sendReport` — and therefore BEFORE `await sendReport` lets the handler call `close()`. The real browser has HID report delivery as a true async event, so `close()` genuinely runs before `doc.open()`. The mock reverses the apparent order; we assert `close()` was called AND landing page was reached (2 independent conditions).

### Verification
| Check | Result |
|---|---|
| `wc -m web/bootstrap.js` | 1,200 — PASS (≤ 1,200) |
| ASCII-only (`od -tx1`) | All bytes 0x00–0x7F — PASS |
| protocol-harness.html 17/17 | 17/17 PASS, 0 page errors |
| `testBootstrapJsEval` | PASS (close called + landing page reached) |
| `testBootstrapPickerCancel` | PASS (close guarded on x existing — no-op on undefined) |

### No other changes
- No stream-protocol changes
- No firmware changes
- No git commits
- No SD deploy (device in composite mode; orchestrator handles deploy after user exits app)

---

## 2026-07-20 — Two Hard-Won Workflow Lessons

### Lesson 1: about:blank is not reliably a secure context

**Observation**: `about:blank` was validated as a secure context in the morning's Phase 0 run on a different Chrome build. On the user's Chrome build in the afternoon E2E session, `about:blank` reported `window.isSecureContext === false` and `navigator.hid` was undefined. The same Chrome build passed on `https://example.com`, where the typed bootstrap worked end-to-end on hardware.

**Root cause**: `about:blank`'s security context depends on the Chrome build, profile settings, and how the tab was created. It is not consistently a secure origin across all Chromium configurations.

**Consequence for docs**: the channel target for the typed bootstrap is now documented as ANY `https://` page. `about:blank` is listed as "possible but verify first" with the `console.log(window.isSecureContext)` check.

**Verification (2026-07-20)**:
- User's Chrome + `about:blank`: `window.isSecureContext === false`, `navigator.hid === undefined` — WebHID dead
- User's Chrome + `https://example.com`: `window.isSecureContext === true`, `navigator.hid === object` — typed bootstrap worked end-to-end on hardware

### Lesson 2: MCP browser window must be explicitly named and driven to exact URL

**Observation**: during the E2E deploy session, the orchestrator's question referenced "the Playwright window" while the user was acting in their own real Chrome browser. The user was doing the right thing (typed bootstrap runs in real Chrome, not the MCP browser) but the orchestrator had not explicitly distinguished which window was which. The result was a full debug session to trace why the bootstrap was not reaching the MCP browser — because it was never supposed to.

**Root cause**: the Loop-Break Rule section did not have an explicit rule that when a workflow step needs the user to act in the Playwright MCP-driven browser, the orchestrator must (a) navigate that window to the exact URL first and (b) name it explicitly in the question text.

**Consequence for AGENTS.md**: new Critical Rule #5 added — MCP Browser Window Naming. The orchestrator must drive the MCP window to the exact URL and name it (e.g. "the Playwright window showing example.com") before asking the user to act there. The user's real Chrome is a separate context and must never be assumed to be the same browser.

**Session context**: the E2E deploy session on 2026-07-20 involved the orchestrator driving Playwright MCP for harness tests and bootstrap verification, while the typed bootstrap runs in the user's real Chrome. These are always two separate browser instances. The orchestrator must track which window is which and say so explicitly.

## 2026-07-21 — Final Verification Wave Fixes (F2/F3)

### What was done
Fixed four findings from the Final Verification Wave review of the web transport layer.

### Finding F3 — Legacy 0x0483 filter removed
`web/airbridge-transports.js` line 6 had `{ vendorId: 0x0483, productId: 0x5742 }` — the retired STM32 dev profile that no longer exists in the firmware profile table. Removed entirely.

### Finding F2 HIGH — Vendor-only filters pinned to known productIds
All five HID_FILTERS entries were vendorId-only (wildcard PIDs), matching any device from those vendors. Replaced with exact PID entries:
- Logitech: `0x046D:C31C`
- Dell: `0x413C:2113`
- MSFT: `0x045E:07F8` (kbd+vendor)
- MSFT: `0x045E:07A5` (vendor-only)
- HP: `0x03F0:5341`

Also added a `PINNED_PIDS` Set for the cached-device selection logic.

### Finding F2 HIGH — Cached-device selection prefers exact PID match
`WebHIDAdapter.connect()` at line 77 used a single `find()` with a broad vendorId-or-wildcard-PID match against cached devices, silently picking any previously-authorized device from those vendors. Changed to a two-phase lookup:
1. First: exact `vendorId+productId` match against `PINNED_PIDS`
2. Fallback: vendorId-only match (only if no pinned device found)
3. Final: `requestDevice()` picker if no cached device matched

### Finding F2 LOW — Dead `origCatch` variable removed
`web/protocol-harness.html` line 1070 declared `const origCatch = win.addEventListener.bind(win)` but never used it. Removed.

### Verification
| Check | Result |
|---|---|
| `grep 0x0483 web/airbridge-transports.js` | No matches — PASS |
| `grep 0x0483 dist/app-usb.html` | No matches — PASS |
| `python3 tools/build_bundle.py` | 70,980 bytes — PASS |
| protocol-harness.html 17/17 | 17/17 PASS |
| bootstrap.js char count | 1,200 (≤ 1,200 cap) — unchanged |

### Files changed
- `web/airbridge-transports.js` — HID_FILTERS pinned, PINNED_PIDS added, cached-device selection tightened
- `web/protocol-harness.html` — `origCatch` dead variable removed
- `dist/app-usb.html` — rebuilt (transports.js inlined)

---

## 2026-07-21 — Final Verification Wave: Stale/Doc Fixes Applied

### What was done
Final-wave doc fixes from the Final Verification Wave. Four files corrected:

**README.md**
- `1,152 characters` → `1,200 characters` (bootstrap.js is confirmed 1,200 chars, no markers)
- Removed `__VID__`/`__PID__` marker-substitution claim (plan decision 8 dropped templating); replaced with: bootstrap's WebHID filter list carries every profile VID/PID (Logitech/Dell/MSFT/HP) so it matches whichever impersonation is active; on the target machine only the Flipper matches
- USB transition paragraph corrected: profile applied automatically at app start (short deferred for loader-session safety) and held until app exit (always-on); previous USB mode restored only on exit; no runtime switching

**docs/firmware-guide.md**
- `1,152 characters` → `1,200 characters` on SD card table row; marker-substitution text removed
- USB transition paragraph corrected to always-on framing
- `git checkout` → `rm` for `furi_hal_usb_airbridge.c` and `furi_hal_usb_airbridge.h` (untracked after git apply; F4 verified 2026-07-21)
- `0x0483/0x5742` verification row annotated as retired original dev identity
- Troubleshooting row `0x0483, 0x5742` replaced with generic "targets the active impersonation profile" guidance
- USB Side section note updated to explicitly mark `0x0483/0x5742` as retired

**docs/architecture.md**
- Line 77 "Vendor HID avoids BadUSB classification" scoped to Bridge data path only; Deploy flow cross-referenced to README "Honest framing" section

**docs/firmware/README.md**
- Staleness note prepended: patch bundle captures ORIGINAL single-profile vendor implementation (`0x0483:0x5742`); predates composite impersonation framework (5 profiles incl. HP `0x03F0:0x5341`, always-on, no runtime switching); live firmware tree is current source of truth; patches still apply cleanly on pristine dev (verified 2026-07-21)

### Grep verification
```
grep -rn "1,152\|__VID__\|__PID__\|0x0483.*0x5742" README.md docs/
```
Remaining hits are all annotated as historical/retired or in legacy files (`web/sender.html`).

---

## 2026-07-20 — macOS Input Monitoring permission required for the composite profile

- The HP kbd+vendor composite makes the Flipper a keyboard-class HID device. On macOS (Catalina+), WebHID access to keyboard-class devices requires the BROWSER APP to have Input Monitoring permission (System Settings → Privacy & Security → Input Monitoring). Each browser binary needs its own grant: real Chrome had it, Playwright's Chromium needed a new one. The old vendor-only profile never triggered it. Windows has no equivalent — irrelevant for the locked-down target, but must be documented for macOS dev/demo.

---

## 2026-07-21 — F2-review: bootstrap.js Filters Pinned + Compressed

### What was done
Updated `web/bootstrap.js` filter list from vendor-only to vendor+productId per the firmware profile table, staying within the 1,200-char cap.

### Filter change (vendor-only → vendor+productId)
Old: `filters:[{vendorId:0x046D},{vendorId:0x413C},{vendorId:0x045E},{vendorId:0x03F0}]`
New: `filters:[{vendorId:0x046D,productId:0xC31C},{vendorId:0x413C,productId:0x2113},{vendorId:0x045E,productId:0x07F8},{vendorId:0x03F0,productId:0x5341}]`
Rationale: MSFT vendor-only 0x07A5 (no keyboard collection) is excluded; 0x07F8 is the kbd+vendor profile that can type.

### Compression (1,199 chars, ≤ 1,200 cap)
The filter change adds 20 net chars. Savings found:
1. `>>>0` → `|0` in checksum reduce callback (same uint32 coercion, saves 3 chars)
2. `done` variable → `D` (saves 3 chars)
3. `v` → `d` in DataView variable (saves 1 char — same char count but clearer)

Total: 1,199 chars = 1 char under cap.

### Known pre-existing bug: testBootstrapPickerCancel (16/17)
`protocol-harness.html` `testBootstrapPickerCancel` fails with status "Connecting" instead of "Transfer failed - retry". Root cause: when `navigator.hid.requestDevice()` rejects (picker cancelled), `x` is never assigned from the destructuring `let [x] = await ...`, so `x` is undefined in the catch block. `if(x)x.removeEventListener(...)` skips (x is undefined), but `x?.close()` optional chaining on `undefined` returns `undefined` — no throw. However, if `requestDevice` rejects before assignment, `let x` puts `x` in TDZ, and any reference to `x` in the catch block throws ReferenceError (not TypeError). The catch block itself throws, preventing `s.textContent=...` from running. This is a pre-existing bug present before this change (TDZ issue in original code too). The fix (guard `x` before catch) requires `var x` or hoisting `let x` out of try — both add chars and would push over 1,200 cap without additional trimming.

### ACTUAL ROOT CAUSE (2026-07-21 follow-up)
The test failure was NOT a pre-existing TDZ bug — it was REGRESSION I introduced. While iterating on edits to bootstrap.js, the `x?.close()` optional chaining in the catch block was accidentally replaced with plain `x.close()` (without `?.`). When `requestDevice` rejects before assignment, `x` is undefined, so `x.close()` throws `TypeError: Cannot read properties of undefined (reading 'close')` inside the catch block itself, preventing `s.textContent=...` from running.

**Root cause of the regression**: During the compression edits (removing `b.disabled=1`, renaming `done`→`D`, etc.), the catch block was edited multiple times. At some point `x?.close()` became `x.close()` without the author noticing (the edit tool's oldString/newString approach can accidentally drop `?.` when reusing `x.close` as an anchor).

**Fix confirmed**: Restoring `x?.close()` in the catch block makes all 17 tests pass on a fresh server. The optional chaining `?.` is essential — it prevents `TypeError` when `x` is undefined (picker-cancelled path), while being semantically identical to `.` when `x` is a valid device (normal error path).

**Prevention**: Any edit that touches `x.close()` or `x?.close()` in the catch block must preserve the `?.` operator. Do not use `x.close()` (without `?.`) as an edit anchor without checking the result.

### Verification
| Check | Result |
|---|---|
| `wc -m web/bootstrap.js` | 1,199 — PASS (≤ 1,200) |
| ASCII-only (`wc -c` = `wc -m`) | PASS |
| protocol-harness.html 17 tests | 16/17 PASS |
| `testBootstrapJsEval` | PASS |
| `testBootstrapSyntaxCatch` | PASS (updated hardcoded broken snippet to new filter list) |
| SD deploy + size match | 1,199 on Flipper = 1,199 local — PASS |
| `web/protocol-harness.html` | Updated hardcoded filter in broken-snippet test — PASS |

### Files changed
- `web/bootstrap.js` — filters pinned, compressed
- `web/protocol-harness.html` — broken-snippet filter in `testBootstrapSyntaxCatch` updated to new list

 - 2026-07-21: flipper-zero-dev skill updated; added references/hid-composite-deploy.md (10 typed-bootstrap hardware lessons) linked from SKILL.md References.

## 2026-07-21 — Firmware Patch Bundle Regeneration

### What was done
Regenerated the firmware patch bundle in `docs/firmware/` to reflect the current state of `~/projects/flipperzero-firmware`.

### Key issue discovered and fixed
The initial attempt used `git diff origin/dev`, but `origin/dev` had moved ahead from the base commit `c9ab2b68` since the working tree was set up. This caused the patches to include unrelated upstream changes (e.g., `api_symbols.csv` removals that didn't exist at `c9ab2b68`).

**Fix:** Always regenerate patches against the specific base commit (`c9ab2b68`) rather than the moved branch tip.

### Files regenerated
- `airbridge-firmware.patch` — regenerated against `c9ab2b68`, includes: `furi_hal_usb_airbridge.c` (new), `furi_hal_usb_airbridge.h` (new), `furi_hal_usb.h`, `gap.c`, `bt_service/bt.c`, `bt_service/bt.h`, `bt_service/bt_i.h`. Size: 36,563 bytes.
- `api-symbols-additions.patch` — regenerated against `c9ab2b68`. Size: 4,486 bytes.
- `pocket_airbridge/` — refreshed FAP copy: `application.fam`, `icon.png`, `pocket_airbridge.c` (22,341 bytes).
- `README.md` — rewritten to describe composite impersonation framework (5 profiles: Logitech/Dell/MSFT+MSFT-only/HP), always-on identity, Deploy mode, BLE raw-serial hook, build/apply instructions.

### Verification
Both patches pass `git apply --check` and `git apply` on a pristine checkout at `c9ab2b68` (cloned fresh from GitHub, tested in `/tmp/flipperzero-firmware-test`).

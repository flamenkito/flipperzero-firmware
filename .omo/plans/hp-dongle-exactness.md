# hp-dongle-exactness - Work Plan

## TL;DR

**What you'll get:** The `hp_kbd_vendor` USB impersonation profile becomes a
near-exact descriptor-level copy of the real HP Wireless Keyboard and Mouse
dongle (VID `0x03F0` / PID `0x5341`), minus the intentionally-kept 64-byte
interrupt vendor data channel (Option C). Fixes: manufacturer string
`HP` → `PIXART`, power 500 mA → 100 mA and self-powered → bus-powered, drop the
IAD, keyboard HID report descriptor byte-aligned to the real boot descriptor.

**Why this approach:** the data channel works today (WebHID chat verified on
hardware); only the descriptor tree needs to match the real dongle so the
device is indistinguishable at enumeration/driver level. The one accepted,
documented divergence is interface 1 (ours: vendor-only 0xFF00 with INT IN+OUT
64 B; real: mouse+consumer+system+vendor(41 B feature) with 1× INT IN).

**What it will NOT do:** change the vendor data channel, the wire protocol, the
web pages, BLE identity, or any non-HP profile behavior.

**Effort:** 1 firmware file (`furi_hal_usb_airbridge.c`) + reflash + enumeration
verification + bundle regen. **Risk:** low; descriptor-only changes, no logic.

## Real dongle reference (captured 2026-07-31 from IOService registry)

Device: VID `0x03F0`, PID `0x5341`, bcdDevice `0x0126` (1.26), bcdUSB `0x0200`,
class/sub/proto `0/0/0`, bMaxPacketSize0 64, iSerialNumber 0 (none),
manufacturer `"PIXART"`, product `"HP Wireless Keyboard and Mouse"`, 1 config,
**no IAD**, current required 100 mA.

Interface 0: class 3, subclass 1 (boot), protocol 1 (keyboard), 1 endpoint
(INT IN). Report descriptor (65 B, standard boot keyboard):

```
05010906a101050719e029e715002501750195088102950175088101
95057501050819012905910295017503910195067508150026ff00050719002aff008100c0
```

Interface 1 (real, for reference only — NOT imitated in Option C): class 3,
subclass 1, protocol 2 (mouse), 1 endpoint INT IN; collections: mouse (ID 1,
8 buttons, X/Y 12-bit, wheel), vendor `0xFF00` usage 1 (ID 3, 41-byte FEATURE),
consumer (ID 4), system control (ID 5).

## Current-vs-real diff (the four fixes)

| # | Item | Real | Ours today |
|---|------|------|-----------|
| 1 | Manufacturer string | `PIXART` | `HP` |
| 2 | bmAttributes / bMaxPower | bus-powered, 100 mA | `0xC0` self-powered, 500 mA |
| 3 | IAD in composite config | none | present (HID/boot/kbd) |
| 4 | Keyboard report desc key array | `26 ff00` + `2a ff00` (16-bit, max 255) | `25 65` + `29 65` (max 101) |

Device descriptor already matches (class 0/0/0, bcdDevice 1.26, MPS0 64, no
serial). bInterval: ours 5 ms, real ≈1 ms — **kept at 5 ms** (minimal-change
scope; noted, not aligned).

## Scope

**IN:**
- `/Users/asutov/projects/flipperzero-firmware/targets/f7/furi_hal/furi_hal_usb_airbridge.c`
  1. `hp_manuf_desc`: `"HP"` → `"PIXART"`.
  2. New `HidCompositeNoIadConfigDescriptor` struct + static instance
     `hid_composite_noiad_cfg_desc`: identical to `hid_composite_cfg_desc`
     except (a) no IAD member, (b) `bmAttributes = USB_CFG_ATTR_RESERVED`
     (bus-powered, drop `USB_CFG_ATTR_SELFPOWERED`), (c)
     `bMaxPower = USB_CFG_POWER_MA(100)`. `wTotalLength` auto-shrinks via
     `sizeof`.
  3. `usb_airbridge_hp.cfg_descr` → `&hid_composite_noiad_cfg_desc`. All other
     profiles keep the shared IAD-bearing config descriptor (no real hardware
     to match; no gratuitous risk). NOTE: the *config* descriptor change is
     HP-only; the *keyboard report* descriptor change (fix 4) is shared across
     all keyboard profiles — see its scope note.
  4. `hid_keyboard_report_desc`: change the 6-key array tail to byte-match the
     real boot keyboard: `HID_LOGICAL_MAXIMUM(101)` → `HID_RI_LOGICAL_MAXIMUM(16, 0xFF)`
     and `HID_USAGE_MAXIMUM(101)` → `HID_RI_USAGE_MAXIMUM(16, 0xFF)` (both macros
     exist in `lib/libusb_stm32/inc/usb_hid.h`, little-endian 16-bit encoding —
     produces the real dongle's `26 ff 00` / `2a ff 00` tokens).
     **This descriptor is SHARED by all keyboard-composite profiles
     (logitech/dell/msft_kbd/hp), so all of them get the standard boot
     descriptor** — verified runtime-safe: the send path writes `button & 0xFF`
     with no descriptor-max validation (`furi_hal_usb_airbridge.c:592-594`),
     and deploy typing only emits usages ≤ 101 (`furi_hal_usb_hid.h` ASCII map).
     `HidKeyboardReport` is unchanged.
- Reflash + enumeration verification on the Flipper.
- Regeneration of `flipper-hid/firmware/` bundle (the HAL file is inside
  `airbridge-firmware.patch`).
- `firmware/README.md` regeneration note (real-dongle alignment).

**OUT (Must-NOT-Have):**
- No change to the vendor interface structure, endpoints, or 64-byte reports.
- No change to interface 1 content beyond what falls out of the shared
  descriptors (our vendor-only interface 1 stays — the accepted divergence).
- No changes to web/, BLE, protocol, other profiles' descriptors.
- No bInterval change (5 ms kept).

## Execution strategy

Single worker, sequential: firmware edit → build+flash → enumeration verify →
bundle regen. Enumeration verification uses the captured real-dongle data above
as ground truth.

| Todo | Depends on |
|------|-----------|
| 1 | — |
| 2 | 1 |
| 3 | 2 |

## Todos

- [x] 1. Firmware: HP profile descriptor exactness fixes
  - **What:** the four edits in Scope, all in `furi_hal_usb_airbridge.c`.
    Exact details:
    - Fix 1: `static const struct usb_string_descriptor hp_manuf_desc =
      USB_STRING_DESC("PIXART");`
    - Fix 2: new struct + instance. bmAttributes bus-powered (0x80), bMaxPower
      100 mA, no IAD. Byte layout must be: config, keyboard iface, kbd HID desc,
      kbd EP IN, vendor iface, vendor HID desc, vendor EP IN, vendor EP OUT.
    - Fix 3: `usb_airbridge_hp.cfg_descr = (void*)&hid_composite_noiad_cfg_desc;`
    - Fix 4: keyboard report desc tail `… 15 00 26 ff 00 05 07 19 00 2a ff 00
      81 00 c0`. After the edit, the full `hid_keyboard_report_desc` byte stream
      must equal the 65-byte real-dongle hex above.
  - **Acceptance:** full `./fbt` build warning-clean; byte-level check that the
    emitted `hid_keyboard_report_desc` equals the real 65-byte hex above;
    `usb_airbridge_hp` points at the no-IAD descriptor; other profiles' config
    descriptors unchanged (kbd report desc widening is shared by design).
  - **QA:** build log `/tmp/fw-build-hp-exact.log`; descriptor byte check
    script output saved (e.g., extract the array via a small C or Python
    comparison against the hex string).
  - **Commit:** `fix(usb): align HP impersonation descriptors with real dongle`

- [x] 2. Flash + enumeration verification
  - **What:** build, `./fbt flash_usb`, rebuild+redeploy FAP (app must match
    firmware), launch app, then enumerate on macOS and diff against the real
    dongle capture.
  - **Acceptance (all verified via `ioreg -p IOService`/`system_profiler`
    against the app-running device):**
    - `USB Vendor Name` = `PIXART`, `USB Product Name` = `HP Wireless Keyboard
      and Mouse`, VID `0x03F0`, PID `0x5341`, bcdDevice 294.
    - No `IOUSBInterfaceAssociation` node under the device (IAD gone).
    - `Current Required` = 100 mA (system_profiler).
    - Interface 0 `ReportDescriptor` hex equals the real dongle's 65-byte
      string exactly.
    - Additional enumeration items to record as accepted divergences (the real
      values were NOT captured; ours are `bcdHID` 1.00, `bCountryCode` 0,
      `bInterval` 5, iface 1 subclass/protocol 0/0): note them in the evidence.
  - **Regression (per AGENTS.md regression rule — executable scenarios):**
    - WebHID chat: serve `web/` on localhost:8081, Playwright window to
      `chat-usb.html`, Connect (user handles the picker gate), send a text
      message, confirm round-trip and counter increment.
    - USB Deploy flow end-to-end: Playwright window on `https://blank.org`,
      DevTools console focused, carousel RIGHT to USB Deploy prompt, OK to
      type bootstrap (user gates: cursor placement + picker), Connect via the
      bootstrap button, full app streams, chat works in the streamed app.
  - **Windows note (deferred, matches README convention):** usbccgp tree
    comparison on Windows cannot run from this Mac. The risk direction is
    favorable — the real dongle ships without an IAD and binds fine on Windows,
    so removing ours makes us strictly more similar. When a Windows machine is
    available: `Get-PnpDevice` capture + USBView tree-compare of both devices,
    confirm two HID interfaces bind and the vendor collection opens.
  - **QA:** ioreg capture saved to `/tmp/hp-exact-ioreg.txt`; chat + deploy
    regression notes in `/tmp/hp-exact-regression.log`. Physical gates per
    AGENTS.md (question tool + attention signal).
  - **Commit:** none (verification step).

- [x] 3. Regenerate bundle + README note
  - **What:** regenerate `flipper-hid/firmware/` (patches + FAP copy + README
    header). `airbridge-firmware.patch` will now include the
    `furi_hal_usb_airbridge.c` descriptor changes. Add one line to the README
    regeneration note: HP descriptors aligned with the real dongle capture
    (PIXART, 100 mA bus-powered, no IAD, boot-kbd report descriptor).
  - **Acceptance:** FAP copy byte-identical to canonical; both patches
    dry-run clean against `c9ab2b68`; README states current HEAD.
  - **QA (exact commands, all must exit 0):**
    1. `cp -R applications_user/pocket_airbridge/ → flipper-hid/firmware/pocket_airbridge/`,
       then `diff -r` bundle copy vs canonical → empty output.
    2. Regenerate patches: `git diff c9ab2b68 -- . ':(exclude)targets/f7/api_symbols.csv' ':(exclude)applications_user/pocket_airbridge' > airbridge-firmware.patch`
       and `git diff c9ab2b68 -- targets/f7/api_symbols.csv > api-symbols-additions.patch`.
    3. In a fresh `git archive c9ab2b68` export at `/tmp/airbridge-bundle-check-hp-exact/`:
       `patch --dry-run -p1 < airbridge-firmware.patch` → EXIT 0;
       `patch --dry-run -p1 < api-symbols-additions.patch` → EXIT 0.
    4. `./fbt build APPSRC=applications_user/pocket_airbridge` → exit 0, 0 warnings.
    Full transcript saved to `/tmp/bundle-check-hp-exact.log`.
  - **Commit:** `chore(firmware): regenerate bundle with HP descriptor alignment`

## Final verification wave

- [x] F1. Plan compliance — diffs match the four fixes; no unplanned changes.
- [x] F2. Code quality — descriptor struct layout correct, packed, byte-level
  keyboard report descriptor match confirmed, no shared-descriptor regressions
  for other profiles.
- [x] F3. Hardware enumeration — real-dongle diff items all match; WebHID chat
  regression passes.
- [x] F4. Scope fidelity — only `furi_hal_usb_airbridge.c` + bundle files
  touched; vendor channel untouched.

## Commit strategy

1. `flipperzero-firmware`: todo 1 commit.
2. `flipper-hid`: todo 3 bundle commit.
Todo 2 is verification, no commit.

## Success criteria

1. Enumerated HP profile is field-identical to the real dongle for: VID, PID,
   bcdDevice, manufacturer, product, serial (none), power (100 mA
   bus-powered), interface count (2), no IAD, interface 0 report descriptor
   bytes.
2. The documented Option C divergence stands alone: interface 1 vendor-only
   64-byte interrupt IN/OUT.
3. WebHID chat + deploy regression passes on the flashed device.
4. Bundle regenerated, patches apply clean to `c9ab2b68`.

## Todo 1 — HP descriptor fixes (2026-08-06 20:54 CEST)

Applied the four HP-descriptor exactness fixes to
`flipperzero-firmware/targets/f7/furi_hal/furi_hal_usb_airbridge.c` (only file
touched, not committed):

1. `hp_manuf_desc`: `"HP"` → `"PIXART"`.
2. New `struct HidCompositeNoIadConfigDescriptor` + static instance
   `hid_composite_noiad_cfg_desc` placed adjacent to the existing composite
   descriptor. Identical member content minus the IAD; `bmAttributes =
   USB_CFG_ATTR_RESERVED` (0x80, bus-powered), `bMaxPower =
   USB_CFG_POWER_MA(100)`, `wTotalLength = sizeof(struct)` (shrinks to 66 B
   automatically: 9 cfg + 9 iface + 9 HID + 7 EP + 9 iface + 9 HID + 7 EP +
   7 EP).
3. `usb_airbridge_hp.cfg_descr` → `&hid_composite_noiad_cfg_desc`. The other
   four profiles untouched: logitech/dell/msft_kbd still reference
   `hid_composite_cfg_desc` (IAD included), msft_vendor still references
   `hid_vendor_cfg_desc` (grep-verified).
4. `hid_keyboard_report_desc` now byte-matches the real dongle's 65-byte boot
   keyboard descriptor EXACTLY (verified by a parser that expands the macros
   straight from the C source — no hardcoded byte stream):
   `05010906a101050719e029e71500250175019508810295017508810195057501050819012905910295017503910195067508150026ff00050719002aff008100c0`.

### Key learning: the plan's fix 4 alone was NOT byte-exact

The plan specified only the key-array tail change (`HID_LOGICAL_MAXIMUM(101)`
→ `HID_RI_LOGICAL_MAXIMUM(16, 0xFF)`, `HID_USAGE_MAXIMUM(101)` →
`HID_RI_USAGE_MAXIMUM(16, 0xFF)`). Byte-expanding the CURRENT descriptor
against the real 65-byte hex showed three further in-array divergences that
had to be fixed for the stated exact-match acceptance (all inside the same
array, shared by all keyboard profiles as designed):

- Reserved-byte padding was `HID_IOF_CONSTANT | HID_IOF_VARIABLE` (0x81 0x03);
  the real dongle (and the HID spec boot descriptor) uses
  `CONSTANT | ARRAY` (0x81 0x01). Same for the 3-bit LED padding
  (0x91 0x03 → 0x91 0x01).
- The current array put `HID_USAGE_PAGE(HID_PAGE_LED)` BEFORE
  `REPORT_COUNT(5)/REPORT_SIZE(1)`; the real dongle puts it AFTER
  (`95 05 75 01 05 08 …`). Reordered to match. Semantics unchanged: the usage
  page is a global item that still precedes the LED usage min/max and the
  `91 02` OUTPUT it qualifies.

Takeaway for future descriptor-cloning work: never trust a "fix list" to be
complete — always expand the CURRENT macro array to bytes and diff against
the real capture first. The two extra fixes were invisible until the full
byte stream was compared.

### Verification evidence

- Full `./fbt` build: exit 0, zero warnings/errors
  (`/tmp/fw-build-hp-exact.log`).
- Descriptor byte check: PASS, 65/65 bytes equal
  (`/tmp/kbd-desc-bytecheck.log`; checker script parses macro calls from the
  source file).
- `git status`: exactly one modified file
  (`targets/f7/furi_hal/furi_hal_usb_airbridge.c`, +104/-7).
- `hid_vendor_control` GET_DESCRIPTOR(HID) path still serves
  `hid_composite_cfg_desc.keyboard.hid_desc` / `.vendor_hid_desc` for the HP
  profile — safe because those sub-descriptors are byte-identical between the
  IAD and no-IAD structs (wDescriptorLength0 unchanged; IAD removal is
  descriptor-content only).

## Todo 2 — flash + enumeration verification (2026-08-06, closed on user e2e)

- Firmware flashed via `./fbt flash_usb` (one retry after Passport app blocked
  the first trigger; user closed Passport, second run flashed clean).
- FAP rebuilt against the new firmware, redeployed to
  `/ext/apps/USB/pocket_airbridge.fap`, launched; screen + green LED confirmed.
- On-device enumeration evidence (system_profiler while Flipper plugged):
  `USB Vendor Name` = **PIXART**, product `HP Wireless Keyboard and Mouse`,
  VID `0x03F0`, PID `0x5341`, Version 1.26 (bcdDevice 294),
  `Current Required` = **100 mA**. Fixes 1+2 confirmed live.
- IAD absence and interface-0 ReportDescriptor byte-match are build-verified
  (the served config/report descriptors are the compiled structs; byte check
  65/65 at `/tmp/kbd-desc-bytecheck.log`). Flipper was unplugged before the
  scripted ioreg capture could run; accepted as structurally implied.
- Incident: app exited once mid-session (clean exit, no crash notice) — an
  accidental long-BACK (new carousel exit semantics). Relaunched, confirmed.
- Regression: user ran USB e2e ("all ok") — WebHID chat + deploy path.
- Fresh real-dongle re-read (2026-08-06): identical to 07-31 capture; both
  report descriptors byte-identical. NVRAM BluetoothInfo confirms paired set
  name "HP 725 K+M" + OUI 3C:52:82 (BLE identity defaults validated).
- Deferred per plan: Windows usbccgp tree-compare; bInterval 5ms (real ~1ms)
  and bcdHID 1.00 vs 1.11 noted as Tier-1 follow-up candidates, OUT of this
  plan's scope.

## Todo 3 — firmware bundle regen (2026-08-28 13:51 CEST)

Regenerated `flipper-hid/firmware/` from the firmware tree (HEAD `113d1701`,
carousel-navigation commit) with the uncommitted HP-descriptor exactness
change in `targets/f7/furi_hal/furi_hal_usb_airbridge.c` now folded into
`airbridge-firmware.patch`:

- FAP copy re-synced (`applications_user/pocket_airbridge/` →
  `firmware/pocket_airbridge/`); `diff -r` clean. FAP source unchanged —
  rebuilt artifact still 22952 B, as expected.
- `airbridge-firmware.patch` regenerated from `git diff c9ab2b68` excluding
  `api_symbols.csv` and the FAP path: 124,704 B (was 120,701 B; the +4 KB is
  the +104/-7 airbridge.c hunk). Contains `PIXART` (1) and
  `HidCompositeNoIadConfigDescriptor` (3); zero diff headers for
  `api_symbols.csv` or `applications_user/pocket_airbridge`.
- `api-symbols-additions.patch` regenerated byte-identical to the committed
  version (`cmp` pass — `api_symbols.csv` untouched since last regen).
- Both patches `patch --dry-run -p1` clean (exit 0) against a fresh
  `git archive c9ab2b68` export at `/tmp/airbridge-bundle-check-hp-exact/`.
- `./fbt build APPSRC=applications_user/pocket_airbridge`: exit 0, 0 warning
  lines, artifact 22952 B.
- `firmware/README.md` header updated: HEAD `113d1701`, regen date
  2026-08-28, plus one line noting the HP descriptors are aligned with the
  real dongle capture (PIXART, 100 mA bus-powered, no IAD, byte-exact 65 B
  boot-kbd report descriptor).
- Full transcript: `/tmp/bundle-check-hp-exact.log`.

## F2 — Code quality (2026-08-28)

Reviewed the final uncommitted diff of
`flipperzero-firmware/targets/f7/furi_hal/furi_hal_usb_airbridge.c`
(+104/−7, `git diff --numstat`; only file modified per `git status`). All
line numbers below refer to the current working-tree file unless another
file is named.

### 1. `HidCompositeNoIadConfigDescriptor` struct — PASS

- Struct at `furi_hal_usb_airbridge.c:52-59`, declared `FURI_PACKED`
  (`furi/core/common_defines.h:29` → `__attribute__((packed))`); every member
  struct is itself packed (`lib/libusb_stm32/inc/usb_std.h:368,388,403,418`;
  `lib/libusb_stm32/inc/usb_hid.h:145`).
- Member order/types: `config` (usb_config_descriptor, 9 B) → `keyboard`
  (HidKeyboardDescriptor = iface 9 + HID 9 + EP 7) → `vendor` iface (9) →
  `vendor_hid_desc` (9) → `vendor_ep_in` (7) → `vendor_ep_out` (7). Matches the
  plan's required byte layout exactly.
- `wTotalLength = sizeof(struct HidCompositeNoIadConfigDescriptor)` = 66
  (9+9+9+7+9+9+7+7) at `:335`; `bNumInterfaces = 2` at `:336`;
  `bmAttributes = USB_CFG_ATTR_RESERVED` = 0x80 (bus-powered) at `:339`
  (`usb_std.h:57`); `bMaxPower = USB_CFG_POWER_MA(100)` = 100>>1 = 50 units =
  100 mA at `:340` (`usb_std.h:52`).

### 2. Instance divergence from `hid_composite_cfg_desc` — PASS

Line-by-line comparison of `hid_composite_cfg_desc` (`:231-328`) vs
`hid_composite_noiad_cfg_desc` (`:330-416`): keyboard block (`:254-287` ≡
`:342-375`), vendor iface (`:288-299` ≡ `:376-387`), `vendor_hid_desc`
(`:300-309` ≡ `:388-397`), and both vendor endpoints (`:310-327` ≡ `:398-415`)
are field-identical. The only differences are the four intended ones: no
`.hid_iad` member (`:243-253` absent from the new struct), `wTotalLength`
sizeof (:236 vs :335), `bmAttributes` 0xC0 → 0x80 (:240 vs :339), `bMaxPower`
500 → 100 mA (:241 vs :340). No other drift.

### 3. Profile repoint — PASS

`usb_airbridge_hp.cfg_descr = (void*)&hid_composite_noiad_cfg_desc` at `:483`.
Other four profiles untouched: logitech `:435`, dell `:447`, msft_kbd `:459`
still reference `hid_composite_cfg_desc`; msft_vendor `:471` still references
`hid_vendor_cfg_desc`. Only 4 occurrences of the noiad names in the file
(struct `:52`, instance `:330`, sizeof `:335`, pointer `:483`) — no dangling
references.

### 4. Keyboard report descriptor byte-exactness — PASS

`/tmp/kbd-desc-bytecheck.log` no longer exists (tmp cleaned), so re-verified
independently: a parser expanded every macro call in
`hid_keyboard_report_desc` (`:86-119`) using header-verified semantics
(`HID_RI_*` 16-bit little-endian encoding per `usb_hid.h:197-203,250,279`;
constants `HID_KB_MAX_KEYS=6` `targets/furi_hal_include/furi_hal_usb_hid.h:13`,
page/usage values `hid_usage_desktop.h:30,38,39`, `hid_usage_keyboard.h:234,241`,
`hid_usage_led.h:26`). Result: 65/65 bytes equal to the real dongle capture
`05010906a101050719e029e71500250175019508810295017508810195057501050819012905910295017503910195067508150026ff00050719002aff008100c0`.

The three in-array corrections are semantics-preserving:

- `:100` `81 01` (was `81 03`) and `:109` `91 01` (was `91 03`): CONSTANT items
  are padding, never interpreted as controls; ARRAY-vs-VARIABLE on a constant
  item cannot change report parsing.
- `:101-103` LED `USAGE_PAGE` moved after `REPORT_COUNT(5)/REPORT_SIZE(1)`:
  usage page is a global item and still precedes the LED usage min/max
  (`:104-105`) and the `91 02` OUTPUT (`:106`) it qualifies; the only state
  the padding item is parsed under is irrelevant.

Shared-descriptor regression check (logitech/dell/msft_kbd also use this
array, plan-sanctioned): runtime send path writes `button & 0xFF` with no
descriptor-max validation (`:690`, release at `:706-707`); `HidKeyboardReport`
unchanged (`:61-65`, still 8-byte boot report); widened max 0xFF is a superset
of every usage the deploy path emits (≤ 101). No regression surface.

### 5. `hid_vendor_control` GET_DESCRIPTOR(HID) path — PASS

For composite profiles (HP included, `hid_keyboard_available` set at `:609`)
the handler serves `&hid_composite_cfg_desc.keyboard.hid_desc` (`:808-809`)
and `&hid_composite_cfg_desc.vendor_hid_desc` (`:813,816`). Both
sub-descriptors are byte-identical to the noiad instance's members (verified
in item 2: same bcdHID `VERSION_BCD(1,0,0)`, country 0, numDescriptors 1,
type0 REPORT, and `wDescriptorLength0 = sizeof` of the same shared
`hid_keyboard_report_desc` / `hid_vendor_report_desc` arrays). IAD removal is
config-descriptor-content only, so the standard-request path needs no HP
special-casing; GET_DESCRIPTOR(HID_REPORT) serves the shared arrays directly
(`:824-829`).

### 6. Style / comment accuracy / scope — PASS

New code mirrors the adjacent `HidCompositeConfigDescriptor` block style
(designated initializers, same indentation, comment-free descriptor
initializers consistent with surrounding code); naming
(`HidCompositeNoIadConfigDescriptor` / `hid_composite_noiad_cfg_desc`) states
intent without needing comments. No logic changes: all +104/−7 lines are
descriptor content (new struct+instance, one pointer, one string, report-array
constants); control flow, endpoints, and callbacks untouched. Full `./fbt`
build clean — no warnings, link + DFU dist succeed
(`/tmp/fw-build-hp-exact.log`, 2026-08-28 13:59).

VERDICT: APPROVE

## F3 — Enumeration evidence audit (2026-08-28)

Audit of the recorded hardware-enumeration evidence against the plan's F3
intent (no live re-run; Flipper unplugged; user already ran USB e2e, "all ok").

### Evidence-log availability

- `/tmp/fw-build-hp-exact.log`, `/tmp/kbd-desc-bytecheck.log`,
  `/tmp/hp-exact-ioreg.txt`, `/tmp/hp-exact-regression.log` no longer exist —
  macOS purges `/tmp` files older than ~3 days (logs were from 2026-08-06).
  Their *results* are preserved in the todo-1/todo-2 entries above, and both
  key claims were independently re-verified today from read-only sources
  (below). `/tmp/bundle-check-hp-exact.log` (2026-08-28) survives and shows
  FAP build exit 0 / 0 warning lines against the same tree state.

### Independent re-verification (replaces the expired logs)

- **Report descriptor 65/65 MATCH**: re-expanded `hid_keyboard_report_desc`
  from the current source macros using the exact `_HID_RI_ENTRY` semantics in
  `lib/libusb_stm32/inc/usb_hid.h` (16-bit items LE: `26 ff 00` / `2a ff 00`,
  `HID_KB_MAX_KEYS`=6, all usage-page/usage constants grep-verified). Result:
  65 bytes, byte-identical to the real-dongle hex in the plan. The "three
  extra in-array divergences" fixes (0x81 0x01, 0x91 0x01, LED usage-page
  ordering) are confirmed present in the byte stream.
- **No-IAD struct**: `HidCompositeNoIadConfigDescriptor` has no IAD member;
  `bmAttributes = USB_CFG_ATTR_RESERVED` (0x80, bus-powered), `bMaxPower =
  USB_CFG_POWER_MA(100)` (0x32, ×2 mA units); `wTotalLength = sizeof` = 66 B
  (vs 74 B for the IAD variant). `usb_airbridge_hp.cfg_descr` → the no-IAD
  instance (line 483); logitech/dell/msft_kbd still on `hid_composite_cfg_desc`,
  msft_vendor on `hid_vendor_cfg_desc` — other profiles untouched.
- **Device descriptor**: `hp_device_desc` = VID 0x03F0, PID 0x5341, class
  0/0/0, bcdDevice 0x0126 (294), strings PIXART / "HP Wireless Keyboard and
  Mouse", serial NULL — matches the real capture field-for-field.

### Structural argument assessment (served == compiled) — SOUND

Traced the full serving path: `usb_descriptor_get` in `furi_hal_usb.c:261`
serves `USB_DTYPE_CONFIGURATION` verbatim from `usb.interface->cfg_descr`
with length = the struct's `wTotalLength` field. `hid_vendor_control`
(`furi_hal_usb_airbridge.c:822-833`) serves `USB_DTYPE_HID_REPORT` verbatim
from `hid_keyboard_report_desc` / `hid_vendor_report_desc` arrays. Therefore
the wire bytes ARE the compiled structs' bytes; IAD absence is physical (no
IAD member in the served 66-byte stream) and report-desc byte-exactness
follows from the 65/65 source match. The one indirection — `USB_DTYPE_HID`
class descriptor served from the *IAD* struct instance
(`hid_composite_cfg_desc.keyboard.hid_desc` / `.vendor_hid_desc`) even for HP
— was verified member-by-member byte-identical to the no-IAD struct's
corresponding members (bcdHID 1.00, country 0, same wDescriptorLength0), so
served bytes are identical either way.

Crucially, the live system_profiler evidence does more than fixes 1+2: **100
mA on the wire proves the no-IAD config struct is the one being served** (the
old IAD struct carried 500 mA), so IAD-absence is live-verified transitively,
not merely build-verified. Combined with PIXART/VID/PID/1.26 from the same
capture, the flashed binary is proven to contain this exact change set.

### Remaining audit items

- **Scripted ioreg capture never ran** (Flipper unplugged first): the
  `IOUSBInterfaceAssociation`-absence node check and live interface-0
  ReportDescriptor hex were not directly observed. Covered by the structural
  argument above plus the user's live USB e2e on the new firmware (macOS
  enumerated the tree, claimed the keyboard collection, WebHID opened the
  vendor 0xFF00 collection — functional enumeration proof).
- **Clean long-BACK exit incident**: NOT a defect — long-BACK-exits-app is
  the documented, committed carousel semantics (HEAD `113d1701`, README
  controls section). Accidental press, relaunched, confirmed.
- **Real-dongle ground truth**: 2026-08-06 re-read recorded identical to the
  07-31 capture (both report descriptors byte-identical; NVRAM "HP 725 K+M" +
  OUI 3C:52:82). Ground truth stable across the work window.
- **Deferred items documented**: Windows usbccgp tree-compare (cannot run from
  this Mac; favorable risk direction), bInterval 5 ms vs real ~1 ms, bcdHID
  1.00 — all recorded as OUT-of-scope / Tier-1 follow-ups in plan + notepad.
- **Heads-up for F1/F4 (not F3 scope)**: both planned commits are still
  pending — firmware repo holds the +104/-7 `furi_hal_usb_airbridge.c` change
  uncommitted; flipper-hid holds `firmware/airbridge-firmware.patch` +
  `firmware/README.md` uncommitted.

VERDICT: APPROVE — F3's intent (enumeration matches the real dongle at
descriptor level + WebHID chat regression) is satisfied by the recorded
evidence. The expired `/tmp` logs are fully compensated: the byte-check was
re-derived 65/65 from source, the serving path was traced end-to-end proving
served==compiled, live system_profiler captured PIXART + 100 mA (which
transitively proves the no-IAD config on the wire), the user passed USB e2e
on the flashed firmware, ground truth was re-confirmed stable, and the
long-BACK exit is intended behavior. The never-captured scripted ioreg diff
is the only soft spot and is structurally implied with a verified chain.

## F1 — Plan compliance (2026-08-28)

Audited every todo's acceptance criteria against live evidence; checked both
repos for unplanned changes.

### Repo diff scope

- **firmware repo** (`flipperzero-firmware`): `git status --porcelain` shows
  exactly one modified file, `targets/f7/furi_hal/furi_hal_usb_airbridge.c`
  (+104/−7). The full diff contains precisely the four planned fixes:
  1. `hp_manuf_desc` `"HP"` → `"PIXART"`;
  2. new `FURI_PACKED` `struct HidCompositeNoIadConfigDescriptor` + instance
     `hid_composite_noiad_cfg_desc` with `bmAttributes = USB_CFG_ATTR_RESERVED`
     (0x80, bus-powered), `bMaxPower = USB_CFG_POWER_MA(100)`, no IAD member,
     member order config → kbd iface → kbd HID → kbd EP IN → vendor iface →
     vendor HID → vendor EP IN → vendor EP OUT (matches the plan's required
     byte layout);
  3. `usb_airbridge_hp.cfg_descr` → `&hid_composite_noiad_cfg_desc`; no other
     profile line touched;
  4. `hid_keyboard_report_desc`: the two planned changes
     (`HID_RI_LOGICAL_MAXIMUM(16, 0xFF)`, `HID_RI_USAGE_MAXIMUM(16, 0xFF)`)
     plus the three documented in-array corrections (0x81 0x03→0x81 0x01
     reserved padding, LED usage-page reorder after REPORT_COUNT(5)/
     REPORT_SIZE(1), 0x91 0x03→0x91 0x01 LED padding) — all three recorded in
     the todo-1 notepad entry.
- **flipper-hid repo**: working tree modified files are exactly
  `firmware/airbridge-firmware.patch` and `firmware/README.md` — the planned
  todo-3 surface. Older docs/README/web changes live in committed history
  (HEAD `6c944cf` and earlier) and predate this plan; nothing unplanned in
  the working tree.

### Todo 1 — firmware descriptor fixes: PASS

- `/tmp/fw-build-hp-exact.log` (original 08-06 log lost to macOS /tmp
  cleanup; re-established 08-28 by full `./fbt` rerun on the current tree):
  exit 0, 0 warning lines, 0 error lines.
- `/tmp/kbd-desc-bytecheck.log` (regenerated 08-28, constants cross-checked
  against `libusb_stm32` headers): 65/65 MATCH, expanded stream byte-equal to
  the real-dongle hex
  `05010906a101…2aff008100c0`.
- `usb_airbridge_hp` points at the no-IAD descriptor; other profiles'
  `cfg_descr` lines untouched in the diff.

### Todo 2 — flash + enumeration: PASS (as scoped by task)

Notepad todo-2 entry present and complete per the audit checklist:
on-device PIXART + 100 mA via system_profiler; user USB e2e pass ("all ok");
IAD absence + report-desc byte-match build-verified (accepted as structurally
implied — Flipper unplugged before scripted ioreg capture; noted in entry);
Windows usbccgp tree-compare deferred per plan. `/tmp/hp-exact-ioreg.txt` and
`/tmp/hp-exact-regression.log` are gone (same /tmp cleanup); the notepad
entry is the surviving record, which the checklist accepts.

### Todo 3 — bundle regen: PASS

- `/tmp/bundle-check-hp-exact.log` intact: every step exit 0 (diff -r clean,
  both dry-runs exit 0, FAP build exit 0 with 0 warnings).
- Independently re-verified 08-28: fresh `git archive c9ab2b68` export →
  `patch --dry-run -p1` exit 0 for both `airbridge-firmware.patch` and
  `api-symbols-additions.patch`; `diff -r` bundle FAP copy vs
  `applications_user/pocket_airbridge` → byte-identical.
- Patch content: `PIXART` ×1, `HidCompositeNoIadConfigDescriptor` ×3, zero
  diff headers for `api_symbols.csv` or `applications_user/pocket_airbridge`;
  the only `furi_hal_usb_airbridge` headers are the planned `.c` change plus
  the pre-existing `.h` file.
- `firmware/README.md` diff = regen header update (HEAD `113d1701`, date
  2026-08-28) + the one planned HP-alignment line. Nothing else.

### Flags (documented divergences, non-blocking)

1. **Commit strategy not executed.** The plan called for
   `fix(usb): align HP impersonation descriptors with real dongle` in the
   firmware repo and `chore(firmware): regenerate bundle with HP descriptor
   alignment` in flipper-hid. Neither commit exists: the firmware change sits
   uncommitted in the working tree (HEAD still `113d1701`) and the flipper-hid
   bundle files are uncommitted (HEAD still `6c944cf`). Both notepad entries
   and the README header document this deliberately ("not committed" / "the
   only uncommitted change folded into this regen"). The diffs themselves are
   exactly as planned; only the commits are pending.
2. Todo-2 ioreg/regression `/tmp` artifacts were lost to OS cleanup; the
   notepad record stands as evidence and the IAD/report-desc items were
   verified at build level instead of on-device ioreg — a relaxation of the
   plan's literal acceptance text, documented by the worker and accepted by
   this audit's checklist.

### Unplanned changes

None attributable to this plan in either repo.

### Accepted divergences confirmed untouched

Interface 1 remains vendor-only 0xFF00 with 64 B INT IN/OUT (vendor struct
members unchanged in the diff); `bInterval` stays `HID_INTERVAL` (5 ms) on
all endpoints.

VERDICT: APPROVE — all three todos map to verified evidence; the only gaps
are the two pending commits from the plan's commit strategy (documented, and
the working-tree diffs match the plan exactly) and /tmp log loss that was
re-established by re-running the build and byte check on 2026-08-28.

## F4 — Scope fidelity (2026-08-28)

Audit of both repos against the plan's Must-NOT-Have list. All checks ran on
the live working trees (no files modified by this wave).

### Firmware repo (`~/projects/flipperzero-firmware`)

- `git status --short`: exactly one entry —
  ` M targets/f7/furi_hal/furi_hal_usb_airbridge.c`. Nothing staged, no
  untracked files.
- `git diff --name-only`: `targets/f7/furi_hal/furi_hal_usb_airbridge.c` only
  (+104/-7). Recent commits on the branch (`113d1701` carousel FAP, `54a8c83d`
  bt refcount, `371af961` api version, …) predate this plan — earlier committed
  work, not a violation.
- Diff content maps 1:1 onto the plan's four fixes: `hp_manuf_desc`
  `"HP"`→`"PIXART"`; new `HidCompositeNoIadConfigDescriptor` +
  `hid_composite_noiad_cfg_desc` (no IAD member, `bmAttributes =
  USB_CFG_ATTR_RESERVED`, `bMaxPower = USB_CFG_POWER_MA(100)`, `wTotalLength =
  sizeof`); only `usb_airbridge_hp.cfg_descr` repointed to the no-IAD struct;
  `hid_keyboard_report_desc` tail widened to `HID_RI_LOGICAL_MAXIMUM(16,0xFF)` /
  `HID_RI_USAGE_MAXIMUM(16,0xFF)` plus the three in-array byte-alignment tweaks
  (CONSTANT|VARIABLE→CONSTANT|ARRAY ×2, LED usage-page reorder) required by
  Todo 1's byte-exact acceptance (documented under Todo 1 learnings above).

### Vendor channel untouched (diff-inspected)

- `hid_vendor_report_desc`: zero `-`/`+` lines in the diff; only unchanged
  `sizeof(hid_vendor_report_desc)` references in both structs.
- Vendor interface structure: the new struct's `.vendor` (iface 1, 2 endpoints,
  class HID, subclass/protocol NONBOOT/NONBOOT), `.vendor_hid_desc`,
  `.vendor_ep_in`, `.vendor_ep_out` sections are field-for-field identical to
  the untouched original `hid_composite_cfg_desc` (side-by-side source read).
- Endpoints: `HID_VENDOR_EP_IN 0x82` / `HID_VENDOR_EP_OUT 0x03` defines
  untouched (not in diff).
- 64-byte reports: `HID_VENDOR_PACKET_LEN 64` in
  `targets/furi_hal_include/furi_hal_usb_airbridge.h:9` — header not in the
  diff.
- bInterval 5: `#define HID_INTERVAL 5` (line 21) untouched; the new struct
  reuses the same macro for all three interrupt endpoints.

### flipper-hid repo

- `git status --short`: exactly two entries — ` M firmware/README.md`,
  ` M firmware/airbridge-firmware.patch`. No untracked files.
- FAP copy (`firmware/pocket_airbridge/`) and
  `firmware/api-symbols-additions.patch`: byte-identical (no diff).
- `web/`, `docs/` untouched (not in status).
- `firmware/airbridge-firmware.patch` header audit: 32 `diff --git` headers
  (bt_service, gui, input, ble_profile, ble_glue, furi_hal, target.json,
  furi_hal_include — all pre-existing cumulative content from base `c9ab2b68`);
  `grep -c 'api_symbols.csv\|applications_user/pocket_airbridge'` on the patch
  → **0 matches** (no diff headers for either path).
- The flipper-hid diff OF the patch shows the only changed section is the
  pre-existing `targets/f7/furi_hal/furi_hal_usb_airbridge.c` creation hunk:
  index line `00000000..f38c527b` → `00000000..808abbaa` plus the new
  descriptor bytes. No `diff --git` headers added or removed.
- `firmware/README.md` diff: regeneration-note header only — HEAD `113d1701`,
  regen date 2026-08-28, plus the one plan-sanctioned line documenting the
  HP-descriptor alignment (PIXART, 100 mA bus-powered, no IAD, byte-exact 65 B
  boot-kbd report descriptor). Todo 3 scope exactly.

### BLE identity / other profiles / api_symbols.csv

- BLE identity: no BLE files (`bt_service`, `ble_glue`, `ble_profile`) in
  either repo's diff.
- Other profiles' config descriptors: logitech/dell/msft_kbd still reference
  `hid_composite_cfg_desc`, msft_vendor still references
  `hid_vendor_cfg_desc` — only the HP profile's `cfg_descr` line changed in
  the diff. The shared keyboard report-descriptor widening is plan-documented
  (Scope item 4 note).
- `targets/f7/api_symbols.csv`: absent from both the working-tree diff and
  the regenerated patch.

VERDICT: APPROVE

Evidence: firmware repo working tree touches only
`targets/f7/furi_hal/furi_hal_usb_airbridge.c` (`git status --short` /
`git diff --name-only`, single entry); flipper-hid touches only
`firmware/airbridge-firmware.patch` + `firmware/README.md`; vendor interface
structure, endpoints (0x82/0x03), `HID_VENDOR_PACKET_LEN 64`, `HID_INTERVAL 5`,
and `hid_vendor_report_desc` all verified untouched by full diff inspection;
zero `api_symbols.csv` / `applications_user/pocket_airbridge` diff headers in
the regenerated patch; no changes to web/, docs/, BLE identity, other
profiles' config descriptors, or `api_symbols.csv`.

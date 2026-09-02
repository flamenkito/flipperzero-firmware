
## W3 — Airbridge DIS service API

W1 should include `targets/f7/ble_glue/services/airbridge_dev_info_service.h` and call:

```c
BleServiceAirbridgeDevInfo* ble_svc_airbridge_dev_info_start(
    const AirbridgeDisStrings* strings);
```

```c
typedef struct {
    const char* manufacturer_name;
    const char* model_number;
    const char* serial_number;
    uint16_t pnp_version;
} AirbridgeDisStrings;
```

The service owns copies of the three supplied strings. It exposes only DIS 0x2A29,
0x2A24, 0x2A25, and 0x2A50. Its PnP ID is seven bytes: USB source `0x01`, VID
`0x03F0` (little-endian), PID `0x5341` (little-endian), and caller-supplied
little-endian `pnp_version`.
## W4 — extra-beacon trace and suppression (2026-07-21)

### Exact startup/profile call chain

- Boot enters `bt_srv()` at `applications/services/bt/bt_service/bt.c:538`; normal boot
  starts the radio stack at `bt.c:552` and then loads the default profile/settings at
  `bt.c:553`.
- `furi_hal_bt_start_radio_stack()` is
  `targets/f7/furi_hal/furi_hal_bt.c:92-133`. Its only extra-beacon action is
  `gap_extra_beacon_init()` at `furi_hal_bt.c:131`; it never calls the beacon start API.
  First initialization explicitly records `GapExtraBeaconStateStopped` at
  `targets/f7/ble_glue/extra_beacon.c:37-43`.
- Default-profile startup goes through `bt_start_application()` at `bt.c:486-500`,
  which selects `ble_profile_serial` with `furi_hal_bt_change_app()` at
  `bt.c:488-493`. FAP profile changes use `bt_profile_start()` at
  `applications/services/bt/bt_service/bt_api.c:4-27`; that queues
  `BtMessageTypeSetProfile`, dispatched at `bt.c:593-595` to `bt_change_profile()`
  (`bt.c:414-453`), which calls `furi_hal_bt_change_app()` at `bt.c:422-427`.
- `furi_hal_bt_change_app()` (`furi_hal_bt.c:241-249`) always calls
  `furi_hal_bt_reinit()` first. Reinit issues `hci_reset()` at
  `furi_hal_bt.c:215-217`, starts a fresh radio stack at `furi_hal_bt.c:237`, and
  therefore runs `gap_extra_beacon_init()` again. If the remembered beacon state was
  Started, init restores its data, changes the software state to Stopped, and restores
  only the config at `extra_beacon.c:25-34`; there is deliberately no restart call.
- The selected profile is then started by `furi_hal_bt_start_app()`
  (`furi_hal_bt.c:165-200`): get GAP config at `:186`, `gap_init()` at `:188`, and
  template `start()` at `:195`. `gap_init()` and the GAP advertising thread
  (`targets/f7/ble_glue/gap.c:533-594,632-656`) manage only the primary connectable
  advertisement. They do not call any extra-beacon API.
- The stock serial template confirms there is no hidden profile-level enable:
  `targets/f7/ble_glue/profiles/serial_profile.c:20-31` starts only DIS, battery, and
  serial services; `:50-81` builds only `GapConfig`; and its template callbacks at
  `:83-89` contain no extra-beacon call.

### The only start/stop owner

- The sole non-wrapper caller in the firmware tree is the standalone example app.
  `ble_beacon_app_restore_beacon_state()` at
  `applications/examples/example_ble_beacon/ble_beacon_app.c:28-58` supplies default
  config only when none exists: 50-150 ms interval, all channels, 0 dBm, public
  address derived from and XOR-separated from the main BLE MAC (`:37-50`).
- `ble_beacon_app_update_state()` stops the previous beacon, writes config and data,
  then starts it only when `app->is_beacon_active` is true
  (`ble_beacon_app.c:130-147`). The HAL wrappers are
  `furi_hal_bt_extra_beacon_start/stop()` at `furi_hal_bt.c:432-438`, forwarding to
  `gap_extra_beacon_start/stop()` at `extra_beacon.c:66-114`, which alone call the
  controller additional-beacon start/stop commands.

### AirBridge suppression strategy

No forbidden-file edit and no new suppression code are required. The extra beacon is
not BT-service-global and is not started by GAP or the serial profile. Starting the
AirBridge template through `bt_profile_start()` necessarily performs the radio reset
described above, which stops any previously active controller beacon and leaves its
remembered state Stopped; the new AirBridge template must simply make no
`furi_hal_bt_extra_beacon_*` calls. This is the minimal profile-level suppression and
keeps the stock example-beacon behavior unchanged. If a future global start is ever
added, suppress it in `bt_change_profile()` (`bt.c:414-453`) based on the selected
template before advertising begins, but W4 must not and does not modify owned
`bt.c`, `gap.c`, `serial_profile.c`, or `extra_beacon.c`.
## W7 USB descriptor tooling — 2026-07-21

- On the development Mac (macOS 15.5 / 24F74), `system_profiler SPUSBDataType
  -detailLevel full` reports VID/PID, version, product/manufacturer/serial,
  and location, while `ioreg -p IOUSB -l -w 0` exposes normalized decimal
  `idVendor`, `idProduct`, `bcdDevice`, `bcdUSB`, `bDeviceClass`,
  `bMaxPacketSize0`, and `bNumConfigurations` on `IOUSBHostDevice` nodes.
- The current IORegistry output exposed no `IOUSBHostInterface` nodes, so the
  capture tool records `interfaces.count = <unavailable>` rather than treating
  unavailable data as a zero-interface descriptor. N50194 must provide the
  definitive configuration/endpoint/HID report descriptor capture.
- The existing attached Flipper (`0483:5740`) was observed only as incidental
  host inventory from the required macOS commands; W7 tooling validation must
  use a different present USB device and must not capture the Flipper or the
  genuine HP dongle before the user-gated hardware step.
## 2026-07-21 — W2 AirBridge serial service

- Canonical UUIDs in `targets/f7/ble_glue/services/airbridge_serial_uuid.h`:
  - Service: `a327d613-269c-465f-b4c5-f0ba2812877b`
  - TX: `274fa3ea-83b0-4232-b78c-9873c05e8287`
  - RX: `8dc92161-f67f-41ba-9858-b7e3eb7e2f15`
  - Flow control: `254f82db-bd5b-4cd2-8f56-d8cbbf68d9d2`
  - Status: `b3b673bf-db37-45bb-aebb-db631371bbbe`
- The fork keeps the stock service's 12-attribute budget and four characteristics exactly: RX write/read, TX indicate/read, flow-control notify/read, and status notify/read/write. The W2 shorthand says “TX notify,” but the stock implementation uses `CHAR_PROP_INDICATE`; the fork preserves that exact wire behavior.
- `bt.c` checks the active profile template through `furi_hal_bt_check_profile_type`. AirBridge TX resolves the active service and calls `ble_svc_airbridge_serial_update_tx`; connect installs `bt_serial_event_callback`, disconnect removes it, and an AirBridge RX with no raw consumer returns before the RPC fallback. The existing serial-profile RPC open/close/status/send path is unchanged and remains gated only by `ble_profile_serial`.
- Coupling: `FuriHalBleProfileBase` exposes only its template pointer, not composed service instances, so the service owns a single active-instance accessor. W1 must start exactly one AirBridge serial service before returning its profile and stop that same instance during teardown. `bt.c` uses a weak declaration for `ble_profile_airbridge` so W2 can compile before W1 lands; W1's strong template symbol replaces it.
- Build-system coupling: every header under `targets/f7/ble_glue/services` is automatically part of the FAP SDK (`target.json`), so these new internal service symbols create pending `api_symbols.csv` entries. W4 owns finalizing/disabling those entries; W2 does not edit that file.
- Verification so far: both W2 C objects compile and path-scoped lint passes. Exact `./fbt` reaches `SDKCHK` but cannot finish until W4 resolves the pending W2/W3 service headers/functions in `api_symbols.csv`; W1's profile source is also not present yet although W4 has predeclared its exported symbols.

## 2026-07-21 — W1 AirBridge composite BLE profile

- `ble_profile_airbridge` composes exactly one instance each, in order: battery,
  AirBridge DIS, stock HIDS (with the stock keyboard/mouse/consumer report map and HID
  info), then AirBridge serial. Teardown is unconditional in exact reverse order. The
  profile state starts with `FuriHalBleProfileBase` and has the same offset-zero static
  assertion as the stock HID profile. Attribute budget is
  battery(8) + AirBridge DIS(9) + HID(23) + AirBridge serial(12) = 52 <= 68.
- Final `GapConfig` mapping: `adv_name[0]` is `AD_TYPE_COMPLETE_LOCAL_NAME` (`0x09`)
  and bytes 1 onward contain the supplied NUL-terminated device name; the complete
  six-byte public MAC is copied verbatim; appearance is copied from params; the sole
  advertised service is 16-bit HIDS `0x1812`; manufacturer bytes and length populate
  `mfg_data`/`mfg_data_len` and therefore go to the scan response; bonding is enabled;
  pairing is `GapPairingPinCodeVerifyYesNo`; connection interval bounds are `0x0006`
  through `0x0024`, with zero slave latency and supervisor timeout.
- NULL profile params use a loud last-resort compiled fallback instead of any stock
  Flipper identity: name `HP Wireless KB`, public-address placeholder
  `3C:52:82:00:00:01`, keyboard appearance `0x03C1`, HP company identifier bytes
  `0E 00`, and placeholder HP DIS strings. W5 must normally pass the config-derived
  identity; the profile stores no params pointer and supports no runtime switching.
- HID report-map IDs remain keyboard=1, mouse=2, consumer=3. The wrappers accept those
  semantic IDs and translate to the frozen HID service's zero-based input-report array
  indices 0, 1, and 2; directly passing ID 3 would violate its `< 3` assertion.
- API finalization follows prior firmware history: preserve version `90.0` while changing
  its status from WIP `v` to finalized `+`; resolve the three AirBridge service headers
  and all `ble_svc_airbridge_*` rows to internal `-`; keep the three report wrapper
  signatures and strong `ble_profile_airbridge` variable as public `+`. The profile and
  identity headers are also public `+` because W5 consumes those contracts. No prior
  public `+` row was altered.
- The profile makes no `furi_hal_bt_extra_beacon_*` call. Per W4, profile installation's
  radio reinitialization is the suppression mechanism and leaves only primary GAP
  advertising active.
- Public BLE-profile APIs also require their headers in `lib/ble_profile/SConscript`'s
  explicit `SDK_HEADERS` list; CSV rows alone are removed by SDKCHK. Because the stock
  BLE-profile library was previously FAP-only at firmware link time, F7 additionally
  needs `ble_profile` in `target.json` immediately before the second `flipper7` archive
  pass so core `bt.c` can resolve the strong template and the profile can resolve its
  service dependencies.
- Final verification: repository `clang-format --dry-run --Werror` is clean for both new
  files, and full `./fbt` passes SDKCHK (`API version 90.0 is up to date`), compile, final
  firmware link, BIN/HEX/DFU generation, and dist output.
## 2026-07-21 — W1-W4 orchestrator sign-off
- Orchestrator personally reviewed: airbridge_profile.c/h, airbridge_serial_service.c/h + uuid.h, airbridge_dev_info_service.c/h, airbridge_identity_params.h, bt.c diff, api_symbols.csv diff. All on-contract.
- Orchestrator personally ran ./fbt in firmware repo: exit 0, SDKCHK "API version 90.0 is up to date".
- W1 composition: battery(8) + airbridge-DIS(9) + HID(23) + airbridge-serial(12) = 52/68 attrs. adv_service = 0x1812 HIDS. Pairing = VerifyYesNo + bonding.
- Fallback identity (NULL params): "HP Wireless KB", MAC 3C:52:82:00:00:01 (HP OUI), HP DIS strings. W5 config is the real source.
- Wrapper report IDs: kb=1, mouse=2, consumer=3 (mapped to service array index id-1).
- Known accepted limitation: W3 DIS PnP VID/PID hardcoded 03F0:5341 (matches the single USB profile; documented in plan).
- OPEN for W5/W6 contract: HP Bluetooth SIG company ID value for manufacturerData must be ONE canonical value, canonical in the FAP config, mirrored by gen_identity.py. W5 must verify the SIG-registered HP company ID and record it in the notepad.
- Hardware verification of W1-W4 (GATT enum, adv payload, pairing) deferred to W8 per plan.

## 2026-07-21 — W6 UUID byte-order correction (hardware QA)

- The firmware's `airbridge_serial_uuid.h` byte arrays are in **controller
  (network) byte order**: MSB first, as written. The header comment lists the
  same canonical strings in that same order. The on-air bytes that browsers
  and bleak observe are the **full 16-byte reversal** of the controller-order
  bytes — a byte-pair reversal, not a within-byte bit reversal. This matches
  the legacy Flipper behavior: controller `0000fe60-cc7a-482a-984a-7f2ed5b3e58f`
  on-air was `8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000`.
- Hardware proof from W6 QA: the firmware's service
  `a327d613-269c-465f-b4c5-f0ba2812877b` (controller order) appears on-air
  as `7b871228-baf0-c5b4-5f46-9c2613d627a3` (the 16 byte-pairs reversed).
  All five AirBridge UUIDs (service + TX/RX/flow/status) carry the same
  reversal; the web identity module must therefore emit the reversed form.
- `tools/gen_identity.py` keeps the controller-order cross-check against the
  header comment (so a header edit cannot silently drift), then reverses the
  16 byte-pairs with `reverse_uuid_bytes` before formatting the 8-4-4-4-12
  string. The result is deterministic: two consecutive regenerations produce
  byte-identical `web/airbridge-identity.js`.
- The new constants in `web/airbridge-identity.js`:
  - `SERIAL_SERVICE_UUID`        = `7b871228-baf0-c5b4-5f46-9c2613d627a3`
  - `SERIAL_TX_CHAR_UUID`        = `87825ec0-7398-8cb7-3242-b083eaa34f27`
  - `SERIAL_RX_CHAR_UUID`        = `152f7eeb-e3b7-5898-ba41-7ff66121c98d`
  - `SERIAL_FLOW_CONTROL_UUID`   = `d2d968bf-cbd8-568f-d24c-5bbddb824f25`
  - `SERIAL_STATUS_UUID`         = `bebb7113-63db-bbae-bb45-37dbbf73b6b3`
- `scripts/ble_qa_scan.py` reads the five constants straight out of
  `web/airbridge-identity.js` via the `IDENTITY_UUID` regex; it tracks the
  reversed form automatically. The `--selftest` synthetic GATT row confirms
  the new `7b871228-…` service and the four new characteristic UUIDs are
  matched end-to-end.

## 2026-07-21 — W5 FAP identity lifecycle and BLE Deploy typing

- The FAP config loader now accepts the frozen keys with optional whitespace around `=`:
  `ble_name`, `ble_mac`, `ble_appearance`, `ble_mfg_company`, `ble_mfg_hex`,
  `ble_dis_mfr`, `ble_dis_model`, `ble_dis_serial`, and `ble_dis_pnp`. Missing keys use
  compiled defaults: `HP Wireless Keyboard`, `3C:52:82:00:00:01`, `0x03C1`, HP company
  ID `0x0065`, empty extra manufacturer bytes, `HP`, `HP Wireless Keyboard and Mouse`,
  `HP5341KBD01`, and `0x0126`. Manufacturer data is therefore `65 00` followed by the
  decoded `ble_mfg_hex` bytes. `0x0065` is HP, Inc.; `0x011B` is HPE and is not used.
- Accepted HP/Hewlett-Packard OUIs are exactly `3C:52:82`, `48:0F:CF`, `94:57:A5`,
  `3C:D9:2B`, `B4:B6:76`, `2C:44:FD`, `A0:D3:C1`, and `40:B0:34`. A malformed, partial,
  or non-allowlisted configured MAC rejects the configured BLE identity atomically,
  restores all compiled HP defaults, and shows `WARN: BLE ID DEFAULT` on screen.
  Oversized/invalid identity fields take the same safe fallback. The loader enforces the
  fixed W1 buffers and `name length + manufacturer-data length <= 27` scan-response rule.
- `AirbridgeBleIdentityParams` and the returned `FuriHalBleProfileBase*` live inside the
  heap-allocated `AirbridgeApp`. This is intentional: `bt_profile_start()` queues
  `BtMessageTypeSetProfile`, and the BT-service thread dereferences the params pointer in
  `get_gap_config`. Although `bt_profile_start()` waits on its API lock before returning,
  stack params would violate the cross-thread lifetime contract and are not used.
- The first event-loop turn after GUI attachment opens `RECORD_BT` and calls
  `bt_profile_start(bt, ble_profile_airbridge, &app->ble_identity)` before applying the
  always-on USB profile. Centralized teardown clears typing/callback state, restores USB,
  calls `bt_profile_restore_default(bt)`, then closes `RECORD_BT`. Thus menu BACK, startup
  failure after opening BT, and exits reached after Deploy/typing/stream/error BACK paths
  all pass through the same restore. BACK during typing sends a release report immediately
  but keeps the AirBridge profile active because the FAP itself is still running.
- Deploy's temporary W5 choice is `OK=USB` / `DOWN=BLE`. Both paths use the existing
  `HID_ASCII_TO_KEY` mapping and 12 ms press / 18 ms release cadence. BLE emits the boot
  report `[mods, 0, key, 0, 0, 0, 0, 0]` and an all-zero 8-byte release after every key;
  the USB calls and abort-time `release_all` behavior remain unchanged.
- `fap_libs=["ble_profile"]` must NOT be added to this FAP. It pulls the AirBridge profile
  object into the FAP and makes APPCHK demand intentionally internal W2/W3 service symbols.
  W1 already exports the template and report wrappers from firmware, so the original
  manifest links correctly without `fap_libs`. Final verification: repository clang-format
  dry-run passed, `./fbt fap_pocket_airbridge` passed APPCHK, and full `./fbt` exited 0 with
  API 90.0 up to date and distribution output in `dist/f7-D`.

## 2026-07-21 — W6 web identity coupling

- `config/pocket_airbridge.conf` preserves the existing top-level
  `profile=hp_kbd_vendor` USB selection and carries the shared top-level `ble_*`
  identity contract used by W5.
- `tools/gen_identity.py` parses the config plus the network-order byte macros in
  `airbridge_serial_uuid.h`, verifies every parsed UUID against the canonical header
  comment, and generates `web/airbridge-identity.js` deterministically.
- Browser discovery uses ORed exact-name and manufacturer-company filters, then grants
  GATT access only through the AirBridge serial UUID in `optionalServices`; HIDS is not
  used in a Web Bluetooth filter.
- The receiver and chat pages use the persisted localStorage key
  `pocket-airbridge-ble-name-override` when an operator supplies an ad-hoc BLE name.
- Browser QA on 2026-07-21: protocol harness passed 17/17; mocked discovery captured
  `name: "Ad hoc HP Keyboard"`, company ID `0x0065`, and the canonical serial service.
## 2026-07-21 — W5/W6 orchestrator sign-off
- W5 reviewed personally: config parser (all 9 ble_* keys), 8-entry HP OUI allowlist, heap-backed ble_identity (async-safe for bt_profile_start), scan-response budget check (name+mfg ≤ overhead) with atomic fallback + WARN line, install at startup (first event-loop turn), centralized restore on exit, BLE typing = 8-byte boot report [mods,0,keycode] + zero release via ble_profile_airbridge_kb_report. Builds pass (worker-verified ./fbt + fap build; orchestrator verified ./fbt after W1).
- W6 reviewed personally + re-ran: gen_identity.py deterministic (identical MD5 across runs), cross-checks firmware header macros vs documented canonical strings; node --check passes; greps show zero namePrefix, zero fe60-64, zero 'flipper' in web/airbridge-*.js.
- Web discovery contract now live: filters [{name: BLE_NAME}, {manufacturerData: [{companyIdentifier: 0x0065}]}] (OR) + optionalServices [airbridge serial UUID]. localStorage override key: pocket-airbridge-ble-name-override.
- W5 temporary UI: deploy prompt OK=USB, DOWN=BLE — W10 rewires to d-pad on single Bridge screen.
- application.fam unchanged: fap_libs=["ble_profile"] would wrongly pull internal symbols; FAP resolves via api table exports.

## 2026-07-21 — W10 single-screen FAP UI

- The FAP now initializes directly to `AirbridgeScreenBridge`; the menu enum, renderer,
  index, and navigation are gone. Bridge BACK ends the event loop, preserving the existing
  centralized `app_restore_usb()` then `app_restore_ble()` teardown.
- Bridge UP selects `AirbridgeTypingTransportUsb` and DOWN selects
  `AirbridgeTypingTransportBle`, then opens the transport-specific confirmation prompt.
  Only OK starts the unchanged typing engine; prompt/typing/streaming cancellation and
  Done/Error dismissal return to Bridge. LEFT/RIGHT have no handlers.
- The Bridge screen keeps the identity/default-warning line, draws the four counter labels
  over one aligned digit row, and uses line-drawn up/down arrows. The stock
  `I_ButtonUp_7x4`/`I_ButtonDown_7x4` assets exist but are not FAP API exports, so directly
  referencing them failed APPCHK and the plan-approved primitive fallback was required.
- Relay forwarding now runs regardless of the visible screen. The existing Waiting-state
  `0x42` bundle request remains the sole intercepted relay event and still starts streaming;
  all other USB/BLE relay events update and forward through the existing counters/paths.
- Verification: Apple clang-format `--dry-run --Werror` passed; `./fbt
  fap_pocket_airbridge` passed through APPCHK; full `./fbt` passed with API 90.0 up to date
  and regenerated `dist/f7-D`. Static scans found no `AirbridgeScreenMenu`, `menu_index`,
  stale `BACK: Menu`, or LEFT/RIGHT action.
## 2026-07-21 — W10 orchestrator sign-off
- Reviewed render_bridge/input_callback: menu fully removed (grep zero), boot into Bridge, UP=USB deploy / DOWN=BLE deploy, LEFT/RIGHT/OK inert, BACK exits with centralized restore.
- 2-line stats: canvas-drawn arrows (stock arrow icons NOT FAP-exported) + !DROP/XTXERR text markers; legend line "↑USB deploy ↓BLE deploy BACK: exit".
- Deploy prompt now transport-specific ("OK types via USB/BLE"); consent gate unchanged.
- FAP build + full ./fbt pass (worker-verified).
## 2026-07-21 — W8 BLE QA tooling

- `scripts/ble_qa_scan.py` reads the top-level tolerant `key=value` FAP config and
  parses all five generated serial UUID constants from `web/airbridge-identity.js`;
  no target name, MAC, DIS string, or AirBridge UUID is duplicated in the tool.
- `scan` is passive and checks name, Flipper leakage across merged Bleak advertisement
  and scan-response fields, exact HIDS-only services, configured HP manufacturer ID,
  exact configured MAC plus the eight-entry HP OUI allowlist, and a second exact-name
  or adjacent-MAC source. `gatt` repeats that pre-flight then requests OS pairing and
  checks the exact service/characteristic and readable-value contract.
- CoreBluetooth deliberately gives Bleak an opaque UUID instead of the on-air address.
  The tool emits visible `WARN` rows and skips exact MAC/OUI/adjacent-MAC checks only on
  Darwin; they remain required on Linux and Windows, including N50194. `stock` is the
  inverse restoration check: `Flipper` name prefix plus a fe60-family service after FAP
  exit and after reboot.
## 2026-07-21 — W8 observed advertising-name truncation

- Hardware on-air QA observed the STM32 BLE stack advertise `HP Wireless Keyb` (16 bytes)
  with HIDS `0x1812` in the 31-byte ADV packet. The scan response carries ONLY manufacturer
  data (company ID 0x0065) by design; there is NO complete-name field in the scan response.
  The full configured name `HP Wireless Keyboard` is only present in the GAP device-name
  characteristic, verified in gatt mode. This is the expected flags + 16-bit HIDS UUID +
  local-name payload budget behavior, not an impersonation failure; HP identity was
  confirmed on-air on 2026-07-21.
- The scanner accepts the observed ADV local name when it equals config `ble_name` exactly,
  OR when it is a strict prefix of `ble_name` with length strictly less than the expected
  name (logged as "stack-truncated prefix — full name verified via GAP char in gatt mode").
  Names longer than expected and unrelated names both fail. On macOS, CoreBluetooth still
  exposes opaque UUID addresses, so MAC/OUI checks remain visible `WARN` skips rather than
  attempted assertions.
## 2026-07-21 — W8 observed Darwin GATT service visibility

- Hardware gatt-mode QA on macOS revealed CoreBluetooth does NOT expose 0x1800 (GAP),
  0x1801 (GATT), or 0x1812 (HIDS) to GATT clients — the OS claims HID devices at system
  level. The enumerated set on Darwin was exactly {0x180A DIS, 0x180F Battery, AirBridge
  serial}. DIS strings were initially `<not read>` because reads raced the pairing dialog;
  the fix is to explicitly trigger pairing via a protected-characteristic read (DIS
  manufacturer 0x2A29) before enumeration, then retry once after a 2s delay if permission
  errors remain. The operator still sees the numeric-comparison prompt and must accept it.
- The scanner's gatt mode now uses platform-split expectations: Darwin requires exactly
  {DIS, Battery, serial} and emits WARN (not FAIL) for absent {GAP, GATT, HIDS} because
  their absence is expected system behavior. Linux/Windows retain the full six-service
  expectation. GAP name/appearance checks on Darwin also emit WARN since those chars are
  unreachable through CoreBluetooth.
## 2026-07-21 — CoreBluetooth host-side verification + BLE deploy bug
- macOS Bluetooth system UI shows the Flipper as "HP Wireless Keyb", CONNECTED, with Address 01:00:00:82:52:3C (= configured MAC 3C:52:82:00:00:01 LE), Vendor ID 0x03F0 / Product ID 0x5341 (read from our DIS PnP ID 0x2A50!), Battery 100%. MAC spoof + DIS PnP impersonation confirmed host-side.
- While macOS holds the keyboard connection, the Flipper does NOT advertise (expected BLE behavior) — bleak scans show nothing; do not mistake this for a crash.
- BUG (user report): BLE Deploy types the FIRST keystroke correctly (bootstrap.js starts with '(') then shows KEYBOARD SEND ERROR. Suspects: aci_gatt_update_char_value failure under notify congestion (12/18ms typing delays), or transient failure right after macOS pairing/encryption setup. FAP aborts on FIRST false return — needs bounded retry.
- GAP: no BLE-specific bootstrap payload exists. web/bootstrap.js is WebHID-only; BLE deploy needs web/bootstrap-ble.js (Web Bluetooth: name filter + optionalServices with airbridge-identity.js UUIDs). FAP must load the transport-matching file.

## 2026-07-21 — W11 BLE bootstrap payload (web/bootstrap-ble.js)

- Payload: 1,601 bytes, pure ASCII, single-expression IIFE mirroring `web/bootstrap.js` exactly. Every identity constant is inlined from `web/airbridge-identity.js` (the on-air, byte-reversed form): exact name `HP Wireless Keyboard`, HP company ID `0x0065`, and the five AirBridge serial UUIDs in the `optionalServices` / `getPrimaryService` / `getCharacteristic` calls. No fetch, no imports, no modules.
- Discovery uses OR'd `filters: [{ name: 'HP Wireless Keyboard' }, { manufacturerData: [{ companyIdentifier: 0x0065 }] }]` with the serial service UUID in `optionalServices`. HIDS 0x1812 is NOT in any filter (Chrome blocklist SecurityError).
- GATT flow: `gatt.connect()` → `getPrimaryService(SERIAL_SERVICE_UUID)` → `getCharacteristic(TX_UUID)` / `getCharacteristic(RX_UUID)` → `startNotifications()` + `addEventListener('characteristicvaluechanged', h)` → `writeValueWithResponse(new Uint8Array([0x42]))` → `await D` (stream promise) → `removeEventListener` → `gatt.disconnect()` → checksum + `document.write` / blob fallback.
- Zombie-handle lesson mirrored from HID bootstrap: GATT is disconnected BEFORE `document.write` so the browser does not retain a dead handle into the replaced page. The catch block uses `x?.gatt?.disconnect()` to guard against the device being undefined when `requestDevice` rejects.
- Framing: same length+checksum header as the HID bootstrap (first 4 bytes = total length LE, next 4 = sum-of-bytes LE), then raw payload bytes. The first BLE indication can carry both the 8-byte header AND start of payload (BLE MTU ≥ 20 vs HID's 64-byte fixed report), so the handler pushes `d.slice(8)` after reading the header on the first event instead of returning like the HID version does.
- Contract with the FAP-side server: single byte `0x42` written to RX (write-with-response) requests the app bundle; the FAP streams the bundle as TX indications. Same 0x42 contract as the HID bootstrap.
- Errors surface on the landing page: `s.textContent` switches between `'Transfer corrupt \u2014 retry'` (checksum mismatch) and `'Transfer failed - retry'` (everything else). The `catch(e)` parameter is always bound; the inner `catch` for the document.write fallback has no param and relies on the `t` variable already being assigned by the outer try.
- `docs/firmware-guide.md` deploy-files table now lists `bootstrap-ble.js` alongside `bootstrap.js`; the deploy command line for it follows the same `storage.py send` pattern.

## 2026-07-21 — W11 FAP BLE deploy fixes

- Root cause of the first-keystroke abort: `ble_gatt_characteristic_update()` in
  `targets/f7/ble_glue/furi_ble/gatt.c` returns `result != BLE_STATUS_SUCCESS`. Because
  `BLE_STATUS_SUCCESS` is `0x00`, the wrapper returns `false` on a successful
  `aci_gatt_update_char_value()` and `true` on an error. The FAP's existing
  `app_typing_press/release` returned this value unchanged and `app_typing_step` aborts on
  a false return, so every successful BLE keystroke was misinterpreted as a failure.
  The stock `ble_profile_hid_kb_press` has the same latent inversion, but the stock HID
  app ignores the return value.
- Report-index convention confirmed: `hid_profile.c` calls
  `ble_svc_hid_update_input_report(hid_svc, ReportNumberKeyboard, ...)` where
  `ReportNumberKeyboard = 0`. The AirBridge wrapper passes `AirbridgeHidReportIdKeyboard - 1`,
  i.e. index 0, so the report index is correct; the bug was purely the inverted success/failure
  boolean.
- aci failure modes observed in `gatt.c`:
  - `BLE_STATUS_INSUFFICIENT_RESOURCES` is already retried up to 1000 times with 1 ms delays
    inside `ble_gatt_characteristic_update()`.
  - Other statuses (`BLE_STATUS_BUSY`, notification/encryption-in-flight, connection not yet
    ready, generic `BLE_STATUS_FAILED`) propagate immediately and are what the macOS host hit
    right after bonding/encryption setup.
- FAP fix: a new `app_ble_kb_report_with_retry()` helper calls
  `ble_profile_airbridge_kb_report()` up to `BLE_TYPING_RETRY_MAX = 5` times with
  `BLE_TYPING_RETRY_DELAY_MS = 20` ms backoff. It treats an underlying `false` as success and
  retries on underlying `true` (aci error). `FURI_LOG_W` records every aci failure; success
  after retries is also logged with the failure count; exhaustion logs `FURI_LOG_E` and the
  helper returns `false`, which the existing typing engine turns into the same
  `KEYBOARD SEND ERROR` screen. USB press/release/release-all remain single-call and
  byte-identical.
- BLE app streaming added via `app_stream_step_ble()`. When the FAP is in `Waiting` state,
  `app_handle_relay()` now consumes the single `0x42` byte from **either** direction (it no
  longer requires `be->to_ble`), so a BLE-deploy RX write triggers the same `app_start_stream()`.
  The stream step sends the same 8-byte header the HID bootstrap expects
  (`uint32_t total_len LE`, `uint32_t sum-of-bytes checksum LE`), then sends raw file bytes in
  up-to-64-byte chunks via `bt_serial_tx()`. The BLE bootstrap reassembles by exact byte count,
  so no report padding is used. Outside the `Waiting` state the bridge relay is unchanged.
- Transport-specific bootstrap load: `app_load_bootstrap()` now uses `app->typing_transport`
  (set before the load) to choose `bootstrap.js` for USB deploy and `bootstrap-ble.js` for BLE
  deploy. Error messages name the missing file.
- Build verification: repository `clang-format --dry-run --Werror` clean,
  `./fbt fap_pocket_airbridge` passes APPCHK, and full `./fbt` exits 0 with API 90.0 up to date
  and distribution output in `dist/f7-D`.

## 2026-07-21 — W12 BLE deploy discovery disconnect pump

- Problem: after BLE deploy typing succeeds, macOS holds the Flipper as a bonded BLE keyboard
  (`HP Wireless Keyb Connected` in system_profiler), so the device never advertises and the
  Chrome Web Bluetooth picker shows no devices.
- `bt_disconnect(Bt*)` is already FAP-exported in `targets/f7/api_symbols.csv`
  (`Function,+,bt_disconnect,void,Bt*`), so no CSV edit was needed.
- Advertising auto-resume verified: `targets/f7/ble_glue/gap.c` handles
  `HCI_DISCONNECTION_COMPLETE_EVT_CODE` by setting state to `GapStateIdle` and, when
  `gap->enable_adv` is true, calling `gap_advertise_start(GapStateAdvFast)`. Therefore calling
  `bt_disconnect()` while the profile is active automatically reopens an advertising window
  without any extra FAP call.
- Pump design: when `app_typing_step()` transitions to `AirbridgeScreenWaiting` from a BLE
  deploy, the FAP immediately calls `bt_disconnect(app->bt)` and records the tick. In the main
  loop, while the screen remains `Waiting` and the transport is BLE, every
  `BLE_WAITING_PUMP_MS = 2500` ms calls `bt_disconnect()` again. This is non-blocking
  (`furi_get_tick()` comparison only) and harmless if the link is already down. macOS HID
  auto-reconnect will typically grab the link between windows, but an actively scanning Chrome
  picker sees advertisements within one or two windows.
- Pump stops automatically when the `0x42` bundle request arrives (screen becomes
  `Streaming`) or when the user presses BACK (screen becomes `Bridge`); `Streaming` and later
  states never call `bt_disconnect()`, preserving the browser's connection for the bundle.
- USB deploy path is untouched: the pump only runs when `typing_transport ==
  AirbridgeTypingTransportBle`, and the Waiting detail text is "Waiting for browser..." for BLE
  and "Send bundle via USB" for USB.
- Build verification: `clang-format --dry-run --Werror` clean, `./fbt fap_pocket_airbridge`
  passes APPCHK, full `./fbt` exits 0 with API 90.0 up to date and `dist/f7-D` generated.
## 2026-07-21 — W11 root cause: inverted return convention (orchestrator-verified)
- ble_gatt_characteristic_update (furi_ble/gatt.c:141) returns `result != BLE_STATUS_SUCCESS` → FALSE on success, TRUE on failure. It also internally retries BLE_STATUS_INSUFFICIENT_RESOURCES 1000x1ms, and calls furi_crash on failure ONLY in debug builds (release: logs + returns true).
- Stock hid_app calls ble_profile_hid_kb_press as void (transport_ble.c) — the inverted convention never mattered before; our FAP was the first caller to check it, and checked it backwards. Every successful BLE keystroke was treated as failure → '(' typed, then KEYBOARD SEND ERROR. Matches hardware report exactly.
- Fix: app_ble_kb_report_with_retry (5 attempts, 20ms backoff) treating false=success, true=aci-error; used for BLE press/release/abort. USB path untouched.
- LESSON FOR SKILL DOCS: any bool returned from ble_svc_*_update / ble_gatt_characteristic_update chains is FALSE=success. Check stock callers before interpreting.
- W11 FAP also landed: transport-specific bootstrap load (bootstrap-ble.js for BLE), BLE app streaming over bt_serial_tx (8-byte header [len LE, checksum LE] + ≤64B chunks), 0x42 consumed from either direction in Waiting state.

## 2026-07-22 — Process rules (agent discipline)
- Added to AGENTS.md "Test Surface and Gate Discipline": (a) Playwright window as test surface, user only on physical gates; (b) CHECK-BEFORE-GATING — 5 s wait before question; (c) REGRESSION RULE — old flows pass before new debugging.
- Same three rules added to pocket-airbridge-hardware-qa/SKILL.md plus macOS Input Monitoring: permission grant requires browser RELAUNCH, not just a refresh.
- hid-composite-deploy.md lesson #11: BLE profile install (bt_profile_start → furi_hal_bt_reinit → core2 reinit) at FAP startup before USB apply breaks vendor OUT (IN works, OUT dead). Apply USB first, then BLE.
## 2026-07-21 — W13 FAP exit freeze root cause and fix (active BLE connection)

### Hardware-reproduced freeze
Two confirmed freezes, both needing physical reset:
- (a) BLE deploy attempt → KEYBOARD SEND ERROR → BACK → frozen.
- (b) BACK from Bridge while macOS held an active bonded BLE keyboard connection → frozen.
Common factor: app exit/teardown with an ACTIVE BLE connection.

### Hypotheses and verdicts

**H1 (CONFIRMED — root cause): `app_restore_ble` calls `bt_profile_restore_default`
without first disconnecting the active BLE connection.**

Exact blocking/crash chain (all file:line verified):
1. FAP exit → `app_restore_ble()` (pocket_airbridge.c:361) → `bt_profile_restore_default()`
   (bt_api.c:29) → `bt_profile_start()` (bt_api.c:4) queues `BtMessageTypeSetProfile`,
   blocks on `api_lock_wait_unlock_and_free` (bt_api.c:23).
2. BT service thread (bt.c:622) → `bt_change_profile()` (bt.c:442) →
   `furi_hal_bt_change_app()` (furi_hal_bt.c:241) → `furi_hal_bt_reinit()`
   (furi_hal_bt.c:202).
3. `furi_hal_bt_reinit` calls `furi_hal_bt_stop_advertising()` (furi_hal_bt.c:207) →
   `gap_stop_advertising()` (gap.c:515) queues `GapCommandAdvStop` to GAP thread.
4. GAP thread (gap.c:632 `gap_app`) processes `GapCommandAdvStop` → `gap_advertise_stop()`
   (gap.c:476): calls `aci_gap_terminate()` (sends terminate to M0 core) then sets
   `gap->state = GapStateIdle` SYNCHRONOUSLY (gap.c:497) — does NOT wait for the
   `HCI_DISCONNECTION_COMPLETE_EVT_CODE` event.
5. Spin loop in `furi_hal_bt_stop_advertising` (furi_hal_bt.c:264
   `while(furi_hal_bt_is_active())`) exits because state is now Idle.
6. `furi_hal_bt_reinit` then calls `current_profile->config->stop()` (furi_hal_bt.c:211)
   → `ble_profile_airbridge_stop` (airbridge_profile.c:173) →
   `ble_svc_airbridge_serial_stop` (airbridge_serial_service.c:229) → sets
   `active_airbridge_serial_service = NULL` (line 233) and frees the service (line 241).
   Also frees the profile struct (airbridge_profile.c:182).
7. The M0 core's `HCI_DISCONNECTION_COMPLETE_EVT_CODE` event arrives ASYNCHRONOUSLY
   on the HCI event thread → `ble_event_app_notification` (gap.c:130) → calls
   `gap->on_event_cb` = `bt_on_gap_event_callback` (bt.c:290).
8. On `GapEventTypeDisconnected` (bt.c:339-344):
   ```c
   if(current_profile_is_airbridge) {
       BleServiceAirbridgeSerial* serial_svc = ble_svc_airbridge_serial_get_active();
       furi_check(serial_svc);   // <-- NULL (cleared in step 6) → furi_crash → FREEZE
   ```
   - `bt_profile_is_airbridge(bt->current_profile)` (bt.c:29) dereferences
     `bt->current_profile` which is a DANGLING POINTER (freed in step 6, not cleared
     until `bt_change_profile` returns at bt.c:450) → USE-AFTER-FREE.
   - `ble_svc_airbridge_serial_get_active()` returns NULL → `furi_check(serial_svc)`
     fires → `furi_crash()` → in release looks like a freeze; in debug shows crash screen.

The primitive that hangs/crashes: `furi_check` (not a queue/mutex/event-flag wait).
The crash is triggered by the async HCI event racing ahead of the synchronous profile
teardown. The FAP's `app_restore_ble` skipped the disconnect-first step that the stock
`hid_app` performs.

**H2 (contributing, covered by the fix): BLE-Waiting disconnect pump (W12) racing teardown.**
The pump (pocket_airbridge.c:973-977) calls `bt_disconnect` every 2500 ms in Waiting
state. When BACK exits Waiting → Bridge, the pump stops (screen is no longer Waiting),
but a DISCONNECTION_COMPLETE event from the pump's last `bt_disconnect` may still be
in flight when `app_restore_ble` runs. The fix's `bt_disconnect + 200 ms` covers this
because `bt_disconnect` is a full synchronous disconnect cycle and the 200 ms delay
lets any in-flight HCI event drain before the profile is torn down.

**H3 (not the issue): `bt_raw_serial_cb` diversion (bt.c:221-223).**
`app_restore_usb` (pocket_airbridge.c:405) calls `bt_set_raw_serial_callback(NULL,
NULL)` BEFORE `app_restore_ble`, so the raw serial hook is cleared before teardown.
Correct ordering; not a factor.

**H4 (not the issue): `ble_svc_airbridge_serial_stop`'s
`furi_check(serial_svc == active_airbridge_serial_service)` (airbridge_serial_service.c:231).**
Called from `ble_profile_airbridge_stop` during reinit. At that point active is still
set (cleared INSIDE `ble_svc_airbridge_serial_stop` at line 233, after the check).
Check passes; not a factor.

**H5 (not the issue): `furi_hal_bt_stop_advertising` spin loop**
(furi_hal_bt.c:264 `while(furi_hal_bt_is_active())`). `gap_advertise_stop` sets state
to Idle synchronously after `aci_gap_terminate`, so the spin loop exits. Not a hang.

### Stock hid_app exit comparison (hid.c:230-239)
```c
bt_disconnect(app->bt);          // terminate + wait for state = Idle
furi_delay_ms(200);              // let HCI DISCONNECTION_COMPLETE event drain
bt_keys_storage_set_default_path(app->bt);
furi_check(bt_profile_restore_default(app->bt));
```
The stock app ALWAYS disconnects first and waits 200 ms for the 2nd core to update NVM
storage (and for the async DISCONNECTION_COMPLETE event to be processed). Our FAP was
missing both steps.

### Fix (pocket_airbridge.c only, app_restore_ble)
Added `bt_disconnect(app->bt); furi_delay_ms(200);` before
`bt_profile_restore_default(app->bt)` in `app_restore_ble`, mirroring the stock hid_app
exit. `bt_disconnect` is already FAP-exported (api_symbols.csv) and used by the W12 pump.
It is safe to call with no active connection (`furi_hal_bt_stop_advertising` is a no-op
when state is Idle). After `bt_disconnect`, `gap->enable_adv` is false so macOS cannot
auto-reconnect during the 200 ms window.

### Exit path re-enumeration after fix
All exit paths funnel through `app_restore_ble` (pocket_airbridge.c:991):
1. **Bridge BACK** → loop exits → `app_restore_usb` → `app_restore_ble` (disconnect +
   200 ms + restore). ✓
2. **Error BACK** (KEYBOARD SEND ERROR / STREAM ERROR / config errors) → screen → Bridge
   → BACK → same as #1. ✓
3. **DeployPrompt BACK** → screen → Bridge → BACK → same as #1. ✓
4. **Typing BACK-abort** → `app_abort_typing` (release report) → Bridge → BACK → #1. ✓
5. **Streaming BACK** → `app_stream_close` → Bridge → BACK → #1. ✓
6. **Startup BLE CONFIG ERROR** → `running = false` → `app_restore_usb` (if configured) →
   `app_restore_ble` (bt is non-NULL, ble_profile may be NULL; `bt_disconnect` is safe). ✓
7. **Waiting BACK** (after BLE deploy) → Bridge → BACK → #1. Pump stopped before BACK
   (input processed before pump check in main loop); any in-flight pump disconnect event
   is drained by the 200 ms delay. ✓

### Remaining risks
- The 200 ms delay is the same value the stock hid_app uses. If a future host stack takes
  longer than 200 ms to deliver DISCONNECTION_COMPLETE, the race could theoretically
  recur. The stock firmware has shipped this value for years without reports, so it is
  considered sufficient.
- The true fix belongs in bt.c (clear `bt->current_profile` before reinit, and make
  `bt_on_gap_event_callback` tolerate a NULL active service on disconnect). Per task
  constraints, bt.c is NOT modified; the FAP-side disconnect-first pattern prevents the
  race from occurring.
- `furi_hal_bt_reinit` itself still has the latent use-after-free on `bt->current_profile`
  if any OTHER caller triggers a profile change while a connection is active without
  disconnecting first. The FAP is now safe; other callers are out of scope.
### Build verification

- `xcrun clang-format --dry-run --Werror applications_user/pocket_airbridge/pocket_airbridge.c`
  → exit 0 (clean).
- `./fbt fap_pocket_airbridge` → SDKCHK pass, APPCHK pass, FAP generated.
- Full `./fbt` → exit 0, API 90.0 up to date, `dist/f7-D` generated.

- Bumped `applications_user/pocket_airbridge/application.fam` `stack_size` from `2 * 1024` to `4 * 1024` to accommodate the larger `AirbridgeApp` state, dynamic bootstrap buffer, and deeper call chains; `./fbt fap_pocket_airbridge` APPCHK pass.
- Restored `airbridge_ble_enabled = true` in `pocket_airbridge.c` (bisect guard flipped back); USB-OUT bisect concluded as host-side, FAP+firmware exonerated; `./fbt fap_pocket_airbridge` APPCHK pass.

## 2026-07-22 — BLE deploy zero-notification root cause

- `targets/f7/ble_glue/services/airbridge_serial_service.c` `ble_svc_airbridge_serial_update_tx()`
  was calling `aci_gatt_update_char_value_ext()` with `update_type = 0x02`
  (`GATT_CHAR_UPDATE_SEND_INDICATION`) for the final fragment, but the TX characteristic had
  been switched to `CHAR_PROP_NOTIFY`. The stack silently dropped the update because a NOTIFY
  characteristic cannot be indicated, so clients saw zero notifications and `tx_errors` stayed 0.
- Fixed by using `GATT_CHAR_UPDATE_SEND_NOTIFICATION` (0x01) for the final fragment; non-final
  fragments remain `GATT_CHAR_UPDATE_LOCAL_ONLY` (0x00) to accumulate the full value. Added a
  comment noting that if TX ever reverts to `CHAR_PROP_INDICATE`, the final fragment must use
  `GATT_CHAR_UPDATE_SEND_INDICATION` (0x02).
- Constants from `lib/stm32wb_copro/wpan/ble/core/ble_defs.h:400-402`:
  `LOCAL_ONLY=0x00`, `SEND_NOTIFICATION=0x01`, `SEND_INDICATION=0x02`.
- Full `./fbt` rebuilt `airbridge_serial_service.c`, linked firmware, and generated
  `dist/f7-D`; clang-format clean.
- Added BLE-specific typing delays (`BLE_TYPE_PRESS_DELAY_MS = 25`, `BLE_TYPE_RELEASE_DELAY_MS = 35`) for `AirbridgeTypingTransportBle`, while keeping USB delays at 12/18 ms. BLE HID input reports are NOTIFY and can be silently dropped under macOS congestion; slower pacing is the only reliable defense. The retry wrapper still handles aci-level failures as a second layer.
- Fixed BLE deploy Waiting pump misfire: the 2.5 s pump was disconnecting the browser after connect because it treated any Waiting interval as a kick opportunity. New state machine:
  - `ble_waiting_ever_connected` is set on the first `BtStatusConnected` event while in Waiting.
  - The pump (`bt_disconnect` + `furi_hal_bt_start_advertising`) fires only while `!ble_waiting_ever_connected`.
  - The 45 s connected-timeout disconnect is removed entirely; a connected central is never dropped in Waiting.
  - The flag is reset on Waiting entry, on BACK from Waiting, and on transition to Streaming.
  - `./fbt fap_pocket_airbridge` APPCHK pass.
- Disabled persistent BLE bonding for the AirBridge profile (`lib/ble_profile/extra_profiles/airbridge_profile.c`: `.bonding_mode = false`). Pairing remains `GapPairingPinCodeVerifyYesNo` (per-session MITM), so the AUTHEN-protected serial/DIS/HID characteristics are still encrypted. With bonding off, macOS no longer hoards the link for a bonded keyboard and the browser can connect without an unpair step. `./fbt` full firmware build green.
- Further hardened BLE typing pacing: increased delays to `BLE_TYPE_PRESS_DELAY_MS = 40` / `BLE_TYPE_RELEASE_DELAY_MS = 60` (USB unchanged 12/18). Added `BLE_TYPE_MODIFIED_SETTLE_MS = 10` extra delay after releasing any key with a modifier byte (`key >> 8 != 0`) on BLE. Rationale: at 25/35 a full 2,300-char bootstrap still dropped one character (the `8` in `Uint8Array` became `UintArray`, causing a ReferenceError and breaking every stream indication). NOTIFY-based HID reports are fire-and-forget, so slower/more-stable pacing is the only reliable fix without host feedback.
- Fixed ISSUE-4 (Bridge-mode advertising wedge): `app_ble_status_changed_callback()` now calls `furi_hal_bt_start_advertising()` on every BLE central disconnect. This restores advertising in Bridge mode after a client disconnect, while the deploy-Waiting pump keeps its own disconnect+advertise cycle. The call is safe because `furi_hal_bt_start_advertising()` is a no-op unless GAP state is Idle, and `app_restore_ble()` handles its own teardown. `./fbt fap_pocket_airbridge` APPCHK pass.

## 2026-07-22 — USB OUT bisect guard + instrumentation

### Bisect guard

- Added `static const bool airbridge_ble_enabled = false;` at the top of
  `applications_user/pocket_airbridge/pocket_airbridge.c`. When `false`, the deferred
  startup block skips `app_configure_ble()` entirely (no `RECORD_BT` open, no
  `bt_profile_start()`, no `furi_hal_bt_reinit()`). When `true`, behavior is the normal
  USB-first-then-BLE startup. Flip the constant to re-enable BLE.

### Instrumentation

- Added `static uint32_t vendor_out_requests;` and incremented it inside
  `usb_event_callback()`'s `HidVendorRequest` branch. This counts every time the USB HAL
  delivers a vendor OUT event to the FAP, independent of whether the relay queue accepts it.
- The Bridge screen now draws `V<count>` at `(113, 42)` next to the existing counters.

### USB vendor OUT path sanity check

- `furi_hal_hid_vendor_set_callback()` (`furi_hal_usb_airbridge.c:489-500`) installs
  `usb_event_callback` as the profile's HID-vendor event handler.
- `hid_vendor_txrx_ep_callback()` (`furi_hal_usb_airbridge.c:643-652`) fires on OUT
  endpoint events and calls `callback(HidVendorRequest, cb_ctx)`.
- `furi_hal_hid_vendor_get_request()` (`furi_hal_usb_airbridge.c:579-582`) reads the
  actual bytes from `hid_vendor_ep_out` via `usbd_ep_read()`.
- The endpoint is configured in `hid_vendor_ep_config()` (`furi_hal_usb_airbridge.c:671-674`)
  when the host selects configuration 1. No today changes touched these paths.

### Suspects for why BLE reinit kills USB OUT

1. **Clock glitch on the shared radio/USB 48 MHz source.** `furi_hal_bt_reinit()`
   (`furi_hal_bt.c:202-239`) resets CPU2 through `ble_glue_reinit_c2()` and restarts the
   radio stack. The USB full-speed PHY clock is derived from PLLSAI1 (`furi_hal_usb_init()`,
   `furi_hal_usb.c:91`). If the BLE stack reinitialization momentarily disturbs PLLSAI1 or
   the HSE/MSI selection while the host is already sending OUT packets, the OUT path can
   drop while IN (device-driven) recovers.
2. **Shared bus/NVIC preemption window.** `furi_hal_bt_reinit()` disables/enables
   `FuriHalBusHSEM/IPCC/AES2/PKA/CRC` inside `FURI_CRITICAL_ENTER/EXIT`
   (`furi_hal_bus.c:176-207, 242-269`). Although USB is on APB1 and these buses are not,
   the critical-section window and CPU2 reset delay may stall the USB ISR long enough for
   the host to NAK an OUT transaction, leaving the endpoint in a state the device never
   recovers from.
3. **Endpoint registration lost after a USB reset event.** `usb_process_mode_reinit()`
   (`furi_hal_usb.c:360-375`) only runs on explicit `UsbApiEventTypeReinit`; BLE reinit does
   not call it. However, if the host sees a USB reset/descriptor timeout due to (1) or (2)
   and re-enumerates, the OUT endpoint configuration from `hid_vendor_ep_config()` may not
   be re-applied correctly if the wakeup/suspend state machine in `furi_hal_usb.c:299-327`
   gets stuck.
4. **Vendor callback context stale.** If `usb_event_callback` is installed before BLE
   reinit but the reinit sequence blocks the main thread, OUT events are still queued by
   the USB thread; the more likely failure is that the callback fires but
   `furi_hal_hid_vendor_get_request()` returns 0 because `usb_dev` or `hid_vendor_ep_out`
   became invalid. The new `V` counter will show whether the callback fires at all.

### Build verification

- `xcrun clang-format --dry-run --Werror applications_user/pocket_airbridge/pocket_airbridge.c`
  → exit 0 (clean).
- `./fbt fap_pocket_airbridge` → SDKCHK pass, APPCHK pass, FAP generated.
- Full `./fbt` → exit 0, API 90.0 up to date, `dist/f7-D` generated.

## 2026-07-22 — BLE identity short model name

- The canonical advertised name, DIS model number, FAP defaults, and profile-only NULL-params
  fallback are now `HP 725 K+M`. Its 10 characters fit the observed advertising-name budget
  without stack truncation; the template MAC remains `3C:52:82:00:00:01`.
- `python3 tools/gen_identity.py` regenerated `web/airbridge-identity.js` with that exact
  `BLE_NAME`, and `python3 tools/build_bundle.py` rebuilt `dist/app-usb.html`.
- Verification: no `HP Wireless Keyboard` matches remain in `config/`, `web/`,
  `applications_user/`, or `lib/ble_profile/`; `node --check web/airbridge-identity.js`,
  `./fbt fap_pocket_airbridge`, and full `./fbt` all passed.

## 2026-07-21 — AirBridge GATT registration capacity and failure instrumentation

### Controller contract and failure cause

- `ble_gatt_characteristic_init()` in `targets/f7/ble_glue/furi_ble/gatt.c` is a
  **void** wrapper. Before this fix it logged a nonzero `aci_gatt_add_char()` result and
  continued; in release `ble_gatt_strict_crash` expands to nothing. `ble_gatt_service_add()`
  is conventional: it returns `true` only for `BLE_STATUS_SUCCESS`. Changing the public
  initializer signature would require the forbidden `api_symbols.csv` change, so the wrapper
  now frees and clears `BleGattCharacteristicInstance.characteristic` on a characteristic or
  descriptor add failure. Every composite constructor checks that state, logs the failing
  characteristic index, removes only successfully-created attributes/services, and returns
  `NULL`. Service-add failures now also return normally rather than crashing in debug before
  their callers can propagate failure.
- ST's in-tree `lib/stm32wb_copro/wpan/interface/patterns/ble_thread/shci/shci.h` documents
  `AttrValueArrSize`: each characteristic reserves its declared maximum value length plus
  5 B (16-bit UUID) or 19 B (128-bit UUID), 2 B per link for a CCCD, and any descriptor
  value length. `ble_gatt_aci.h` documents `Char_Value_Length` as the maximum; it is reserved
  even when the characteristic is variable.
- Therefore the observed missing RX/TX pair is ATT value-array exhaustion, not a UUID/order
  issue and not a per-characteristic registration limit. `aci_gatt_add_char()` accepts a
  `uint16_t` max length, while the in-tree STM32WB docs identify `BLE_STATUS_OUT_OF_MEMORY`
  (`0x98`) as the relevant permanent allocation failure; other plausible status values are
  `BLE_STATUS_OUT_OF_HANDLE` (`0x61`), `BLE_STATUS_INVALID_OPERATION` (`0x62`),
  `BLE_STATUS_INSUFFICIENT_RESOURCES` (`0x64`), and `BLE_STATUS_INVALID_PARAMS` (`0x92`).
  The new logs preserve the exact status so hardware QA can distinguish them.

### Exact 2-link ATT value-array arithmetic

`CFG_BLE_NUM_LINK = 2`, so a CCCD costs 4 B. With the default 20-byte HP name and DIS
strings `HP` (2), `HP Wireless Keyboard and Mouse` (30), and `HP5341KBD01` (11):

| Component | Calculation | Bytes |
| --- | --- | ---: |
| GATT/GAP system | Service Changed `4+5+4`; Device Name `20+5`; Appearance `2+5`; PPCP `8+5` | 58 |
| Battery | `2 * (1+5+4)` | 20 |
| AirBridge DIS | `2+5 + 30+5 + 11+5 + 7+5` | 70 |
| HIDS | Protocol `1+5`; report map `255+5`; info `4+5`; control `1+5`; three input reports `3 * (255+5+4+2)` | 1079 |
| Pre-serial total | `58+20+70+1079` | 1227 |
| Old serial | RX `486+19`; TX `486+19+4`; flow `4+19+4`; status `4+19+4` | 1068 |
| Old composite total | `1227+1068` | 2295 |
| New serial | RX `244+19`; TX `244+19+4`; flow `4+19+4`; status `4+19+4` | 584 |
| New composite total | `1227+584` | **1811** |

At the old 1344-B pool, serial starts with only 117 B: RX (505 B) and TX (509 B) fail without
allocation, then flow and status (27 B each) fit and leave 63 B. This exactly explains the two
characteristics observed on fresh clients. Reducing RX/TX alone to 244 B is still insufficient,
so `CFG_BLE_ATT_VALUE_ARRAY_SIZE` is increased to the 4-byte-aligned 1812 B minimum.

### Code change and verification

- AirBridge RX/TX declarations are 244 B; the four-character order, UUIDs, and `bt.c` coupling
  are unchanged. `BLE_SVC_AIRBRIDGE_SERIAL_CHAR_VALUE_LEN_MAX` remains 243 because the
  STM32WB `aci_gatt_update_char_value_ext` command permits at most 243 B per command (though
  its total value can be 512 B): a 244-B TX update is correctly split as 243+1.
- AirBridge serial defers event-handler registration until all GATT setup and initial status
  initialization succeed. Battery, AirBridge DIS, and HID now log and unwind add failures;
  `ble_profile_airbridge_start()` propagates any start/report-map/HID-info failure by stopping
  earlier services in reverse order. `furi_hal_bt_start_app()` still leaves the GAP thread alive
  if a profile template returns `NULL`; touching that caller would exceed the task's permitted
  profile-only propagation surface, but no half-composed AirBridge profile remains.
- `xcrun clang-format --dry-run --Werror` passed on all touched files. After an `./fbt -c`
  generated-artifact cleanup, full `./fbt` passed SDKCHK (`API version 90.0 is up to date`),
  link, HEX/BIN/DFU generation, and `dist/f7-D` output. No flash or hardware test was run.

## 2026-07-21 — R1/R3 repeated launch/exit guards

- **R1 PASS:** `applications/services/bt/bt_service/bt.c:308-313` now guards the
  AirBridge connect callback installation and logs an error when the active serial service
  is absent; `bt.c:345-347` now silently skips callback clearing when a late disconnect
  arrives after service teardown. The serial-profile/RPC branches at `bt.c:316-332` and
  `bt.c:349-357` are unchanged.
- **R3 PASS:** `applications_user/pocket_airbridge/pocket_airbridge.c:78` adds
  `ble_profile_installed`; `pocket_airbridge.c:357-358,361-362` makes
  `app_configure_ble` idempotent and sets the flag only after successful profile start;
  `pocket_airbridge.c:366-369,387-388` clears the FAP-local install state on every
  restore outcome, including a missing BT handle or a reported default-profile restore failure.
- Verification: `xcrun clang-format --dry-run --Werror` passed for both changed source
  files; `./fbt fap_pocket_airbridge` passed APPCHK; full `./fbt` compiled `bt.c`, linked
  firmware, generated HEX/BIN/DFU, and wrote `dist/f7-D` with API 90.0 up to date.

## 2026-07-21 — R4–R8 FAP lifecycle audit (verification-only)

No `pocket_airbridge.c` change was warranted: all R4–R8 verdicts below are SAFE for
the current FAP/firmware contract. R1–R3 were not revisited.

| Ref | Verdict | File:line evidence and conclusion |
| --- | --- | --- |
| R4 — params lifetime race | **SAFE** | `app_configure_ble()` passes the heap-owned `&app->ble_identity` to `bt_profile_start()` at `applications_user/pocket_airbridge/pocket_airbridge.c:356-362`; the containing `AirbridgeApp` is not freed until `:1022`, after BLE restore at `:1013`. More importantly, `bt_profile_start()` queues `BtMessageTypeSetProfile` at `bt_api.c:13-21` and waits on its API lock at `:23` before returning. The BT thread dispatches that message synchronously to `bt_change_profile()` at `bt.c:625-626`; it calls `furi_hal_bt_change_app()` at `:454-459`, which calls `get_gap_config()` at `furi_hal_bt.c:186` before profile `start()` at `:195`. Only after the handler returns does the BT thread unlock at `bt.c:639`. AirBridge dereferences the params in `ble_profile_airbridge_get_config()` at `airbridge_profile.c:242-258`, so `bt_profile_start()` is synchronous through that read. A fast FAP exit cannot free `app` while its own event-loop thread is blocked in that call. |
| R5 — partial profile start | **SAFE — ordinary capacity failure is theoretical, not a plausible fixed-profile launch failure** | Attribute reservation is `battery(8) + DIS(9) + HID(23) + serial(12) = 52`, below the 68-attribute provision (`airbridge_profile.c:150`; `app_conf.h:52-58`), leaving 16 records. `CFG_BLE_ATT_VALUE_ARRAY_SIZE` is 1344 (`app_conf.h:66-75`). The current default live values total 1,190 B before report-reference descriptor values, or 1,196 B including the three 2-B report references: serial `486 + 486 + 4 + 4 = 980` (`airbridge_serial_service.h:12-13`, `airbridge_serial_service.c:24-63`); DIS `2 + 30 + 11 + 7 = 50` from the FAP defaults (`pocket_airbridge.c:36-39`) and DIS characteristic lengths (`airbridge_dev_info_service.c:63-107`); battery `1 + 1 = 2` (`battery_service.c:51-71`); HID protocol/map/info/control/input reports `1 + 138 + 4 + 1 + 8 + 4 + 2 = 158` (the exact 138-B map is `airbridge_profile.c:28-102`; HID fixed values are `hid_service.c:75-118`). Thus the normal identity has 148 B of raw-value margin; maximum accepted DIS strings add only 50 B (`airbridge_identity_params.h:23-26`), retaining 98 B. The profile has also been hardware-enumerated with this composition (W8), so the specific configured composite is not resource-exhausted. The unchecked `battery -> DIS -> HID -> serial` construction remains visible at `airbridge_profile.c:147-170`, but requires an independent heap/controller GATT failure rather than normal FAP lifecycle or configured GATT budget exhaustion. |
| R6a — stream close on every Streaming exit | **SAFE** | USB header failure closes at `pocket_airbridge.c:621-625`, USB payload failure at `:640-644`, and USB normal completion at `:631-634`; BLE equivalents are `:661-664`, `:680-683`, and `:670-673`. Streaming BACK closes at `:897-901`. The unconditional process teardown calls `app_stream_close()` at `:1010`, covering any future/direct loop exit as well. Done/error inputs merely return to Bridge (`:905-907`), where the only process exit is Bridge BACK (`:863-866`), after the stream has already closed. |
| R6b — no stuck typing key | **SAFE** | `typing_key_down` becomes true only after a successful press at `pocket_airbridge.c:544-550`. A release failure calls `app_abort_typing()` before showing the error (`:522-525`); Typing BACK calls it at `:889-893`; and unconditional teardown calls it at `:1011`. `app_abort_typing()` emits a release when needed and clears `typing_key_down` plus both enter flags at `:138-152`. Failed presses never set the flag, and the non-US-ASCII error occurs before a press (`:538-546`). |
| R6c — callback/queue teardown order | **SAFE** | The only callback installation is after successful USB configuration at `pocket_airbridge.c:411-421`. `app_restore_usb()` clears raw-serial first and vendor-HID second at `:424-430`; final teardown invokes it at `:1012`, before BLE restore and well before `furi_message_queue_free()` at `:1021`. If `usb_configured` is false, `app_restore_usb()` returns at `:425`, but that FAP instance cannot have installed these callbacks. |
| R7 — service/profile free symmetry | **SAFE** | DIS stop frees manufacturer, model, serial, then its struct at `airbridge_dev_info_service.c:133-145`; its startup failure path also frees all successful `strdup`s at `:50-59`. Serial stop clears the active singleton, unregisters its event handler, deletes characteristics/service, frees `buff_size_mtx`, then frees the struct at `airbridge_serial_service.c:229-242`. AirBridge profile stop frees services in reverse construction order and frees the profile allocation at `airbridge_profile.c:173-182`. |
| R8 — cross-cycle reset and allocation symmetry | **SAFE** | Every launch allocates and zeroes a fresh `AirbridgeApp` (`pocket_airbridge.c:929-935`) and resets all four displayed relay counters plus USB/UI globals at `:936-942`; the initial screen is Bridge at `:934`. Teardown balances app, queue, storage record/files, GUI/notification records, and viewport at `:1014-1022`; BT is closed by `app_restore_ble()` at `:365-392` whenever it was opened. The R3 flag is zeroed with the app, blocks duplicate install at `:356-357`, is set only for a non-NULL start result at `:359-362`, and is cleared on every restore outcome (`:366-391`). |

### Residual risks for the repeated-launch E2E gauntlet

- `ble_profile_airbridge_start()` has no transactional NULL checks (`airbridge_profile.c:147-170`). If a future firmware/resource change makes a service allocation or `ble_gatt_service_add()` fail, later dereferences or stop calls can fault; serial-start failure also occurs after registering its event handler (`airbridge_serial_service.c:164-190`). This is outside the allowed FAP-only edit surface. **Recommendation:** harden `airbridge_profile.c` and each service constructor to check every allocation/service-add result, unwind already-started services in reverse order, unregister any registered event handler on failure, and return NULL only after complete cleanup.
- `ble_gatt_characteristic_init()` logs failed characteristic additions but returns no status (`furi_ble/gatt.c:38-52`), so a controller-level characteristic failure cannot be propagated to the profile constructor. The current hardware-verified profile fits, but a firmware configuration change should be tested with full GATT enumeration after each change.
- R4 depends on the present API-lock completion contract. If `bt_profile_start()` is later changed to acknowledge before `furi_hal_bt_start_app()` consumes `get_gap_config`, the profile parameters must be copied into static/profile-owned storage or a separate install-complete acknowledgment must gate FAP exit.
- Run the E2E gauntlet across repeated disconnected and actively bonded launch/exit cycles, USB and BLE deploy normal completion/abort/error paths, and a stream BACK/error/completion each cycle; watch for GATT enumeration loss, stuck host keys, nonzero DROP/TXERR, and crash/freeze logs.

## 2026-07-21 — BLE full-picker discovery

- Browser BLE discovery now uses `acceptAllDevices: true` in `bootstrap-ble.js`,
  `WebBluetoothAdapter`, `receiver.html`, and `chat-ble.html`; the serial UUID remains
  the sole `optionalServices` entry. Device-name overrides and their persisted setting
  were removed so the user selects the HP Wireless device from the full picker list.
- `web/bootstrap-ble.js` is pure ASCII and measures 1,535 characters (`wc -m`).

## 2026-07-21 — W14 BLE deploy disconnect pump: connection-aware + explicit advertising restart

### Problems with the W12 pump

1. **Connection-blind disconnects.** The W12 pump called `bt_disconnect(app->bt)` every
   `BLE_WAITING_PUMP_MS = 2500` ms while the screen was `AirbridgeScreenWaiting`, even if a
   central had already connected and was still setting up the GATT link or waiting for the
   user to pick the device in Chrome. This could tear down a legitimate connection before
   the browser had a chance to write `0x42` and start streaming.
2. **Advertising wedge.** On some runs the device stopped being discoverable after repeated
   pump disconnects even though `gap.c` auto-resumes advertising on
   `HCI_DISCONNECTION_COMPLETE_EVT_CODE`. The auto-resume path depends on `gap->enable_adv`
   remaining true and the event being delivered; an explicit FAP-side advertising restart
   gives a second chance if that path fails.

### Connection-state tracking

- `bt_set_status_changed_callback(Bt*, BtStatusChangedCallback, void*)` is FAP-exported and
  is registered after `bt_profile_start()` succeeds at
  `applications_user/pocket_airbridge/pocket_airbridge.c:381-383`. The callback is cleared
  with `NULL` before closing `RECORD_BT` in `app_restore_ble()` at `:409` so the BT thread
  never calls back into a freed `AirbridgeApp`.
- `AirbridgeApp` now carries `bool ble_connected` and `uint32_t ble_waiting_connected_tick`.
  The status callback (`:142-155`) updates the flag on any change and records the tick when
  transitioning to `BtStatusConnected`; it clears the tick on disconnect.

### Connection-aware pump logic

- New constant `BLE_WAITING_CONNECTED_TIMEOUT_MS = 45000` at `:32`.
- On transition into BLE Waiting (`app_typing_step()` at `:589-595`), the FAP snapshots
  `ble_waiting_connected_tick = ble_connected ? now : 0`, records the first pump tick,
  calls `bt_disconnect()` to clear any existing link, and immediately calls
  `furi_hal_bt_start_advertising()` to ensure advertising begins even if the disconnect is
  not yet complete.
- In the main-loop Waiting pump (`:1018-1041`):
  - If `ble_connected` is true, the pump does nothing unless the connection has been held
    for at least 45 s without receiving a `0x42`. Only then does it disconnect once and
    restart advertising, resetting `ble_waiting_connected_tick` to 0.
  - If `ble_connected` is false, the pump still runs every 2.5 s: `bt_disconnect()` is a
    no-op when no link exists, and `furi_hal_bt_start_advertising()` explicitly restarts
    advertising whenever the GAP state is `GapStateIdle`.

### Why `furi_hal_bt_start_advertising()` is safe to call from the FAP

- `targets/f7/api_symbols.csv` exports it (`Function,+,furi_hal_bt_start_advertising,void,`),
  and `furi_hal_bt.h` declares it. The implementation at
  `targets/f7/furi_hal/furi_hal_bt.c:255-259` starts advertising only when
  `gap_get_state() == GapStateIdle`, so calling it while connected is a harmless no-op.
- It is called immediately after `bt_disconnect()` in both the entry path and the pump path.
  Because `bt_disconnect()` terminates an active connection and waits for idle, the follow-up
  `start_advertising()` either kick-starts advertising right away (if already idle) or does
  nothing and lets the pending disconnect-complete handler restart it.

### Build verification

- `xcrun clang-format --dry-run --Werror applications_user/pocket_airbridge/pocket_airbridge.c`
  → exit 0 (clean).
- `./fbt fap_pocket_airbridge` → SDKCHK pass, APPCHK pass, FAP generated.
- Full `./fbt` → exit 0, API 90.0 up to date, `dist/f7-D` generated.

## 2026-07-21 — W15 BLE deploy streaming paced on indication confirmations

### Hardware symptom

After the ATT pool fix, BLE deploy reached GATT connect and the FAP received the `0x42`
bundle request, but the browser stayed at "Connecting" and the Flipper showed
"Deploy error / STREAM ERROR". Root cause: `app_stream_step_ble()` sent the 8-byte header
and then ≤64-byte payload chunks back-to-back via `bt_serial_tx()` with no pacing. The
AirBridge TX characteristic is `CHAR_PROP_INDICATE`; `aci_gatt_update_char_value_ext`
returns `BLE_STATUS_INSUFFICIENT_RESOURCES` (or a similar busy status) when a previous
indication is still unconfirmed, so `bt_serial_tx()` returned `false` and the FAP aborted.

### Confirmation event path

- `airbridge_serial_service.c:136-143` converts `ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE`
  into `SerialServiceEventTypeDataSent` and invokes the serial service callback.
- `bt.c:235-242` previously always set `BT_RPC_EVENT_BUFF_SENT` for that event. The RPC
  thread consumed it; the FAP never saw it.

### Sentinel contract (least invasive forwarding)

- `bt.h` now documents `BtRawSerialCallback` with a sentinel: `data == NULL && len == 0`
  means "previous BLE Serial TX indication was confirmed by the central," NOT a data
  packet. Real data deliveries always have `len > 0`.
- `bt.c:235-242` forwards the confirmation only when the active profile is AirBridge and a
  raw serial callback is registered: `ret = bt_raw_serial_cb(NULL, 0, bt_raw_serial_ctx)`.
  The stock serial-profile RPC path is unchanged (it still gets `BT_RPC_EVENT_BUFF_SENT`).

### FAP pacing and stall handling

- `ble_raw_serial_callback()` now receives `AirbridgeApp*` as context (changed from the
  event queue). For `len == 0` it sets `app->stream_tx_confirmed = true` and returns
  `HID_VENDOR_PACKET_LEN`; it never queues the sentinel, so the bridge relay path stays
  byte-identical for real data.
- `app_start_stream()` initializes `stream_tx_confirmed = true` so the first header can be
  sent without waiting for a prior confirmation.
- `app_stream_step_ble()` (`pocket_airbridge.c:701-750`) sends exactly one fragment per
  confirmed window:
  - Header: send only when `stream_tx_confirmed`, then clear the flag and record the tick.
  - Each payload chunk: same pattern; the function early-returns while waiting.
- Stall detection (`app_ble_stream_stall_check()` at `:682-699`):
  - `BLE_STREAM_STALL_TIMEOUT_MS = 2000`.
  - `BLE_STREAM_STALL_MAX = 3` strikes.
  - On each timeout the strike counter increments and the timer resets; after three strikes
    the stream closes with a distinct `STREAM STALLED` error.
- Framing is unchanged: 8-byte header (`uint32_t total_len LE`, `uint32_t checksum LE`)
  followed by raw payload bytes up to 64 B per indication.

### Files changed

- `applications/services/bt/bt_service/bt.h`: `BtRawSerialCallback` docstring with sentinel
  contract.
- `applications/services/bt/bt_service/bt.c`: `bt_serial_event_callback()` DataSent branch
  forwards `NULL,0` to the FAP when AirBridge + raw callback are active.
- `applications_user/pocket_airbridge/pocket_airbridge.c`:
  - `ble_raw_serial_callback()` handles `len == 0` sentinel and uses `AirbridgeApp*` context.
  - `app_configure_usb()` passes `app` as raw-serial context.
  - Added `stream_tx_confirmed`, `stream_tx_strikes`, `stream_tx_sent_tick`.
  - `app_start_stream()` initializes TX-ready state.
  - New `app_ble_stream_stall_check()` and paced `app_stream_step_ble()`.

### Build verification

- `xcrun clang-format --dry-run --Werror` on `pocket_airbridge.c`, `bt.c`, and `bt.h` →
  exit 0 (clean).
- `./fbt fap_pocket_airbridge` → SDKCHK pass, APPCHK pass, FAP generated.
- Full `./fbt` → `bt.c` recompiled, firmware linked, HEX/BIN/DFU generated, exit 0 with
  API 90.0 up to date and `dist/f7-D` output. No `api_symbols.csv` changes were needed.

## 2026-07-21 — W16 BLE deploy streaming: TX NOTIFY + congestion-tolerant retries

### Why indication pacing was abandoned

With TX as `CHAR_PROP_INDICATE`, the 8-byte stream header reached Chrome, but the
`ACI_GATT_SERVER_CONFIRMATION` event never made it back to the FAP. The dispatcher routing
in `airbridge_serial_service.c` (`:136-143`), the `bt.c` sentinel forwarding (`:235-242`),
and the FAP handler were all verified correct; the confirmation simply does not arrive from
the host/Chrome side in this setup. Rather than keep debugging a fragile host-dependent
confirmation dependency, the AirBridge TX characteristic was switched to `CHAR_PROP_NOTIFY`,
the standard BLE-UART pattern, and the FAP now retries on transient congestion instead of
waiting for confirmations.

### Service changes

- `targets/f7/ble_glue/services/airbridge_serial_service.c`:
  - TX characteristic properties changed from `CHAR_PROP_READ | CHAR_PROP_INDICATE` to
    `CHAR_PROP_READ | CHAR_PROP_NOTIFY` (`:40`). RX, flow-control, and status are unchanged.
  - `ble_svc_airbridge_serial_update_tx()` (`:284-323`) now retries on
    `BLE_STATUS_INSUFFICIENT_RESOURCES` inside each `aci_gatt_update_char_value_ext` call:
    up to 100 retries with 1 ms delay, mirroring `ble_gatt_characteristic_update()` in
    `targets/f7/ble_glue/furi_ble/gatt.c:144-153`. Other error codes still fail immediately.

### FAP stream-step simplification

- Removed the indication-confirmation gate and the `stream_tx_confirmed` /
  `stream_tx_sent_tick` fields. `stream_tx_strikes` is now a per-chunk retry counter.
- `app_start_stream()` initializes `stream_tx_strikes = 0` only.
- `ble_raw_serial_callback()` no longer handles the `len == 0` sentinel; it keeps the
  `AirbridgeApp*` context and only queues real RX data. The `bt.c`/`bt.h` sentinel forwarding
  remains in place and is harmless for the bridge data path.
- `app_stream_step_ble()` (`pocket_airbridge.c:677-727`) now:
  - Sends the 8-byte header once per tick; on success clears strikes, on failure increments
    strikes and returns to retry.
  - Sends one ≤64 B payload chunk per tick; on success advances `stream_sent` and clears
    strikes, on failure increments strikes and returns to retry the same chunk.
  - After `BLE_STREAM_RETRY_MAX = 20` consecutive failures closes the stream with a distinct
    `STREAM STALLED` error. At ~10 ms per tick this gives ~200 ms of retries before giving up.
  - File read errors still produce `STREAM ERROR` immediately.
- Framing is byte-identical: 8-byte header (`uint32_t total_len LE`, `uint32_t checksum LE`)
  followed by raw payload bytes up to 64 B per notification.

### Files changed

- `targets/f7/ble_glue/services/airbridge_serial_service.c`: TX = NOTIFY, busy-retry in
  `ble_svc_airbridge_serial_update_tx()`.
- `applications_user/pocket_airbridge/pocket_airbridge.c`:
  - Constants: `BLE_STREAM_RETRY_MAX` replaces the old stall timeout/max pair.
  - Struct: removed `stream_tx_confirmed` and `stream_tx_sent_tick`; kept
    `stream_tx_strikes`.
  - `ble_raw_serial_callback()`: dropped sentinel branch.
  - `app_start_stream()`: simplified TX-ready init.
  - Removed `app_ble_stream_stall_check()`; rewrote `app_stream_step_ble()` for notify +
    retry semantics.

### Build verification

- `xcrun clang-format --dry-run --Werror` on `pocket_airbridge.c` and
  `airbridge_serial_service.c` → exit 0 (clean).
- `./fbt fap_pocket_airbridge` → SDKCHK pass, APPCHK pass, FAP generated.
- Full `./fbt` → `airbridge_serial_service.c` recompiled, firmware linked, HEX/BIN/DFU
  generated, exit 0 with API 90.0 up to date and `dist/f7-D` output. No
  `api_symbols.csv` changes were needed.

## 2026-07-21 — BLE bootstrap landing diagnostics

- `web/bootstrap-ble.js` now renders a timestamped `<pre>` and logs the picker, GATT,
  discovered characteristics, notification start, `0x42` write, indication progress,
  stream completion, checksum outcome, disconnect events, app boot, and handled error
  messages. The picker options, UUIDs, stream framing, and disconnect-before-write order
  remain unchanged.
- The typed payload is pure ASCII and measures 2,294 characters (`wc -m`).
- `BOOTSTRAP_MAX_BYTES` removed; `app->bootstrap` is now a dynamically allocated
  `char*` sized to the file, loaded with `malloc(file_size + 1)`, and freed on teardown
  and before reload. `./fbt fap_pocket_airbridge` APPCHK pass.

## 2026-07-22 — BLE discovery uses HP name prefix

- Browser BLE discovery now uses `filters: [{ namePrefix: 'HP' }]` in
  `web/bootstrap-ble.js`, `WebBluetoothAdapter`, and `web/receiver.html`.
  `web/chat-ble.html` inherits the filter through `WebBluetoothAdapter`.
  The `optionalServices` list remains exactly the AirBridge serial UUID.
- The literal `'HP'` matches both the stack-truncated advertised name
  (`HP Wireless Keyb`) and the full configured name (`HP Wireless Keyboard`).
- `scripts/ble_qa_runbook.md` updated to exercise the prefix filter.
- `web/bootstrap-ble.js` is pure ASCII, node --check passes, and measures
  2,300 characters (`wc -m`).
2026-07-22 — Reverted `web/airbridge-transports.js` and `web/receiver.html` WebBluetooth discovery to `acceptAllDevices: true` plus the AirBridge serial UUID in `optionalServices`; `web/bootstrap-ble.js` keeps `namePrefix: 'HP'` so the typed bootstrap still narrows the picker.
## 2026-07-21 — USB OUT regression BISECTED (old FAP works)
- Bisect: old FAP (docs/firmware/ copy, morning build) on NEW firmware → U->B=2, USB OUT WORKS. Regression is FAP-side, introduced today.
- Playwright Chromium needed Input Monitoring granted + BROWSER RELAUNCH (grant applies at relaunch only). After that, Playwright writes work — Playwright is now viable for BOTH chat sides.
- Prime suspect: W5 startup order — app_configure_ble (bt_profile_start → furi_hal_bt_reinit → core2 reset) runs BEFORE app_configure_usb. Old FAP had no BLE install at startup. USB IN works, USB OUT (vendor EP 0x03) dead → suggests BLE reinit disturbs USB EP state or the ordering leaves the vendor OUT path misconfigured.
- Fix direction: USB apply FIRST, then BLE install (or delay BLE install until USB is enumerated); verify USB OUT after.
- PROCESS RULES (user directives): (1) Playwright window is the standing test surface (both chat tabs + example.com for deploy); (2) before any user-interaction gate, wait ~5s and CHECK whether the expected state already happened; (3) regression rule already recorded (always retest chat/file transfer after changes).

## 2026-07-22 — USB OUT regression fix: USB apply before BLE install

### Change

- `applications_user/pocket_airbridge/pocket_airbridge.c` deferred startup block now calls
  `app_configure_usb()` before `app_configure_ble()`. USB failure still shows
  `USB CONFIG ERROR`; BLE failure still shows `BLE CONFIG ERROR` and exits the app.

### Reasoning

- `bt_profile_start()` queues a profile change that ultimately calls
  `furi_hal_bt_change_app()` → `furi_hal_bt_reinit()` (core2 reset, `hci_reset()`, bus
  disable/enable for HSEM/IPCC/AES2/PKA/CRC). While `furi_hal_bt_reinit()` does not touch
  USB registers directly, running that long, power/clock-disturbing sequence before the
  composite USB profile is applied apparently leaves the vendor OUT endpoint unreachable
  (USB IN kept working because it is device-driven; OUT depends on host-side descriptor
  matching the endpoints the device actually exposes).
- Applying USB first lets the host enumerate the composite HID+vendor descriptor while the
  device is still in its startup/default power state, before any BLE-side core reset
  activity can shift clocks or delay enumeration. This mirrors the old working FAP that had
  no BLE install at startup.

### Sanity check of USB vendor path

- `app_configure_usb()` installs `usb_event_callback` via `furi_hal_hid_vendor_set_callback()`
  and the raw serial callback; unchanged.
- `usb_event_callback()` posts `HidVendorRequest` data as `EVENT_TYPE_RELAY` with
  `to_ble = true`.
- `app_handle_relay()` increments `chunks_usb_to_ble` and forwards via `bt_serial_tx()`;
  unchanged.
- No other today changes touched these paths. Verdict: SAFE.

### Remaining risk

- If BLE reinit still disturbs an already-enumerated USB bus, a short `furi_delay_ms()`
  after `app_configure_usb()` (or waiting for the host to settle) may be needed. The minimal
  first fix is ordering only; hardware retest will confirm.

### Build verification

- `xcrun clang-format --dry-run --Werror applications_user/pocket_airbridge/pocket_airbridge.c`
  → exit 0 (clean).
- `./fbt fap_pocket_airbridge` → SDKCHK pass, APPCHK pass, FAP generated.
- Full `./fbt` → exit 0, API 90.0 up to date, `dist/f7-D` generated.

## 2026-07-22 — WebHID HP composite interface selection

- Hardware diagnosis: the HP composite exposes two WebHID devices with the same
  VID/PID. Interface 0 is the keyboard collection (`usagePage: 0x0001`,
  `usage: 0x0006`) and has no output reports because WebHID blocklists keyboard
  writes; interface 1 is the AirBridge vendor collection (`usagePage: 0xFF00`,
  `usage: 0x0001`) with 64-byte input/output report 0.
- The old cached-device lookup matched VID/PID only and selected the first result.
  `getDevices()` interface ordering varies, explaining the intermittent
  `sendReport` `NotAllowedError`; host reboots only happened to change that
  ordering and were not a fix.
- WebHID picker filters now include `usagePage: 0xFF00`, and cached selection
  prefers the matching vendor collection, warning only when it must fall back to
  the old first-pinned-device behavior.
## 2026-07-22 — CHAT REGRESSION PASS (both root causes fixed on hardware)
- FIX 1 (WebHID selection): airbridge-transports.js now selects the vendor (0xFF00) interface; all 5 HID chooser filters carry usagePage:0xFF00. Root cause of the day's "USB OUT dead" phantom: Chrome's getDevices() returns keyboard AND vendor interfaces with the same VID/PID; VID/PID-only .find() grabbed the blocklisted keyboard interface (NotAllowedError on writes); ordering variance across enumerations explained the intermittency ("host state" theory was wrong; reboot "fix" was ordering coincidence).
- FIX 2 (update_type): aci_gatt_update_char_value_ext with 0x02=SEND_INDICATION on a CHAR_PROP_NOTIFY characteristic emits nothing (TXERR 0, zero notifications at all clients). Changed to 0x01=SEND_NOTIFICATION. Root cause of zero-notification deploy streaming + BLE chat silence.
- VERIFIED: chat-usb → chat-ble text (sent+ACKed, hash logged), chat-ble → chat-usb text (received, hash verified). DROP 0, TXERR 0.
- Host-side confirmations along the way: hidapi (terminal) writes worked throughout (V=1, U->B=1) — device+descriptor were always healthy.
## 2026-07-22 — FULL REGRESSION PASS (post-fix)
- Chat text USB->BLE: sent + ACKed. Chat text BLE->USB: received + hash verified.
- File USB->BLE (510 B): SHA-256 verified. File BLE->USB (248 B): hash verified.
- DROP 0, TXERR 0 throughout. Root causes fixed: WebHID vendor-interface selection (usagePage 0xFF00) + GATT_CHAR_UPDATE_SEND_NOTIFICATION (0x01) on the NOTIFY char.

## 2026-07-22 — Deployed USB bundle identity binding

- `tools/build_bundle.py` previously stripped the identity import from
  `airbridge-transports.js` without emitting `web/airbridge-identity.js`, so the deployed
  non-module page threw `identity is not defined` on Connect.
- `inline_module(path, None)` now emits a de-exported module directly. `main()` emits the
  generated `const identity = Object.freeze(...)` before the transports IIFE, preserving the
  existing import/export stripping regexes and module-wrapper behavior for other modules.
- Rebuild verification: `dist/app-usb.html` is 71,391 bytes; `grep -c "const identity"` is
  `1`; no `identity is not defined`, `import `, or `type="module"` text remains; and the
  extracted script passes `node --check /dev/stdin`.
- The inlined object exposes `BLE_NAME`, `BLE_MFG_COMPANY_ID`, and all three serial UUIDs.
  The current transports implementation directly reads the service/TX/RX UUIDs; the name and
  manufacturer fields remain reachable through that same top-level `identity` binding.

## 2026-07-22 — BLE bootstrap same-origin boot and compression

- `web/bootstrap-ble.js` no longer uses `document.open()` / `document.write()` / `document.close()`.
  It parses the streamed HTML with `DOMParser`, replaces the current `documentElement` without
  navigating, snapshots the inert parsed scripts, and recreates each `<script>` with copied
  attributes and `textContent`. Recreating the nodes is required because parsed-and-inserted
  scripts do not execute.
- The primary path preserves the existing `https://example.com` origin and therefore its secure
  context; the existing `blob:` navigation remains only as the boot catch fallback. The inline
  bootstrap comment records this `isSecureContext === true` invariant.
- Chromium proof: before/after origin was `https://example.com`, before/after secure context was
  `true`, the revived `dist/app-usb.html` script count was `1`, and its startup log ran. A second
  `https://example.com/?mock=1` run clicked the revived bundle's Connect button and reached
  `status=Connected`, `device=Mock USB`, `secure=true`.
- Payload reduced from 2,300 to 1,874 ASCII characters (426 fewer). Reused single-letter
  literals cover the GATT event names, service UUID, listener/characteristic/notification method
  names, and retry strings. `node --check` and the static wire-contract check pass; discovery
  remains `namePrefix: 'HP'`, the request is still `0x42`, header reads remain 8 bytes, checksum
  verification and disconnect-before-boot remain unchanged.

## 2026-07-22 — Deploy bundle transport routing

- `tools/build_bundle.py` now builds both self-contained non-module deploy pages from the
  matching chat endpoints: `dist/app-usb.html` from `chat-usb.html` with `WebHIDAdapter`, and
  `dist/app-ble.html` from `chat-ble.html` with `WebBluetoothAdapter`. Each emits the generated
  top-level `const identity` before the inlined transports module.
- Build output: `dist/app-usb.html: 71391 bytes`; `dist/app-ble.html: 72441 bytes`; and
  `web/bootstrap.js: 1200 chars`. Generated-page greps found exactly one `const identity` in
  each output, no `import ` or `type="module"` syntax in either, and identity before the matching
  adapter class. The final structural assertion also passed for both artifacts.
- `app_start_stream()` now selects `APP_DATA_PATH("app-ble.html")` when
  `typing_transport == AirbridgeTypingTransportBle`; all other transports select
  `APP_DATA_PATH("app-usb.html")`. The stream length/checksum header and payload framing were
  not changed.
- Verification: `xcrun clang-format --dry-run --Werror applications_user/pocket_airbridge/pocket_airbridge.c`
  passed; `./fbt fap_pocket_airbridge` compiled the FAP and passed SDKCHK, FAP, FASTFAP, and
  APPCHK with API version 90.0 up to date. No deploy, flash, hardware interaction, or commit was
  performed.
## 2026-07-22 — BLE DEPLOY E2E: FULL SUCCESS
- Typed bootstrap (hardened pacing 40/60+settle, zero drops) → landing page → Connect → paired 'HP 725 K+M' → streamed app-ble.html over BLE → BLE chat booted on example.com (transport-routed bundle) → deployed app connected to Flipper over BLE.
- Permanent browser via CDP (chrome --remote-debugging-port=9222, MCP attaches, no MCP-owned lifecycle) solved the auto-close problem permanently.
- Root causes fixed this session: WebHID vendor-interface selection (usagePage 0xFF00), GATT_CHAR_UPDATE_SEND_NOTIFICATION (0x01), pump pre-first-connection only, bootstrap document.write→DOMParser+script-revival, bundle identity inlining, transport-routed bundles (app-ble.html for BLE, app-usb.html for USB).
## 2026-07-22 — COMPLETE BLE DEPLOY E2E + BRIDGE LOOP VERIFIED
- Deployed BLE app (on example.com) received 'usb-to-deployed-ble' (19 B) from chat-usb (WebHID vendor) through the bridge. The whole system works: BLE deploy typing → streaming → BLE chat boot → live two-way bridge.
- Plan W11 marked complete. Remaining: W7 hardware captures, W9 delivery, Final Wave F1-F4.

## 2026-07-22 — W9 delivery bundle regeneration

- Firmware publication base is pristine upstream `c9ab2b68`; the committed AirBridge BLE implementation is `31f929b2`. The clean patch bundle is split so `airbridge-firmware.patch` excludes `targets/f7/api_symbols.csv` and `api-symbols-additions.patch` contains that file exclusively.
- The documented FAP source now mirrors `applications_user/pocket_airbridge/`; its icon was already byte-identical. Documentation records config-driven BLE name/MAC/DIS, HIDS plus AirBridge serial, per-connection numeric comparison with bonding disabled, on-air generated UUIDs, and USB/BLE transport-routed deploy bundles.

## 2026-07-22 — F3 identity-leak remediation

- A paired client writing `0x00` to the AirBridge serial status characteristic can still emit
  `SerialServiceEventTypesBleResetRequest`, but `bt_serial_event_callback()` now checks the
  active profile first. While AirBridge is active it logs and ignores the request, so it never
  queues `BtMessageTypeSetProfile` for `ble_profile_serial` and cannot restore the stock
  Flipper name, MAC, or `fe60`-family service while the FAP remains running.
- The default serial profile keeps the existing mobile-app reset behavior: a reset request on
  any non-AirBridge profile still queues the unchanged `ble_profile_serial` restore.
- The AirBridge NULL-params fallback now uses HP's little-endian manufacturer company ID bytes
  `{0x65, 0x00}` (`0x0065`).
## 2026-07-22 — F3 downgrade protection hardware-verified
- Hostile test: paired GATT client wrote 0x00000000 to the airbridge RPC-status characteristic (downgrade attempt). Device stayed 'HP 725 K+M' (macOS showed it connected, VID/PID 03F0:5341); NO 'Flipper Luwot' name, NO fe60 stock UUIDs on air. bt.c now ignores BleResetRequest while the airbridge profile is active. F3 leak closed on hardware.
- Plan updated to accepted reality: bonding OFF (Hard Constraint 4 / Decision 4), discovery = acceptAllDevices + namePrefix 'HP' (Decision 8), USB 0xFF00 vendor interface = accepted residual (Decision 12).

## 2026-07-22 — F2/F3 final-review corrections

- `app_stream_step_ble()` now reads one ≤64-byte payload chunk and retries `bt_serial_tx()` on
  that same buffer until it succeeds or reaches `BLE_STREAM_RETRY_MAX`. It increments
  `stream_sent` only after success, so a transient BLE TX failure cannot advance the source
  file cursor past unsent bytes. The 8-byte LE length/checksum header and raw payload framing
  are unchanged.
- Cached WebHID selection now accepts only a pinned VID/PID device that exposes the AirBridge
  `0xFF00` collection; there is no cached keyboard/VID-PID fallback. The intentional
  service-UUID probe catch is documented.
- Removed the obsolete `web/sender.html` and `web/receiver.html` pages. README and the firmware
  guide now state that they were removed and direct transfers to the chat pages; USB chat copy
  calls the physical link AirBridge.
- Verification: `xcrun clang-format --dry-run --Werror` and `./fbt fap_pocket_airbridge` passed;
  `node --check web/airbridge-transports.js` passed; Playwright protocol harness passed 17/17
  with no console errors. No commit was made.

## 2026-07-22 — F3 firmware bundle snapshot

- Firmware remained at committed AirBridge profile `31f929b2`; the F3 downgrade guard in
  `bt.c` and the `{0x65, 0x00}` AirBridge fallback manufacturer data were uncommitted working-tree
  changes. A clean future bundle should follow an F3 commit.
- `docs/firmware/airbridge-firmware.patch` was regenerated as the explicit
  `c9ab2b68`-to-current-worktree snapshot, excluding the separately bundled
  `targets/f7/api_symbols.csv` and FAP directory. It contains the `BleResetRequest` ignore
  guard and corrected fallback bytes, matches the generated diff exactly, and passes
  `git apply --check` on a detached pristine `c9ab2b68` worktree.
- `api-symbols-additions.patch` exactly matches the base-to-current API CSV diff; F3 did not
  change that file. The bundled `pocket_airbridge/` directory exactly matches the current FAP
  source, so no FAP re-sync was needed.
## 2026-07-22 — USB DEPLOY E2E PASS (final-wave HC0 evidence)
- USB deploy: Flipper typed bootstrap.js (UP, ~1,200 chars) into localhost:8081 console → landing page → Connect (WebHID vendor) → streamed cleaned app-usb.html (zero 'Flipper' in bundle) → USB chat booted ('PC-A chat endpoint over WebHID … through the AirBridge', button 'Connect AirBridge (USB)') → deployed app connected to Flipper and sent 'deployed-usb-app-test'. Full loop PASS with cleaned bundle.
## 2026-07-22 — STOCK RESTORATION ON EXIT (final-wave HC2 evidence, part 1)
- After FAP exit (BACK), scan shows STOCK TARGET FOUND: 'Flipper Luwot' with stock adv UUID 0x3082 (vs airbridge 'HP 725 K+M' + 0x1812). GATT enum on stock device: SVC 0000180a (DIS), 0000180f (Battery), 8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000 (fe60-family serial service). Runtime-only impersonation with full stock restoration on exit CONFIRMED.
## 2026-07-22 — STOCK RESTORATION ON REBOOT (final-wave HC2 evidence, part 2)
- After full Flipper reboot (BACK+LEFT), scan shows 'Flipper Luwot' with stock adv UUID 0x3082. No impersonation until the FAP runs — runtime-only confirmed on both exit AND reboot. HC2 fully evidenced.

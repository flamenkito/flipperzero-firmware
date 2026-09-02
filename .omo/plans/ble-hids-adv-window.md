# Plan: BLE HIDS Advertising Window + getDevices Reconnect

## Goal

Kill the unpair/pair dance in the BLE deploy flow (and BLE chat connect) on macOS.
Root cause (investigated 2026-07-23, user-approved scope A+B): the AirBridge profile
advertises HIDS `0x1812` permanently; macOS claims any paired device that advertises
HIDS and auto-reconnects to it, which (a) empties the Web Bluetooth picker (single-link
peripheral stops advertising while macOS holds the link) and (b) drops connect attempts.
Every pairing — System Settings or Chrome — re-registers the keyboard.

- **Fix A (firmware)**: advertise HIDS only during the BLE-deploy typing window;
  advertise name+appearance only at all other times (serial UUID is not in adv anyway).
- **Fix B (web)**: `getDevices()`-first reconnect in the BLE transport adapter and the
  BLE bootstrap, so post-`document.write` and repeat connects skip the picker entirely.

## Investigation evidence (ground truth, do not re-litigate)

- `airbridge_profile.c` GapConfig: adv packet = complete local name + 16-bit HIDS UUID;
  scan response = HP mfg data only; 128-bit serial UUID advertised nowhere.
- `gap.c`: adv data baked at `gap_init`; `aci_gap_update_adv_data` exists in the stack
  but is never called; `furi_hal_bt_start_advertising()` is a no-op unless GapStateIdle;
  advertising stops on connection (single-link state machine despite CFG_BLE_NUM_LINK=2).
- `furi_hal_bt_start_advertising()` reads `gap_get_state()` directly (no queue hop), so a
  new `furi_hal_bt_set_adv_hids()` may also be called from any thread.
- Web: `requestDevice` picker requires FRESH advertising (WebBluetoothCG #511);
  `getDevices()` is origin-scoped, persists through `document.write` on the same origin,
  and `gatt.connect()` on a granted device works without advertising on macOS
  (CoreBluetooth connects known peripherals by address). Requires Chrome flags
  `#enable-experimental-web-platform-features` (+ `#enable-web-bluetooth-new-permissions-backend`
  for cross-restart persistence). Joypad-ai proves the HID+WebBluetooth pattern on macOS
  (verify the custom service after connect; a bare gatt.connect can succeed while macOS HID
  holds the device yet service discovery fails — retry with backoff).

## Tasks

- [x] **W1: Firmware adv-swap API** — in `~/projects/flipperzero-firmware`: add
  `void furi_hal_bt_set_adv_hids(bool enable)` to gap glue (mutate
  `gap->service.adv_svc_uuid` between `HUMAN_INTERFACE_DEVICE_SERVICE_UUID`/UUID_TYPE_16
  and none/UUID_TYPE_NONE; if currently advertising, stop + re-set discoverable + start).
  Declare in `furi_hal_bt.h`, export in `targets/f7/api_symbols.csv` (`+` entry).
  Full `./fbt` build must pass.
- [x] **W2: FAP wiring** — `applications_user/pocket_airbridge/pocket_airbridge.c`:
  HIDS adv OFF at Bridge boot; ON only when BLE typing actually starts
  (`AirbridgeScreenTyping` + BLE transport, before first key); OFF when typing ends
  (success/abort/error) and on app exit. Start advertising after each swap (no-op unless
  idle). Guard: swap only when `furi_hal_bt_is_connected()` is false (mid-link swap is
  pointless; advertising is already stopped while connected).
- [x] **W3: Web getDevices reconnect** — `web/airbridge-transports.js`
  (`WebBluetoothAdapter.connect`): try `navigator.bluetooth.getDevices?.()` first, match
  by configured identity name, `gatt.connect()`, verify serial service via existing
  `findSerialService()`, retry with backoff (3×, 500 ms) for the macOS-HID-holds-link
  service-discovery failure, then fall back to the current `requestDevice` picker.
  `web/bootstrap-ble.js`: same getDevices-first logic (repeat deploys on same origin skip
  the picker). Rebuild bundles (`tools/build_bundle.py`), deploy web assets to SD.
- [x] **W3b: Firmware bonding ON + serial UUID in adv** — user directive 2026-07-23
  ("let it bond, as usual ble device" + full bonding-model brief): `airbridge_profile.c`
  `bonding_mode = true` (keep MITM numeric-comparison for first pairing); bonds must
  persist across app sessions/reboots/firmware updates via the stock key storage (verify
  the per-session profile install/uninstall does not wipe them); add the 128-bit AirBridge
  serial service UUID to the ADVERTISING packet (move full name to scan response with mfg
  data — budget: flags 3 + UUID 18 = 21 ≤ 31 adv; name 12 + mfg 6 = 18 ≤ 31 scan-rsp);
  HIDS adv windowing stays (HIDS UUID toggles in ADDITION to the always-present serial
  UUID); same advertised UUIDs before/after bonding; no whitelist; connectable adv always.
- [x] **W3c: Web exact filters + reconnect backoff** — `web/airbridge-transports.js`:
  `requestDevice` filters become `[{services: [identity.SERIAL_SERVICE_UUID]}]` (exact,
  no acceptAllDevices primary; keep a graceful fallback). `web/bootstrap-ble.js`: move the
  serial UUID from optionalServices into filters (minimal bytes). Add bounded
  exponential-backoff auto-reconnect on `gattserverdisconnected` ([500,1000,2000,4000,
  8000] ms, only for non-user-initiated drops) to `WebBluetoothAdapter`. Rebuild bundles,
  redeploy web assets to SD.
- [x] **W4: BLE deploy E2E — the no-dance run (bonded)** — with bonding ON: pair ONCE
  EVER (first deploy typing, macOS settings during HIDS-adv window) → bootstrap Connect
  via services-filter picker (or getDevices) → macOS reuses bond (no code on reconnects)
  → stream → app-ble.html boots → deployed chat connects via getDevices, no dance.
  Verify with ble_qa_scan: serial UUID always in adv, HIDS only in window. Bridge BLE
  chat connect clean. ALL of this in the orchestrator-driven CDP window (test-surface
  discipline — the deployed app must stay observable via CDP).
- [x] **W5: Regression** — bridge text both directions, attachment SHA-256, USB deploy
  E2E, protocol harness 17/17, exit identity restore. Plus: verify HIDS-adv-off state
  does not break Web Bluetooth discovery in any flow (popup + getDevices paths).
- [x] **W6: Publish + docs** — REGENERATE `firmware/airbridge-firmware.patch` +
  `firmware/api-symbols-additions.patch` (firmware changed!), verify `git apply --check`
  on pristine c9ab2b68, update firmware README regen note, copy FAP source to
  `firmware/pocket_airbridge/`, update `docs/firmware-guide.md` (HIDS adv window
  behavior, chrome flags for getDevices, removal of unpair/pair workaround notes),
  README.md flags note, commit flipper-hid + firmware repo (firmware repo HAS tracked
  changes this time: gap glue + api_symbols.csv).
- [x] **W7: Final Verification Wave** — F1: goal/constraint audit (impersonation intact:
  name/mfg/DIS unchanged by adv swap; consent model preserved: per-connection code still
  required); F2: code quality review of gap patch + FAP wiring + web adapter (oracle);
  F3: regression evidence audit (every W4/W5 claim has an artifact); F4:
  plan-vs-implementation diff.

## Status: COMPLETE (9/9) — closed 2026-07-24. Final Wave: F1 APPROVE | F2 APPROVE | F3 APPROVE | F4 APPROVE (after one remediation round: advertise_hids init, gap_int.h, GATT staleness teardown, bootstrap per-click state, evidence block, patch regen).

## Non-goals

- No bonding change (stays disabled; per-connection numeric code is the consent model).
- No HIDS removal from the GATT table (typing needs it; only the ADVERTISEMENT is windowed).
- No bridge-screen or protocol changes.
- `watchAdvertisements`/`requestLEScan` not used (getDevices + picker fallback suffices).

## Open follow-up (not in this plan)

- ISSUE-5 BACK-exit freeze recurred twice on 2026-07-23 (Bridge exit wedge; user recovers
  via LEFT+BACK). Separate investigation needed.
- Bridge-mode squatter-kick (W5 finding, 2026-07-23): when macOS holds the bonded keyboard
  link in BRIDGE mode, chat-ble connect can fail (getDevices service-discovery fails 3×,
  picker empty) and needed an app restart. Deploy Waiting is covered by the 15 s kick;
  Bridge mode is not — a Bridge-mode kick would need a "no serial RX within N seconds"
  heuristic (chat clients may sit silently; macOS HID never writes vendor RX). Needs a
  design pass to avoid kicking legitimate read-only chat users.
- Spontaneous FAP exit observed once at W5 regression start (no repro since; app was
  stable 60 s+ after relaunch and through the full regression). Watch for recurrence.

# HID Composite Profile Deploy: Hard-Won Operational Lessons

Hardware-verified during the Pocket AirBridge typed-bootstrap sessions of
2026-07-20 and 2026-07-21. Each item below cost real debugging time, hardware
crashes, or physical device resets. Read this before building or deploying any
Flipper app that enumerates a composite USB HID profile (keyboard + vendor
collection) or types a bootstrap payload into a browser.

Pocket AirBridge is maintained in the local custom firmware and apps repository
`/Users/asutov/projects/flipperzero-firmware`. Its FAP source is
`applications_user/pocket_airbridge/`, and its web/deploy assets are in
`airbridge/`. Work from those in-tree sources. There is no patch bundle or
patch-application workflow.

## 1. Composite-to-composite reconfiguration is fatal

Switching a custom HID profile while it is already active wedges the Flipper
USB stack. Calling `furi_hal_usb_set_config` from one composite profile to
another leaves the device enumerated but dead: endpoints accept nothing, or
the whole stack dies and needs a physical reset.

Design rule: ONE profile, applied once. There is no runtime profile switching
on this USB stack. Pick the profile at app start (from config) and hold it for
the app's entire lifetime.

## 2. Bidirectional endpoint numbers must not share an index

A vendor IN endpoint `0x82` and an OUT endpoint `0x02` share endpoint index 2.
`usbd_ep_config` rewrites the whole endpoint register on each call, so
configuring OUT after IN silently disables IN.

Use distinct indices across all endpoints, for example: keyboard IN `0x81`,
vendor IN `0x82`, vendor OUT `0x03`.

## 3. Always-on impersonation pattern

Apply the USB profile on the first event-loop iteration after GUI init. Never
apply it synchronously inside the loader's open call; doing so crashes. Hold
the profile for the app's whole lifetime and restore the previous USB mode
only on full app exit.

Consequence: while the app runs there is no CDC, so there is no serial port.
`storage.py` and `runfap.py` fail with `Failed to resolve port`. Deploy files
to the SD card BEFORE launching the app, or after exiting it.

## 4. FAP launch via CLI

```
loader open "/ext/apps/name.fap"
```

Use the bare path. `loader open "Apps" <path>` opens the app browser instead
of launching the FAP, and `loader open` by registered name only works for
built-in apps, not external FAPs.

`loader close` may refuse for view_port-style apps with a "has to be closed
manually" error. Gate a physical app exit on the user instead of retrying.

## 5. APP_DATA_PATH resolves to the appid directory

The SDK macro `APP_DATA_PATH(...)` expands to `/ext/apps_data/<appid>/` where
`<appid>` comes from `application.fam`. Deploy SD files there, not to a
hand-named directory. For the Pocket AirBridge app (appid `pocket_airbridge`)
that is `/ext/apps_data/pocket_airbridge/`.

## 6. Stale FAP copies in category folders

The GUI Apps menu shows category folders (Apps -> USB/...). A stale `.fap`
left in `/ext/apps/<Category>/` hangs or misbehaves while
`/ext/apps/<name>.fap` is current. The classic symptom is "GUI launch hangs
but CLI launch works".

Before every deploy, check for duplicates first. Exactly one line must remain,
the canonical `/ext/apps/USB/pocket_airbridge.fap`; remove all strays before
uploading:

```
python3 scripts/storage.py list /ext/apps
```

## 7. Stuck detection and recovery

`airbridge/tools/flipper_alive.py` reports ALIVE / HUNG /
ABSENT by probing the serial CLI for the `>:` prompt. Port present but silent
(the device enumerates at `/dev/cu.usbmodem*` but never answers) means hung
firmware: the firmware is dead but USB CDC still enumerates.

Recovery is a PHYSICAL reset. There is no reliable programmatic reset; do not
attempt DTR-toggle resets from software. Before asking the user to touch the
device, gate them with a loud alert:

```
afplay /System/Library/Sounds/Funk.aiff && sleep 1 && afplay /System/Library/Sounds/Funk.aiff
```

## 8. macOS Input Monitoring permission

A composite profile with a keyboard collection makes the device
keyboard-class. On macOS, WebHID from any browser binary then requires Input
Monitoring permission per app (System Settings -> Privacy & Security -> Input
Monitoring). Playwright's Chromium needs its own grant separate from the
user's real Chrome. Windows has no equivalent.

## 9. HID keyboard deploy flow gotchas

- Check `ep_write` return values. A failed write with a held semaphore is a
  silent stall: no error surfaces, the transfer just never progresses.
- A bootstrap that `document.write`s a new document must close its HID handle
  first. If it doesn't, the handle survives the document replacement as a
  zombie, and the injected app's opens receive no reports (they keep routing
  to the dead handle). Close before `document.open()/write()/close()`.

## 10. usbd_ep_read arms RX

The host's first OUT write fails if the OUT endpoint was never read-armed.
Call `usbd_ep_read` to arm reception before expecting host data; the
`get_request` path re-arms after each received report.

## 11. BLE profile install before USB apply kills vendor OUT

`bt_profile_start` (→ `furi_hal_bt_reinit` → `core2_reinit`) at FAP startup
before the USB apply can break the USB vendor OUT path: IN still works, OUT is
dead. Apply USB first, then BLE.

## 12. WebHID must select the vendor collection, not just VID/PID

The HP composite enumerates keyboard and vendor WebHID devices with the same
VID/PID. The keyboard collection is `usagePage: 0x0001`, `usage: 0x0006` and
is blocked for WebHID writes; AirBridge is `usagePage: 0xFF00`, `usage: 0x0001`
with report-ID-0 64-byte input/output reports. Include `usagePage: 0xFF00` in
chooser filters and prefer that collection when resolving cached devices.

## 13. Match GATT update type to the TX characteristic

`aci_gatt_update_char_value_ext()` silently drops an indication sent through a
`CHAR_PROP_NOTIFY` characteristic. For AirBridge TX notify use
`GATT_CHAR_UPDATE_SEND_NOTIFICATION` (`0x01`); use
`GATT_CHAR_UPDATE_SEND_INDICATION` (`0x02`) only if the characteristic itself
is declared `CHAR_PROP_INDICATE`.

## 14. Retain persistent bonds for the BLE profile

macOS can keep a bonded HIDS keyboard connection open, suppressing advertising
and preventing Web Bluetooth discovery. The live AirBridge profile nevertheless
uses `.bonding_mode = true`: one identity serves both typed bootstrap and browser
data, so the typing bond must remain valid for the later browser connection.
Wiping the Flipper-side bond leaves the host with dead keys and makes subsequent
connections fail. Keep numeric-comparison pairing and persistent bonds; use the
15-second unsubscribed-link squatter watchdog rather than removing bonds to
recover a daemon-held link.

## 15. Recover advertising after every central disconnect

Do not assume GAP's automatic advertising resumption always leaves the device
discoverable. In the FAP's BLE status callback, call
`furi_hal_bt_start_advertising()` after a disconnect. It is a no-op unless GAP
is idle and fixes the Bridge-mode advertising wedge.

## 16. Pace BLE bootstrap typing for notifications

BLE HIDS input reports are notifications and may be silently dropped under
macOS congestion. For long ASCII bootstraps use a 40 ms key-press delay, a
60 ms release delay, and a further 10 ms after modified keys; retain bounded
report retries for controller-level failures. USB's 12/18 ms cadence is not a
safe BLE default.

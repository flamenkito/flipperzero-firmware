# Firmware Build, Flash, and FAP Deployment

## Repository

```text
Path:   /Users/asutov/projects/flipperzero-firmware
Branch: pocket-airbridge
```

Do not assume the working tree is clean; this custom firmware and apps repository
is shared with the user and other agents.

Pocket AirBridge product assets are in `airbridge/`; the canonical FAP source is
`applications_user/pocket_airbridge/`. Firmware, FAP, and web assets are all
in-tree. Do not use a second checkout, patch bundle, or patch-application step.

## Build and flash

Core HAL, BLE service, GAP, or API-symbol changes require a firmware flash:

```bash
./fbt build APPSRC=applications_user/pocket_airbridge
./fbt flash_usb
```

`flash_usb` is the normal USB-only path. Manual DFU is recovery-only. The
serial device disappears during update and normally returns after 30–60
seconds.

Before every user or hardware gate, play:

```bash
afplay /System/Library/Sounds/Funk.aiff && sleep 1 && afplay /System/Library/Sounds/Funk.aiff
```

Then use the `question` tool. Never poll while waiting for a button press,
unlock, plug/unplug, picker selection, pairing confirmation, or screen state.

## Deploy artifacts before launch

The active AirBridge USB profile has no CDC interface, so deploy while the FAP
is stopped:

- FAP: `/ext/apps/USB/pocket_airbridge.fap`
- Bootstrap source: `airbridge/web/bootstrap.js`, deployed to `/ext/apps_data/pocket_airbridge/bootstrap.js`
- BLE bootstrap source: `airbridge/web/bootstrap-ble.js`, deployed to `/ext/apps_data/pocket_airbridge/bootstrap-ble.js`
- Bundle source: `airbridge/dist/app-usb.html.gz`, deployed to `/ext/apps_data/pocket_airbridge/app-usb.html.gz`
- BLE bundle source: `airbridge/dist/app-ble.html.gz`, deployed to `/ext/apps_data/pocket_airbridge/app-ble.html.gz`

Use `scripts/storage.py send -f`, verify sizes with `storage.py size`, then use
`scripts/runfap.py`. A final `Device not configured` error is expected when the
launched FAP intentionally replaces CDC with HID.

Before each upload, run `python3 scripts/storage.py -p <port> list /ext/apps |
grep -i airbridge`. It must report exactly one copy at
`/ext/apps/USB/pocket_airbridge.fap`; remove every stray copy before deploying.

## Browser pages

Serve the in-tree web assets with:

```bash
python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
```

Open `http://127.0.0.1:8081/chat-usb.html`,
`http://127.0.0.1:8081/chat-ble.html`, or the protocol harness at
`http://127.0.0.1:8081/protocol-harness.html`. There is no separate product
checkout, patch bundle, or patch-application step.

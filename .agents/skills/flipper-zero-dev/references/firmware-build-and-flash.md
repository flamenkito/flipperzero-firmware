# Firmware Build, Flash, and FAP Deployment

## Repository

```text
Path:   ~/projects/flipperzero-firmware
Remote: git@github.com:flamenkito/flipperzero-firmware.git
Branch: pocket-airbridge
```

Do not assume the working tree is clean; the project and firmware repositories
are shared with the user and other agents.

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
- Bootstrap: `/ext/apps_data/pocket_airbridge/bootstrap.js`
- Bundle: `/ext/apps_data/pocket_airbridge/app-usb.html`

Use `scripts/storage.py send -f`, verify sizes with `storage.py size`, then use
`scripts/runfap.py`. A final `Device not configured` error is expected when the
launched FAP intentionally replaces CDC with HID.

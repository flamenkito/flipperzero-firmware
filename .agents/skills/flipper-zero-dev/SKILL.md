---
name: flipper-zero-dev
description: |
  Flipper Zero firmware development, FAP creation, custom USB HID profiles,
  BLE service patching, and browser integration (WebHID/Web Bluetooth).
  Use when building Flipper Zero apps, flashing firmware, creating custom
  hardware profiles, or debugging USB/BLE communication with the device.
license: MIT
metadata:
  author: pocket-airbridge-project
  version: "2.0"
  project: flipper-hid
  created: "2026-07-04"
---

# Flipper Zero Development Guide

## Overview

This skill covers the complete workflow for developing Flipper Zero firmware,
building Flipper Application Packages (FAPs), creating custom USB HID profiles,
patching BLE services, and integrating with browser APIs (WebHID/Web Bluetooth).

## Pocket AirBridge Current Contract

- The firmware checkout is `~/projects/flipperzero-firmware`, normally on the
  `pocket-airbridge` branch of `git@github.com:flamenkito/flipperzero-firmware.git`.
- The live data interface is a 64-byte, report-ID-0 HID collection on vendor
  usage page `0xFF00`. Do not reuse the stock U2F collection: Chromium blocks
  FIDO usage page `0xF1D0` from ordinary WebHID access.
- The default USB identity is the HP composite profile, VID `0x03F0`, PID
  `0x5341`. The keyboard collection exists for the explicit Deploy flow only;
  Bridge mode must never emit keyboard reports.
- Select one USB profile from `/ext/apps_data/pocket_airbridge/config`, apply it
  once after app startup, and hold it until app exit. Never perform a
  composite-to-composite runtime switch.
- The browser-facing BLE transport uses the **custom AirBridge serial UUID
  family** generated in `web/airbridge-identity.js`. The required firmware
  addition is both the raw RX/TX hook in `bt_service` AND the new
  `airbridge_serial_service.c` with its own UUID family.
- HAL, Bluetooth service, or API-symbol changes require a complete firmware
  build and flash. Uploading a new FAP alone cannot install those changes.
- Treat `firmware/` in the main project as the reproducible patch bundle
  and `docs/firmware-guide.md` as the operational source of truth.
- The FAP ships at exactly one on-device path: `/ext/apps/USB/pocket_airbridge.fap`
  (its `fap_category="USB"` menu slot). Never deploy to `/ext/apps/` root or
  another category; assert one copy with `storage.py list /ext/apps | grep -i
  airbridge` before deploying and remove strays.

## Key Directories

| Path | Purpose |
|------|---------|
| `~/projects/flipperzero-firmware/` | Firmware source tree |
| `applications_user/` | External FAPs |
| `targets/f7/furi_hal/` | Hardware Abstraction Layer |
| `targets/furi_hal_include/` | Public HAL headers |
| `applications/services/bt/bt_service/` | Bluetooth service |
| `lib/ble_profile/extra_profiles/` | BLE profile templates |

## Building Firmware

### Full Firmware Build

```bash
cd ~/projects/flipperzero-firmware
./fbt
```

Output goes to `dist/f7-D/` and `build/f7-firmware-D/`.

### Building a FAP

```bash
./fbt build APPSRC=applications_user/my_app
```

Output: `build/f7-firmware-D/.extapps/my_app.fap`

### API Symbol Updates

When adding new HAL functions or variables, the build will fail with:
```
API version is still WIP: X.Y. Review the changes and re-run command.
CSV file entries to mark up:
```

**Fix**: Edit `targets/f7/api_symbols.csv` and change `?` to `+` for new entries:
```csv
Function,?,my_new_function,void,int
# Change to:
Function,+,my_new_function,void,int
```

Then rebuild.

## Flashing Firmware

> **Attention Signal** — BEFORE any physical gate during flashing (reboot, DFU entry, device plug/unplug, screen confirmation), play a loud audible alert:
> ```
> afplay /System/Library/Sounds/Funk.aiff && sleep 1 && afplay /System/Library/Sounds/Funk.aiff
> ```
> Fallback (if `afplay` unavailable): `say "Flipper needs your attention"`. The alert fires on EVERY physical gate, not just the first in a session.

### Method 1 (PRIMARY, USB-only, no DFU): `./fbt flash_usb`

**No DFU button combo, no ST-Link, no qFlipper needed.** The device stays normally
booted throughout. `./fbt flash_usb` builds the firmware, bundles a self-update
package, uploads it over the serial CLI to `/ext/update/f7-update-local/`, and
the Flipper reboots into its own built-in updater to flash itself.

```bash
# Build + flash in one step (self-update package over serial CLI)
./fbt flash_usb
```

For the full-image variant instead of an incremental update package:

```bash
./fbt flash_usb_full
```

**What happens**:
1. Builds firmware and bundles a self-update package
2. Uploads the package to `/ext/update/f7-update-local/` over the serial CLI
3. Flipper reboots into its own updater automatically
4. Self-flashes; serial port disappears for ~30–60 s then returns on its own
5. **Wait for `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*` (Linux) to reappear
   before running `storage.py` or `runfap.py`**

> **No DFU button combo needed.** Manual DFU entry is a recovery path for bricked
> devices only. If you ever need DFU from the CLI: `scripts/power.py reboot2dfu`
> reboots into DFU without button combos. Do NOT gate normal flashing on DFU.

### Method 2: qFlipper CLI (alternative, USB-only)

```bash
# Build first
./fbt

# Flash via qFlipper
/Applications/qFlipper.app/Contents/MacOS/qFlipper-cli \
  firmware dist/f7-D/flipper-z-f7-full-local.dfu
```

**What happens**:
1. Backs up internal settings
2. Reboots into recovery mode
3. Downloads firmware
4. Exits recovery mode
5. Restores settings
6. Final reboot

**Note**: After flash, the Flipper may show a lock screen. The user must unlock it
before serial/FAP deployment works.

### Method 3: `./fbt flash` (Requires Debugger)

Requires ST-Link or similar debug probe. Fails with "No available interfaces"
if only USB cable is connected.

## Creating a Custom USB HID Profile

### Pattern: Clone the U2F Profile

The U2F profile (`furi_hal_usb_u2f.c`) is the best template because it already has:
- Bidirectional endpoints (IN `0x81`, OUT `0x01`)
- 64-byte packet size
- Non-boot HID subclass
- Proper `usbd_ep_read` / `usbd_ep_write` handlers

### Steps

1. **Create `targets/f7/furi_hal/furi_hal_usb_myprofile.c`**
   - Copy structure from `furi_hal_usb_u2f.c`
   - Change Usage Page to vendor-defined (`0xFF00`)
   - Change VID/PID to unique values
   - Update string descriptors

2. **Create `targets/furi_hal_include/furi_hal_usb_myprofile.h`**
   - Declare callback type
   - Declare `get_request()` and `send_response()` functions
   - Declare `is_connected()`

3. **Register in `targets/furi_hal_include/furi_hal_usb.h`**
   ```c
   extern FuriHalUsbInterface usb_myprofile;
   ```

4. **Export symbols in `targets/f7/api_symbols.csv`**

5. **Rebuild full firmware** (not just FAP)

### Key API Functions

```c
// Set callback for connect/disconnect/request events
void furi_hal_hid_myprofile_set_callback(MyCallback cb, void* ctx);

// Read incoming data from host (non-blocking, returns bytes read)
uint32_t furi_hal_hid_myprofile_get_request(uint8_t* data);

// Send data to host (blocks until sent)
void furi_hal_hid_myprofile_send_response(uint8_t* data, uint8_t len);

// Check if host is connected
bool furi_hal_hid_myprofile_is_connected(void);
```

### Callback Events

```c
typedef enum {
    HidMyProfileDisconnected,
    HidMyProfileConnected,
    HidMyProfileRequest,  // Data available from host
} HidMyProfileEvent;
```

## Patching BLE Services

### Pattern: Add Raw Callback Hook

The stock BLE Serial service routes all data to the RPC system. To intercept
it for custom protocols:

1. **Add the callback typedef and public setter in `bt_service/bt.h`**:
   ```c
   typedef uint16_t (*BtRawSerialCallback)(const uint8_t* data, uint16_t len, void* context);
   ```

2. **Add hook variables and setter in `bt_service/bt.c`**:
   ```c
   static BtRawSerialCallback bt_raw_serial_cb = NULL;
   static void* bt_raw_serial_ctx = NULL;
   
   void bt_set_raw_serial_callback(BtRawSerialCallback cb, void* ctx) {
       bt_raw_serial_cb = cb;
       bt_raw_serial_ctx = ctx;
   }
   ```

3. **Intercept in `bt_serial_event_callback`**:
   ```c
   if(event.event == SerialServiceEventTypeDataReceived) {
       if(bt_raw_serial_cb) {
           return bt_raw_serial_cb(event.data.buffer, event.data.size, bt_raw_serial_ctx);
       }
       // ... existing RPC code ...
   }
   ```

4. **Expose in public `bt_service/bt.h`**:
   ```c
   void bt_set_raw_serial_callback(BtRawSerialCallback cb, void* ctx);
   bool bt_serial_tx(const uint8_t* data, uint16_t len);
   ```

5. **Export in `api_symbols.csv`**

### AirBridge Serial Service UUIDs

| Component | UUID |
|-----------|------|
| Service | `7b871228-baf0-c5b4-5f46-9c2613d627a3` |
| TX (Flipper → Central, Notify) | `87825ec0-7398-8cb7-3242-b083eaa34f27` |
| RX (Central → Flipper, Write) | `152f7eeb-e3b7-5898-ba41-7ff66121c98d` |

## FAP Deployment Methods

> **Attention Signal** — BEFORE any physical gate during deployment (unplug/replug USB, screen confirmation, app launch), play a loud audible alert:
> ```
> afplay /System/Library/Sounds/Funk.aiff && sleep 1 && afplay /System/Library/Sounds/Funk.aiff
> ```
> Fallback (if `afplay` unavailable): `say "Flipper needs your attention"`. The alert fires on EVERY physical gate, not just the first in a session.

### Method 1: `./fbt launch` (USB + Auto-run)

```bash
./fbt launch APPSRC=applications_user/my_app
```

**Prerequisites**: Flipper must be unlocked and at desktop.

### Method 2: Official `storage.py` Upload + `runfap.py` Launch (Verified)

Use this when you want explicit upload, size verification, and launch steps:

```bash
cd ~/projects/flipperzero-firmware
./fbt build APPSRC=applications_user/my_app

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f \
  build/f7-firmware-D/.extapps/my_app.fap \
  /ext/apps/USB/my_app.fap

python3 scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 size \
  /ext/apps/USB/my_app.fap

python3 scripts/runfap.py -p /dev/cu.usbmodemflip_Luwot1 \
  -s build/f7-firmware-D/.extapps/my_app.fap \
  -t /ext/apps/USB/my_app.fap
```

For apps that switch USB mode after launch, `runfap.py` may end with `Error: read failed: [Errno 6] Device not configured`. Treat this as expected if the app intentionally drops CDC serial and enumerates as a custom USB device.

On macOS, verify USB-mode-switching apps with:

```bash
ioreg -p IOUSB -l | grep -A25 -i "My App Name\|product_id"
```

### Method 3: qFlipper GUI File Manager

1. Open qFlipper app
2. Navigate to `SD Card/apps/USB/` (or appropriate category)
3. Drag `.fap` file into the folder
4. Eject Flipper

### Method 4: SD Card Direct

1. Power off Flipper
2. Remove SD card
3. Mount on computer
4. Copy `.fap` to `apps/USB/`
5. Reinsert SD card

### Method 5: Raw Serial CLI (Last Resort)

The Flipper serial CLI (`/dev/cu.usbmodemflip_*` at 115200 baud) supports:
- `storage mkdir <path>`
- `storage write <path> <size>` followed by raw bytes
- `storage stat <path>`
- `storage list <path>`
- `loader open "<app_path>"`

Prefer `storage.py send` over hand-written raw serial transfer. If storage commands hang, stop stale clients, unplug/replug USB, unlock the Flipper, wait for the desktop, and retry.

## FAP Structure

```
applications_user/my_app/
├── application.fam      # Manifest
├── my_app.c             # Source
└── icon.png             # 10x10 PNG icon for menus
```

### application.fam

```python
App(
    appid="my_app",
    name="My App",
    apptype=FlipperAppType.EXTERNAL,
    entry_point="my_app_main",
    stack_size=2 * 1024,
    requires=["gui", "bt"],
    fap_category="USB",
    fap_icon="icon.png",
)
```

## WebHID Integration

### Connecting to Custom HID Profile

```javascript
const devices = await navigator.hid.requestDevice({
  filters: [{ vendorId: 0x03F0, productId: 0x5341 }]
});
```

### Sending Reports

```javascript
const REPORT_SIZE = 64;
const REPORT_ID = 0;

async function sendReport(data) {
  const padded = new Uint8Array(REPORT_SIZE);
  padded.set(data);
  await device.sendReport(REPORT_ID, padded);
}
```

### Receiving Reports

```javascript
device.addEventListener('inputreport', (e) => {
  const data = new Uint8Array(e.data.buffer);
  // Process data
});
```

### Requirements

- Chromium-based browser (Chrome, Edge, Brave)
- Secure origin (`https://` or `localhost`)
- User gesture (button click) to trigger `requestDevice()`

## Web Bluetooth Integration

### Connecting to AirBridge BLE

```javascript
const device = await navigator.bluetooth.requestDevice({
  acceptAllDevices: true,
  optionalServices: ['7b871228-baf0-c5b4-5f46-9c2613d627a3']
});

const server = await device.gatt.connect();
const service = await server.getPrimaryService('7b871228-baf0-c5b4-5f46-9c2613d627a3');

// TX characteristic (Flipper → Browser, Notify)
const txChar = await service.getCharacteristic('87825ec0-7398-8cb7-3242-b083eaa34f27');
await txChar.startNotifications();
txChar.addEventListener('characteristicvaluechanged', onData);

// RX characteristic (Browser → Flipper, Write)
const rxChar = await service.getCharacteristic('152f7eeb-e3b7-5898-ba41-7ff66121c98d');
```

### Key Differences from Standard UART-over-BLE

| Aspect | Standard Nordic UART | AirBridge Serial |
|--------|----------------------|-------------------|
| Service UUID | `6e400001...` | `7b871228...` |
| TX properties | Notify | **Notify** |
| MTU | Typically 20-512 | ~64 bytes typical |
| Pairing | None | PIN code (Yes/No) |

**Critical**: The TX characteristic uses **Notify** (not Indicate). The firmware
sets `GATT_CHAR_UPDATE_SEND_NOTIFICATION` (0x01). Using Indicate (0x02) on a
Notify characteristic causes the stack to emit zero notifications — this was
a root-caused bug.

## Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| `./fbt flash` fails with "No available interfaces" | No ST-Link/debugger | Use `./fbt flash_usb` instead (USB-only, no DFU needed); fall back to qFlipper-cli if needed |
| API version WIP error | New symbols not marked | Change `?` to `+` in `api_symbols.csv` |
| `app may not be runnable` | Symbols not exported | Add to `api_symbols.csv` and `public header` |
| WebHID device not found | Wrong VID/PID or not in secure context | Check filter, use `localhost` or `https` |
| Web Bluetooth can't pair | Not in secure context or not advertising | Use user gesture, ensure app is running |
| Serial deployment hangs | Flipper locked, stale client, or wedged CDC session | Kill stale clients, unplug/replug USB, unlock screen, wait for desktop |
| qFlipper-cli can't find device | Multiple serial devices | Use `-p /dev/cu.usbmodemflip_*` explicitly |
| `runfap.py` reports `Device not configured` after launch | App switched USB away from CDC serial | Verify the expected USB product with `ioreg` or Device Manager |
| App missing icon in menu | No `icon.png` or no `fap_icon` entry | Add 10x10 PNG and `fap_icon="icon.png"`, then rebuild/redeploy |

## Inspecting or Reverting Firmware Changes

Inspect `git status` and `git diff` first. Never run wildcard checkout/reset
commands in the shared firmware worktree. Revert only explicitly named files
after the user asks for destructive cleanup.

## References

- [references/usb-airbridge-profile.md](references/usb-airbridge-profile.md) — AirBridge USB descriptor and endpoint contract
- [references/ble-serial-passthrough.md](references/ble-serial-passthrough.md) — stock Serial UUIDs and raw service hook
- [references/firmware-build-and-flash.md](references/firmware-build-and-flash.md) — branch, build, flash, and deployment sequence
- [references/api-symbol-exports.md](references/api-symbol-exports.md) — external FAP API export checklist
- [references/hid-composite-deploy.md](references/hid-composite-deploy.md) — Composite HID profile and typed-bootstrap deploy lessons (hardware-verified 2026-07-20/21)
- [references/unleashed-compatibility.md](references/unleashed-compatibility.md) — why Unleashed still needs the AirBridge core patches
- [scripts/check-flipper.sh](scripts/check-flipper.sh) — Detect connected Flipper device

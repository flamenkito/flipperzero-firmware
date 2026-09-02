# Pocket AirBridge v2 Demo Script

Use this as the human runbook for a final demo. It lists what to do and what to record. Don't claim a hardware result unless you observed it during this run.

## Evidence folder

Write logs and screenshots under:

```text
.omo/evidence/airbridge-roadmap-encryption/task-9/
```

## Setup

1. Main repo: `/Users/asutov/projects/flipper-hid`.
2. Firmware repo: `/Users/asutov/projects/flipperzero-firmware`.
3. Serve the web app:

   ```bash
   cd /Users/asutov/projects/flipper-hid
   python3 -m http.server 8080
   ```

4. For browser-driven QA, use persistent Chrome CDP at `http://localhost:9222`, not an ephemeral browser.
5. If no Flipper is connected, record `tools/flipper_alive.py` output as the hardware blocker and stop before physical claims.

## Build and deploy

1. Build the browser deploy bundles:

   ```bash
   cd /Users/asutov/projects/flipper-hid
   python3 tools/build_bundle.py
   ```

   Expected outputs include `dist/app-usb.html.gz`, `dist/app-ble.html.gz`, raw and gzip SHA-256 values, and `web/bootstrap.js: 1196 chars`.

2. Build the live FAP:

   ```bash
   cd /Users/asutov/projects/flipperzero-firmware
   ./fbt build APPSRC=applications_user/pocket_airbridge
   ```

3. With the Flipper exited to desktop and unlocked, upload the FAP, bootstraps, and gzip app assets to the canonical paths:

   ```bash
   /ext/apps/USB/pocket_airbridge.fap
   /ext/apps_data/pocket_airbridge/bootstrap.js
   /ext/apps_data/pocket_airbridge/bootstrap-ble.js
   /ext/apps_data/pocket_airbridge/app-usb.html.gz
   /ext/apps_data/pocket_airbridge/app-ble.html.gz
   ```

4. Before launch, check there is exactly one FAP copy at `/ext/apps/USB/pocket_airbridge.fap`. Remove stale copies before continuing.

## Connect and unlock

1. Launch Pocket AirBridge.
2. PC-A opens `http://localhost:8080/web/chat-usb.html`, clicks **Connect USB**, and selects the active impersonation profile in the WebHID picker.
3. PC-B opens `http://localhost:8080/web/chat-ble.html`, clicks **Connect BLE**, selects the Flipper, and confirms the BLE numeric comparison code when first pairing.
4. Both browser pages display a six-digit SAS after the crypto handshake.
5. Compare the SAS out loud. Click **Accept SAS** on both pages only if they match.
6. Before SAS acceptance, send controls must stay locked and plaintext item frames must fail closed.

## Chat and file demo

1. Send `Hello from USB` from PC-A to PC-B.
2. Send `Hello from BLE` from PC-B to PC-A.
3. Send a small attachment from PC-A. Confirm the receiver offers a download and reports SHA-256 success after decrypt.
4. Send an attachment in the other direction if time allows.
5. Start a larger transfer, click **Cancel**, and confirm both sides return to idle.
6. Watch the Flipper screen during healthy traffic. `U->B` and `B->U` should increment. `DROP` and `TXERR` should stay `0` for a healthy run.

## Compressed Deploy demo

1. From the Bridge screen, use LEFT or RIGHT to select USB Deploy or BLE Deploy.
2. Open `https://blank.org` on the target PC, open DevTools, and click into the console.
3. Press OK on the Flipper. The screen shows `TYPING via USB` or `TYPING via BLE`; BACK aborts immediately.
4. The typed bootstrap paints a landing page. Click **Connect**.
5. The FAP streams `app-usb.html.gz` or `app-ble.html.gz`. The bootstrap verifies compressed bytes, requires `DecompressionStream("gzip")`, inflates the app, and replaces the page.
6. If the browser lacks gzip streaming support, expect `Transfer unsupported - retry` before a picker opens.

## Caveats to state if asked

- E2E crypto is browser-only and always on. The Flipper is a blind relay and never stores plaintext, session keys, or decrypted files.
- NACK retries exact current-item outer frames only. There is no byte-range resume, plaintext-range resume, or cross-session resume.
- BLE tuning evidence is static unless recorded in this run: configured ATT MTU 414, DLE enabled, 2M preference, 7.5 to 45 ms requested interval, and 244-byte serial value capacity. Negotiated runtime values require hardware logs.
- Stealth hardening includes deploy typing jitter and per-device DIS serial. BLE service UUID hiding and Windows USB tree comparison remain deferred without physical Chrome and Windows evidence.

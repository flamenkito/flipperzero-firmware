# Pocket AirBridge v2 Demo Script

Use this as the human runbook for a final demo. It lists what to do and what to record. Don't claim a hardware result unless you observed it during this run.

## Evidence folder

Write logs and screenshots under:

```text
.omo/evidence/airbridge-roadmap-encryption/task-9/
```

## Setup

1. Custom firmware and apps repo: `/Users/asutov/projects/flipperzero-firmware`.
2. Serve the web app:

   ```bash
   python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
   ```

3. For browser-driven QA, use persistent Chrome CDP at `http://localhost:9222`, not an ephemeral browser.
4. If no Flipper is connected, record `airbridge/tools/flipper_alive.py` output as the hardware blocker and stop before physical claims.
5. Before every physical or browser-permission action, follow root `AGENTS.md`: play the attention signal, check whether the expected state is already true, then use one `question` gate with confirmation and cancel. For a browser gate, verify the visible frontmost persistent-CDP tab first and name its exact URL in the question. Keep captures in `/tmp`, `.playwright-mcp/`, or `.omo/evidence/`, never in tracked product paths.

## Build and deploy

1. Build the browser deploy bundles:

   ```bash
   python3 /Users/asutov/projects/flipperzero-firmware/airbridge/tools/build_bundle.py
   ```

   Expected outputs include `airbridge/dist/app-usb.html.gz`, raw and bundle SHA-256 values, the normalized `airbridge/web/bootstrap.js` size and SHA-256, and regenerated `applications_user/pocket_airbridge/airbridge_assets_digest.h`.

2. Build the live FAP from the same repository:

   ```bash
   cd /Users/asutov/projects/flipperzero-firmware
   ./fbt build APPSRC=applications_user/pocket_airbridge
   ```

3. With the Flipper exited to desktop and unlocked, upload the FAP, bootstrap, and USB app asset to the canonical paths:

   ```bash
   /ext/apps/Tools/pocket_airbridge.fap
   /ext/apps_data/pocket_airbridge/bootstrap.js
   /ext/apps_data/pocket_airbridge/app-usb.html.gz
   ```

4. Before launch, run `python3 scripts/storage.py -p <port> list /ext/apps | grep -i airbridge`. It must return exactly `/ext/apps/Tools/pocket_airbridge.fap`; remove every stray copy before continuing.

## Connect and unlock

1. Launch Pocket AirBridge.
2. PC-A opens `http://127.0.0.1:8081/chat-usb.html`, clicks **Connect USB**, and selects the active impersonation profile in the WebHID picker.
3. PC-B opens `http://127.0.0.1:8081/chat-ble.html`, clicks **Connect BLE**, selects the Flipper, and confirms the BLE numeric comparison code when first pairing.
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

1. From the Bridge screen, use LEFT or RIGHT to select USB Deploy.
2. Open `https://blank.org` on the target PC, open DevTools, and click into the console.
3. Press OK on the Flipper. The screen shows `TYPING via USB`; BACK aborts immediately.
4. The typed bootstrap paints a landing page. Click **Connect**.
5. The FAP first verifies the bundle container's `ABND` magic, version, gzip marker, pinned size, and SHA-256, then streams its gzip payload. The bootstrap verifies the wire checksum, requires `DecompressionStream("gzip")`, inflates the app, and replaces the page.
6. If the browser lacks gzip streaming support, expect `Transfer unsupported - retry` before a picker opens.

## Caveats to state if asked

- E2E crypto is browser-only and always on. The Flipper is a blind relay and never stores plaintext, session keys, or decrypted files.
- NACK retries exact current-item outer frames only. There is no byte-range resume, plaintext-range resume, or cross-session resume.
- BLE tuning evidence is static unless recorded in this run: a configured and supported local ATT MTU maximum of 414, DLE enabled, 2M preference, a 7.5 to 45 ms requested interval, and 244-byte serial value capacity. Negotiated MTU is peer-driven; negotiated runtime values require hardware logs.
- Stealth hardening includes deploy typing jitter and per-device DIS serial. BLE service UUID hiding and Windows USB tree comparison remain deferred without physical Chrome and Windows evidence.

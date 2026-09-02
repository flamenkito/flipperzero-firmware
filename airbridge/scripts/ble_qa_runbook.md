# W8 BLE identity QA runbook

Run this only after the W8 tooling self-test is green. It is the hardware evidence
procedure for the BLE half of the HP impersonation goal. It does not flash,
deploy, or claim a result that was not observed.

## Preconditions

1. The W1–W6 and W10 firmware image is flashed to the Flipper and the Pocket
   AirBridge FAP is deployed.
2. The configuration is deployed at
   `/ext/apps_data/pocket_airbridge/config`; it must match
   `airbridge/config/pocket_airbridge.conf` used by this repository.
3. The QA host has Python 3, a working Bluetooth adapter, and Bleak:

   ```sh
   pip3 install bleak
   python3 airbridge/scripts/ble_qa_scan.py --selftest
   ```

   Expected: exit `0`; the good synthetic-advertisement table is all `PASS`, the
   bad synthetic fixture has expected `FAIL` rows, and the parser/rejection-oracle
   table is all `PASS`.

4. Use the same config values in every result. Do not substitute a remembered HP
   name or MAC: the scanner reads the checkout config unless `--name` or `--mac`
   is explicitly supplied.

> **macOS limitation:** CoreBluetooth exposes an opaque per-host UUID, not the
> on-air BLE MAC. On Darwin the scanner prints `WARN` and skips MAC equality, HP
> OUI, and adjacent-MAC checks. This is expected. Run the mandatory exact-MAC/OUI
> evidence on Windows N50194 (or Linux), where Bleak exposes the real address.

## Physical gates and scanner evidence

At every numbered physical gate, the orchestrator must first run:

```sh
afplay /System/Library/Sounds/Funk.aiff && sleep 1 && afplay /System/Library/Sounds/Funk.aiff
```

Use `say "Flipper needs your attention"` only if `afplay` is unavailable, then use
one `question` tool call with a completion and a cancel option. Do not poll for a
user action.

1. **Forget stale bond — physical gate.** In the host OS Bluetooth settings, forget
   every old Flipper/Pocket AirBridge device and remove any cached pairing. Confirm
   with `question` only after the device has been forgotten. This avoids stale GATT
   cache and old bond-key failures.
2. **Launch FAP — physical gate.** Navigate to and launch Pocket AirBridge on the
   Flipper. Confirm with `question` when its always-on Bridge screen is visible.
3. **Passive identity scan — no connection.** Run:

   ```sh
   python3 airbridge/scripts/ble_qa_scan.py scan --timeout 8
   ```

   Expected exit `0`: every row is `PASS` (Darwin MAC/OUI/adjacent rows are the
    documented `WARN` exception). The target's local name equals config `ble_name`,
    no row reports `flipper`, the AirBridge serial service and HIDS `0x1812` are
    advertised, HP company data is present, and exactly one matching source exists.
    On Windows/Linux, the actual address must exactly equal config `ble_mac` and its
     OUI must be allowed. The normal active state after startup/watchdog recovery
     advertises both services in Bridge and Deploy; advertising HIDS does not emit
     keyboard reports. Keyboard reports remain restricted to an explicitly
     confirmed Deploy typing flow.
4. **Pair and enumerate GATT — physical gate when the OS dialog appears.** Start:

   ```sh
   python3 airbridge/scripts/ble_qa_scan.py gatt --timeout 12
   ```

   The command requests pairing on Linux/Windows before connection. When the OS
   numeric-comparison dialog appears, use the alert and `question` tool; compare the
   number shown on the host with the Flipper and accept only when they agree. On macOS,
   CoreBluetooth owns pairing and may prompt on the first authenticated access; accept
   the same numeric comparison then. Do not cancel the dialog.

   Expected exit `0`: the pre-flight scan is green, then GATT has only GAP `0x1800`,
   GATT `0x1801`, DIS `0x180A`, Battery `0x180F`, HIDS `0x1812`, and the serial
   service parsed from `airbridge/web/airbridge-identity.js`. DIS has only its four expected
   characteristics and config strings, no readable value matches a git hash, GAP has
   the config name and appearance `0x03C1`, and serial has exactly the four canonical
   UUIDs parsed from that module.
5. **Exit FAP — physical gate.** Long-press BACK to leave Pocket AirBridge. Confirm with
   `question` when the Flipper has returned to its desktop; this exercises the FAP's
   mandatory profile restore path.
6. **Verify restoration after exit.** Run:

   ```sh
   python3 airbridge/scripts/ble_qa_scan.py stock --timeout 8
   ```

   Expected exit `0`: `STOCK TARGET FOUND` observes a name beginning `Flipper`, and
   `stock fe60-family serial service is advertised` is `PASS`. This is deliberately
   the opposite identity from steps 3–4.
7. **Reboot — physical gate.** Reboot the Flipper normally. Confirm with `question`
   only when it has restarted and reached the desktop.
8. **Verify restoration after reboot.** Repeat the previous `stock` command. Expected:
    exit `0` with both stock rows `PASS`. Save both scanner transcripts under
    `.omo/evidence/ble-impersonation/` as W8 evidence.

## Chromium / Playwright Web Bluetooth scenario

This scenario verifies the browser contract separately from Bleak. Use the
persistent Chrome instance with CDP on port 9222, not an ephemeral Playwright
browser, and connect every Playwright call with `cdp_url="http://localhost:9222"`.
Use an HTTPS or `localhost` origin; Web Bluetooth does not work from a file URL.
Keep the FAP running from step 2. Before each browser gate, confirm that the
Chrome window is visible, the target tab is frontmost, and its URL is the exact
one named in the `question` prompt. Save captures only in `/tmp`,
`.playwright-mcp/`, or `.omo/evidence/`.

1. Serve the in-tree web directory and have Playwright navigate its controlled
   window to `http://127.0.0.1:8081/chat-ble.html`:

   ```sh
   python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
   ```

2. Inject the following control with `page.evaluate()` in that exact Playwright
    window, then use `page.locator('#ble-qa-all').click()`. The dynamic import makes the
    test consume the checked-in generated identity rather than copied values:

   ```js
   await page.evaluate(async () => {
     const { identity } = await import('/airbridge-identity.js');
       const button = document.createElement('button');
       button.id = 'ble-qa-all';
       button.textContent = button.id;
       button.addEventListener('click', async () => {
         const device = await navigator.bluetooth.requestDevice({
           acceptAllDevices: true, optionalServices: [identity.SERIAL_SERVICE_UUID],
         });
        const service = await (await device.gatt.connect())
          .getPrimaryService(identity.SERIAL_SERVICE_UUID);
        console.log('BLE_QA_PASS', button.id, device.name, service.uuid);
      });
      document.body.append(button);
   });
   ```

3. The click must open a Chromium picker with the full device list. Before the human
   picker action, wait about five seconds and verify the picker was not already
   handled. Then play the alert and use `question` naming the Playwright window
   showing `http://127.0.0.1:8081/chat-ble.html` explicitly. Select the HP-named
   device and approve numeric comparison only when its values agree.
4. Expected console result is `BLE_QA_PASS`, the HP device name, and the generated
   AirBridge serial UUID. No `SecurityError` may appear. This proves the user can select
   the HP device from unfiltered discovery and `getPrimaryService(identity.SERIAL_SERVICE_UUID)`
   succeeds via `optionalServices` without requesting blocklisted HIDS `0x1812` in a filter.
5. Capture the run. Expected browser evidence is: the requested picker options contain
   `acceptAllDevices: true`; `optionalServices` contains only the generated AirBridge
   serial UUID; no request filter contains `0x1812`; and the serial service is obtained
   successfully.

The picker itself cannot be accepted by Playwright: Chromium requires a real user
gesture and OS permission. That is why each picker/pairing interaction is a separate
attention-signal plus `question` gate rather than an automation retry loop.

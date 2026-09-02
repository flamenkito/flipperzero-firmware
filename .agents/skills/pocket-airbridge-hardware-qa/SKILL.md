---
name: pocket-airbridge-hardware-qa
description: Pocket AirBridge hardware deployment and end-to-end QA. Use when flashing firmware, uploading or launching the FAP, deploying bootstrap assets, pairing WebHID or Web Bluetooth, running the physical demo, or diagnosing USB/BLE enumeration.
license: MIT
metadata:
  author: pocket-airbridge-project
  version: "1.0"
  project: flipperzero-firmware
  created: "2026-07-21"
---

# Pocket AirBridge Hardware QA

Use this skill for operations against a physical Flipper Zero. For firmware or
FAP implementation details, also load `flipper-zero-dev`. For browser steps,
load the `playwright` skill and drive the same named Playwright window described
to the user.

## Success condition

Do not declare success from a green build. Success requires observing the
physical system through its real surfaces:

1. Correct impersonated USB identity enumerates.
2. WebHID opens the vendor collection and exchanges reports.
3. Web Bluetooth discovers the AirBridge serial service and subscribes to TX notify.
4. Text succeeds in both directions.
5. An attachment completes with SHA-256 verification.
6. Cancel returns both endpoints to idle.
7. Flipper shows `DROP: 0` and `TXERR: 0` during healthy traffic.

## Persistent browser — Playwright MUST NOT time out

The Playwright MCP's **default ephemeral browser dies during long physical
gates** (idle timeout) while the user is at the Flipper for minutes. When it
dies, every timed browser step (deploy streaming, WebHID picker) silently breaks
and has to be redone. **Never let that happen.** Use a persistent browser for the
entire QA session:

1. Launch a persistent Chrome ONCE, before the first browser step, with a CDP
   remote-debugging port and a dedicated profile:

   ```bash
    mkdir -p ~/.cache/airbridge-pw-chrome && open -na "Google Chrome" --args --remote-debugging-port=9222 --user-data-dir=$HOME/.cache/airbridge-pw-chrome "https://blank.org"
   ```

   Verify it stays up across gates: `curl -s http://localhost:9222/json/version`.

2. Connect Playwright to it via CDP instead of the ephemeral browser — pass
   `cdp_url="http://localhost:9222"` to every `skill_mcp` Playwright call. A
   normal Chrome process with a user-data-dir does NOT idle-timeout.

3. If the browser ever IS gone (process killed, Mac slept), relaunch the
   persistent Chrome from step 1 and reconnect — do not fall back to the
   ephemeral MCP browser for a gated flow.

4. Keep the tab at the intended URL; re-navigate on reconnect rather than asking
   the user to restore it.

**Mandatory physical gate protocol** (unchanged) applies on top of this; the
persistent browser is what makes the gates safe.

## Screenshot and capture paths

`browser_take_screenshot`, page snapshots, and any QA capture files MUST NEVER
land in the project working directory (repo root or any tracked subdir). Pass
an explicit absolute `filename` under `/tmp` (or the opencode tmp dir), the
Playwright MCP default `.playwright-mcp/` directory (gitignored), or — for
evidence meant to persist — directly into `.omo/evidence/<plan-name>/`. Never
accept a default cwd-relative path. Stray captures found in the repo are
deleted or moved to `.omo/evidence/` immediately.

## Mandatory physical gate protocol

Never assume a hardware or permission action happened. Before every button
press, device unlock, plug/unplug, reset, browser picker, BLE pairing prompt, or
Flipper-screen confirmation:

1. Play the attention signal:

   ```bash
   afplay /System/Library/Sounds/Funk.aiff && sleep 1 && afplay /System/Library/Sounds/Funk.aiff
   ```

   If unavailable, run `say "Flipper needs your attention"`.

2. Call the `question` tool with one physical action, one confirmation option,
   and one cancel/skip option.
3. Stop issuing dependent commands until the user confirms.
4. Never poll for a physical state while blocked on the user.

When the user must act in a browser controlled by Playwright, navigate that
window first and name it exactly in the question, for example: “the Playwright
window showing `http://127.0.0.1:8081/chat-usb.html`.”

## Preflight

Confirm without modifying state:

- Custom firmware and apps repository: `/Users/asutov/projects/flipperzero-firmware`
- Firmware branch: `pocket-airbridge`
- Product assets: `airbridge/`; canonical FAP source: `applications_user/pocket_airbridge/`
- Flipper is unlocked and at the desktop before serial operations.
- Bluetooth is enabled on the Flipper and receiving computer.
- Chrome/Edge runs from `localhost` or HTTPS.

Use `airbridge/tools/flipper_alive.py` to distinguish ALIVE, HUNG, and ABSENT
states. Port present but no CLI prompt means HUNG and requires a physical reset.

## Build and flash sequence

Run from the repository root:

```bash
./fbt build APPSRC=applications_user/pocket_airbridge
./fbt flash_usb
```

Use `flash_usb`, not debugger-only `./fbt flash`. Expect serial to disappear
for roughly 30–60 seconds. Wait once with a suitably long timeout rather than
repeated short polls. If the updater or lock screen needs user action, use the
physical gate protocol.

After the device returns, verify it is running the newly built firmware before
deploying the FAP. A newly built FAP against old firmware can link successfully
yet fail at launch because its exported API version or symbols differ.

## Build and deploy assets

The AirBridge FAP removes CDC while running. Upload everything before launch or
after explicitly exiting the app.

From the repository root:

```bash
python3 airbridge/tools/build_bundle.py
```

From the repository root, upload and verify:

| Source | Destination |
|---|---|
| `build/f7-firmware-D/.extapps/pocket_airbridge.fap` | `/ext/apps/USB/pocket_airbridge.fap` |
| `airbridge/web/bootstrap.js` | `/ext/apps_data/pocket_airbridge/bootstrap.js` |
| `airbridge/web/bootstrap-ble.js` | `/ext/apps_data/pocket_airbridge/bootstrap-ble.js` |
| `airbridge/dist/app-usb.html.gz` | `/ext/apps_data/pocket_airbridge/app-usb.html.gz` |
| `airbridge/dist/app-ble.html.gz` | `/ext/apps_data/pocket_airbridge/app-ble.html.gz` |

Use `scripts/storage.py send -f` and then `scripts/storage.py size` for each
artifact. Launch with `scripts/runfap.py`. A terminal `Device not configured`
error is expected after a successful launch because the app replaces CDC with
the HID profile.

**Canonical FAP path:** `/ext/apps/USB/pocket_airbridge.fap` is the ONLY valid location. Before every deploy, run `storage.py list /ext/apps | grep -i airbridge` and require exactly one line; remove any strays with `storage.py remove`. A duplicate at `/ext/apps/` root goes stale and shadow-launches old builds from the menu (burned on 2026-07-23: a 19,044 b stale root copy hid the icon-screen build).

## USB verification

On macOS, inspect the real USB tree with `ioreg -p IOUSB -l`. For the default
profile verify:

- Manufacturer: HP
- Product: HP Wireless Keyboard and Mouse
- VID: `0x03F0`
- PID: `0x5341`
- No STM32/Flipper VID `0x0483` while the app runs

Then serve `airbridge/web/` with `python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web`, navigate the Playwright browser to `http://127.0.0.1:8081/chat-usb.html`, and use the physical gate protocol for the WebHID chooser.
Select the currently configured impersonation profile. Verify the page reports
an open transport and that input reports arrive with report ID `0`.

On macOS, a composite keyboard profile may require Input Monitoring permission
for the exact browser binary. Playwright Chromium and the user's Chrome are
different binaries and need separate grants. **Input Monitoring permission applies
to a browser binary only after that browser is RELAUNCHED** — grants are not
live-reloaded for a running process.

**Test Surface and Gate Discipline (four rules):**
- **PLAYWRIGHT TEST SURFACE:** open and keep a Playwright window so the agent can
  monitor page state/logs via `browser_evaluate`; user handles only physical gates,
  named exactly (e.g. "the Playwright window showing `chat-usb.html`"). The window
  MUST be the **persistent CDP browser** from the "Persistent browser" section above
  — the ephemeral Playwright MCP browser times out during gates and is forbidden
  for gated flows.
- **CHECK-BEFORE-GATING:** before calling `question`, wait ~5 s and CHECK whether
  the state already happened. Never gate something already true.
- **REGRESSION RULE:** after every firmware/FAP/web change, re-run all previously
  working flows (bridge text both directions, file transfer SHA-256, USB deploy,
  protocol harness 17/17) before debugging anything new.
- **OPEN-BEFORE-GATING (browser surfaces):** before ANY gate that asks the user to
  act in a browser (open DevTools, click into a console, handle a picker), the
  orchestrator MUST first verify programmatically that (1) the target browser process
  has a VISIBLE window (not a windowless/background instance), (2) the intended tab
  is loaded at the intended URL and is frontmost. If anything is missing, the
  orchestrator opens/raises/navigates it ITSELF first (CDP `/json/new`,
  `/json/activate`, `open -a`, or Playwright navigate) and only then fires the
  question, naming the exact window/tab. Never gate a browser action on an
  unverified window — the browser-state check (`/json/list` + OS window count) is
  part of every browser-gate preflight, exactly like the serial-port check before
  hardware gates. Asking the user to hunt for a window or tab is an orchestrator
  failure.

## BLE verification

Navigate the intended Playwright window to `http://127.0.0.1:8081/chat-ble.html` before asking the
user to act. Use the physical gate protocol separately for:

1. Opening the Web Bluetooth chooser.
2. Selecting the Flipper.
3. Confirming the pairing code on the Flipper.

Verify discovery of the AirBridge serial UUID family, RX writes, TX notifications,
and a stable GATT connection. The browser transport uses the custom AirBridge
UUID family from `airbridge/web/airbridge-identity.js`.

## End-to-end QA matrix

Run each scenario through the visible chat UI:

| Scenario | Expected observation |
|---|---|
| USB to BLE text | Receiver renders exact text; sender receives ACK |
| BLE to USB text | Receiver renders exact text; sender receives ACK |
| Small attachment | Download appears and SHA-256 passes |
| Multi-kilobyte attachment | Progress advances under ACK backpressure |
| Sender cancel | Peer reports cancellation and both sides return idle |
| Receiver cancel | Sender stops and both sides return idle |
| Disconnect during transfer | Visible failure; no silent hang |

During healthy transfers, inspect the Flipper display:

- `U->B` and `B->U` increment with traffic.
- `DROP` stays zero.
- `TXERR` stays zero except a possible single visible cancel race.
- The green heartbeat continues every 500 ms.

## Recovery

- Missing serial port while AirBridge is running: exit the FAP with BACK using
  the physical gate protocol.
- Port present but CLI silent: physical reset; do not attempt DTR reset loops.
- WebHID device absent: verify HTTPS/localhost, active VID/PID filter, usage page
  `0xFF00`, and browser Input Monitoring permission.
- BLE service absent: ensure advertising is active, refresh the page, and verify
  the browser UUID canonicalization list.
- Persistent `TXERR`: reconnect BLE before retrying.
- GUI launch hangs but CLI launch works: check for stale duplicate FAPs in
  `/ext/apps/<category>/`. Also check `/ext/apps/` root — the canonical path is
  `/ext/apps/USB/pocket_airbridge.fap` and exactly one copy may exist.

See [references/hardware-checklist.md](references/hardware-checklist.md) for the
compact evidence ledger used during a demo or release check.

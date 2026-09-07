# Agent Instructions for Pocket AirBridge

## Project Overview

Pocket AirBridge is a browser-only offline file transfer system using a Flipper Zero
as a physical bridge between two computers via USB HID and Bluetooth Low Energy.
It is developed locally in this one custom firmware and apps repository:
`/Users/asutov/projects/flipperzero-firmware`. Product assets live under
`airbridge/`; firmware changes and the FAP are canonical in-tree sources. Do not
use a second product checkout, a patch bundle, or a patch-application workflow.
Development is local-only. Do not push, create a pull request, or suggest an
upstream contribution workflow for Pocket AirBridge.

## Architecture

- **PC-A**: WebHID chat page `chat-usb.html` in `airbridge/web/` sends messages and attachments over USB
- **Flipper Zero**: FAP bridges USB HID ↔ BLE Serial transparently
- **PC-B**: Web Bluetooth chat page `chat-ble.html` in `airbridge/web/` receives and reassembles

## Repository Layout

```
flipperzero-firmware/
├── airbridge/
│   ├── web/
│   │   ├── chat-usb.html           # WebHID chat page (PC-A, primary)
│   │   ├── chat-ble.html           # Web Bluetooth chat page (PC-B, primary)
│   │   ├── airbridge-protocol.js   # Shared wire protocol
│   │   ├── airbridge-transports.js # Shared transport helpers
│   │   └── protocol-harness.html   # In-browser protocol test harness
│   └── docs/                       # Architecture, protocol, and operations docs
├── applications_user/
│   └── pocket_airbridge/           # Canonical FAP source
└── .agents/skills/
    └── flipper-zero-dev/   # Agent skill for Flipper dev workflow
```

## Flipper Firmware

This is one custom firmware and apps repository. Its in-tree additions include:
- `usb_airbridge` HID profile (vendor-defined, 64-byte, bidirectional)
- BLE Serial passthrough hooks in `bt_service`
- FAP: `applications_user/pocket_airbridge/`
- FAP icon: `applications_user/pocket_airbridge/icon.png` (10x10 PNG, referenced by `fap_icon`)

## Development Workflow

1. **Web changes**: Edit `airbridge/web/*.html`, then serve with this canonical command:
   ```bash
   python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
   ```
   Use `http://127.0.0.1:8081/chat-usb.html`, `http://127.0.0.1:8081/chat-ble.html`, and `http://127.0.0.1:8081/protocol-harness.html`.
2. **Firmware changes**: Edit this repository, then build with `./fbt`
3. **FAP changes**: Edit `applications_user/pocket_airbridge/`, deploy with `scripts/storage.py send` to the canonical path `/ext/apps/USB/pocket_airbridge.fap` (the ONLY location — never `/ext/apps/` root or another category), verify with `storage.py size`, then launch with `scripts/runfap.py`. Before deploying, assert exactly one copy on device: `storage.py list /ext/apps | grep -i airbridge` must return one line; `storage.py remove` any strays.

## Protocol

- 64-byte frames, 59-byte payload
- Types: HELLO(0x01), ITEM_META(0x02), ITEM_DATA(0x03), ACK(0x04), NACK(0x05), ITEM_DONE(0x06), ERROR(0x07), BUSY(0x08), CANCEL(0x09), KEY_OFFER(0x0A), KEY_REPLY(0x0B), KEY_CONFIRM(0x0C), KEY_ABORT(0x0D)
- ACK payload is `[acked_type(1), acked_seq(2 BE)]` — match on both fields
- NACK payload is `[nacked_type(1), nacked_seq(2 BE), reason(1)]`, with reasons `1=missing`, `2=malformed`, `3=auth-failed-retryable`, `4=busy-window`; NACK retries only exact current-item outer frames and does not resume transfers
- HELLO payload is the 4-byte big-endian `itemId`; ITEM_META JSON uses `kind` ("text" | "attachment")
- ACK-per-chunk backpressure; receiver verifies SHA-256 before ACKing ITEM_DONE and sends ERROR on mismatch
- Always-on browser-only E2E crypto uses P-256 ECDH, HKDF, AES-GCM, and SAS confirmation; the Flipper remains a blind relay and never sees plaintext or session keys
- End-to-end SHA-256 verification
- Flipper-side debugging aids: green LED heartbeat blink every 500 ms while the app runs; on-screen counters `U->B` / `B->U` / `DROP` / `TXERR` (DROP and TXERR should stay 0 during healthy transfers)

## Testing

- Both pages must be on `localhost` or `https://` (WebHID/Web Bluetooth requirement)
- Use Chrome/Edge (Chromium-based)
- Single-PC test: open sender in one tab, receiver in another
- Flipper must show "Pocket AirBridge" screen with USB/BLE status

## Agent Loop-Break Rule — Blocking User Interaction

When the orchestrator is blocked on a **user action** (hardware button press, Flipper interaction, browser permission dialog, physical device operation), it MUST use the built-in **`question` tool** to pause execution and wait for confirmation.

### How It Works

The `question` tool blocks agent execution, renders an interactive TUI dialog in the terminal, and resumes only after the user answers. It supports:

- **Single-select** (radio buttons) — "Which action should I take?"
- **Multi-select** (checkboxes) — "Select all that apply"
- **Confirm** — "Press Enter to confirm you've completed the action"
- **Custom free-text** — user can type their own answer

### Required Usage Pattern

When blocked on a physical action, the orchestrator MUST call the `question` tool with:

1. A clear `header` (short label, max 12 chars)
2. A `question` describing exactly what the user needs to do (one sentence)
3. `options` including a confirmation choice (e.g., "Done — continue")

Example for a Flipper device action:

```
Tool: question
Questions:
  - header: "DFU Mode"
    question: "Put the Flipper Zero into DFU mode: hold BACK + LEFT while powering on. Confirm when the screen shows 'DFU Mode'."
    options:
      - label: "Done — Flipper is in DFU mode"
        description: "Screen shows DFU Mode, device is ready for flashing"
      - label: "Cancel — skip this step"
        description: "Abort the current operation"
```

Example for a browser action:

```
Tool: question
Questions:
  - header: "WebHID Pair"
    question: "In Chrome, the WebHID device picker should appear. Select 'Pocket AirBridge' from the list and click 'Connect'."
    options:
      - label: "Done — device connected"
        description: "WebHID connection established successfully"
      - label: "Device not found"
        description: "The Flipper Zero doesn't appear in the device list"
```

### Critical Rules

1. **MUST call `question`** — never assume the user performed an action without explicit confirmation
2. **MUST NOT poll** — do not loop checking device state; the `question` tool blocks until answered
3. **One action per question** — each physical step gets its own `question` call
4. **Handle cancellation** — provide a cancel/skip option for every question
5. **MCP Browser Window Naming** — when a workflow step needs the user to act in a SPECIFIC browser window driven by the Playwright MCP, the orchestrator MUST first navigate that window to the exact URL and then explicitly name it in the question (e.g. "the Playwright window showing https://blank.org"). Never assume the user is acting in the same browser the orchestrator is driving. State scattering across two browsers cost a full debug session on 2026-07-20.
6. **Attention Signal** — BEFORE calling `question` (or otherwise blocking on a user action), play a loud audible alert:
   ```
   afplay /System/Library/Sounds/Funk.aiff &
   ```
   Fallback (if `afplay` unavailable): `say "Flipper needs your attention"`. The `&` is mandatory: play the sound ONCE, backgrounded, and call `question` immediately after — never block on or repeat the alert. The alert fires on EVERY physical gate — DFU entry, device plug/unplug, browser picker, screen confirmation, etc. — not just the first in a session.

### Test Surface and Gate Discipline

**a. PLAYWRIGHT TEST SURFACE:** for any browser-involved test, the orchestrator opens and keeps a Playwright window (chat pages on `http://127.0.0.1:8081/` and `https://blank.org` for deploy tests — blank.org is an offline-cacheable blank HTTPS origin) so the agent can monitor page state/logs directly via `browser_evaluate`; the user only performs physical gates (device pickers, OS permission prompts) — always named with the exact window (e.g. "the Playwright window showing https://blank.org").

**a2. PERSISTENT PLAYWRIGHT BROWSER (MANDATORY — never ephemeral):** the test browser MUST be a persistent Chrome instance launched once with CDP enabled, NOT a skill-spawned ephemeral browser. Skill-spawned MCP browsers die when the MCP instance is reaped during long physical gates (lost a full measurement session on 2026-08-28). Launch once per machine/session:

```
mkdir -p ~/.cache/airbridge-pw-chrome && open -na "Google Chrome" --args --remote-debugging-port=9222 --user-data-dir=$HOME/.cache/airbridge-pw-chrome "https://blank.org"
```

Verify with `curl -s http://localhost:9222/json/version`. All Playwright MCP calls then pass `cdp_url="http://localhost:9222"` — tabs and page state survive ANY MCP instance restart, gate length, or opencode hiccup. If the port is dead, relaunch with the same command (profile persists). NEVER rely on the default skill-embedded browser for a flow that contains physical gates.

**b. CHECK-BEFORE-GATING:** before calling the `question` tool for a user action, wait ~5 seconds and CHECK whether the expected state already happened (page connected, picker already handled, counter already ticked). Never fire a gate for something already true.

**c. REGRESSION RULE:** after EVERY firmware/FAP/web change, re-run previously-working flows BEFORE debugging anything new: bridge chat text both directions, file transfer with SHA-256, USB deploy, protocol harness 17/17. A change is not done until old flows still pass.

**d. OPEN-BEFORE-GATING (browser surfaces):** before ANY gate that asks the user to act in a browser (open DevTools, click into a console, handle a picker), the orchestrator MUST first verify programmatically that (1) the target browser process has a VISIBLE window (not a windowless/background instance), (2) the intended tab is loaded at the intended URL and is frontmost. If anything is missing, the orchestrator opens/raises/navigates it ITSELF first (CDP `/json/new`, `/json/activate`, `open -a`, or Playwright navigate) and only then fires the question, naming the exact window/tab. Never gate a browser action on an unverified window — the browser-state check (`/json/list` + OS window count) is part of every browser-gate preflight, exactly like the serial-port check before hardware gates. Asking the user to hunt for a window or tab is an orchestrator failure.

**e. SCREENSHOT/ARTIFACT PATHS:** Playwright screenshots, page snapshots, and any QA capture files MUST NEVER be saved into the project working directory (repo root or any tracked subdir). Allowed destinations only: the Playwright MCP default `.playwright-mcp/` directory (gitignored), `/tmp` (or the opencode tmp dir), or — for evidence meant to persist — directly into `.omo/evidence/<plan-name>/`. Every `browser_take_screenshot` call MUST pass an explicit absolute `filename` in one of these locations; never accept a default/cwd-relative path. Stray captures found in the repo must be deleted or moved to `.omo/evidence/` immediately. (Two `crypto-pill-*.png` files landed in the repo root on 2026-08-31 when a subagent accepted the default path.)

### Applicable Actions

This applies to: exiting a Flipper app, entering DFU mode, clicking a browser picker, plugging/unplugging USB, pressing physical buttons, toggling Bluetooth pairing mode, confirming a dialog on the Flipper screen.

### Attention Signal

BEFORE calling `question` (or otherwise blocking on a user action), play a loud audible alert so the user notices immediately:

```
afplay /System/Library/Sounds/Funk.aiff &
```

Fallback (if `afplay` unavailable): `say "Flipper needs your attention"`. The `&` is mandatory: play the sound ONCE, backgrounded, and call `question` immediately after — never block on or repeat the alert.

The alert fires on EVERY physical gate — DFU entry, device plug/unplug, browser picker, screen confirmation, etc. — not just the first in a session.

## Key Constraints

- API version policy: minor bumps only; major bumps break all existing FAPs.
- Keyboard emulation exists ONLY as the menu-gated Deploy flow: an explicit `Deploy app` menu action, an on-screen confirmation after cursor placement, `TYPING…` shown for the entire emission, and BACK aborting instantly. No keyboard reports are ever sent in Bridge mode or on any data path.
- The Deploy flow is BadUSB-shaped by design (HID is the only USB class that survives HID-only policies) and requires physical possession plus explicit on-device action. The typed payload is a fixed, reviewable ASCII artifact (`airbridge/web/bootstrap.js`).
- A single USB impersonation profile is selected once from `/ext/apps_data/pocket_airbridge/config` (default `hp_kbd_vendor`, HP VID `0x03F0` PID `0x5341`). There is NO runtime profile switching: composite→composite reconfiguration is fatal on this USB stack. The menu profile line is display-only.
- The Flipper's real USB identity (STM32 VID `0x0483`) must never be exposed on the bus while the app runs; every profile is a full impersonation.
- The typed bootstrap requires a US keyboard layout on the target PC (ASCII-only payload, US scancodes).
- Explicit user consent on both sides
- Flipper never stores a full file: relay traffic is held only in a bounded eight-slot event queue of one-frame `BridgeEvent` values
- Optimize for hackathon demo, not production

## Agent Skill

See `.agents/skills/flipper-zero-dev/` for detailed Flipper firmware development
workflows, USB HID profile creation, BLE patching, and deployment methods.

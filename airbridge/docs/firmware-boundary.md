# Firmware Ownership

Pocket AirBridge's USB profiles, BLE profile, DIS strings, serial UUIDs, GATT
service, relay routing, and reconnect policy live in
`applications_user/pocket_airbridge/`. Browser assets and their wire protocol
live under `airbridge/`. `gen_identity.py` reads the app-owned UUID header.

The comparison base is firmware commit `c9ab2b68`. The vendor submodule is back
at `133182d5583e998bb263cd947105be4df9c29cb3`. The USB HAL and `furi/core/` match
that base. Shared native HID changes cover partial-creation cleanup and event
ownership: the HID callback must not consume another service's GATT writes.
Normal advertising returns to the firmware's low-power interval after its initial
fast period.

At the ownership refactor, against `c9ab2b68`, the shared-source delta went from
49 paths (+4,128/-334) at implementation baseline `3b253f12` to 19 paths
(+1,111/-209): about 70% fewer
changed lines. This counts `applications/services/`, `targets/`, `furi/`, and
`lib/`, including the vendor gitlink; it excludes the app and product assets.
These are historical refactor counts, before the BLE Deploy restoration.

## Remaining Shared Changes

| Area | Reason it remains in firmware |
| --- | --- |
| `bt.c`, `bt_i.h`, `bt_profile_quiescence.h` | Service-owned profile publication and reader references prevent destruction during direct GATT operations. A mailbox avoids blocking the BLE event worker on the service queue. SD reload leaves external profiles alone; retries retain only the firmware's default profile. |
| `bt_api.c`, `bt.h`, `bt_status_registration.h` | Callback registration, ordered initial status, and draining must synchronize with the firmware dispatcher. A pairing-wait flag lets the app exclude human input from deadlines. |
| `furi_hal_bt.c`, `ble_event_thread.c` | Join the event worker before destroying service callbacks; late IRQ notifications must not address a freed thread. Completed teardown is synchronous. The HAL also exposes idempotent HIDS advertising control. |
| `ble_app.c`, `ble_glue.c` | Idempotent failed-start cleanup, with one owner for BLE app destruction. |
| `gap.c`, `gap.h`, `gap_int.h`, `gap_command.h` | Custom complete names and manufacturer data share a scan response; honest connection/disconnection state and timer/worker draining prevent stale events accessing freed state. The GAP worker refreshes HIDS advertising on request. |
| `furi_ble/gatt.c`, `services/battery_service.c` | Partial GATT creation reports failure and unwinds allocations; the app cannot implement this inside the existing firmware GATT allocator. |
| `lib/ble_profile/extra_services/hid_service.c` | Native HID creation unwinds partial allocations and unregisters its callback before freeing the service. The callback acknowledges only its own attribute range and does not swallow serial subscriptions/RX writes or other services' indication confirmations. |
| `target.json`, `api_symbols.csv` | Existing GAP state query, public service-header lookup, profile references, status registration, and HIDS advertising exports. No AirBridge-specific exports. |

## Exit And Faults

The main FAP thread supervises a separate I/O worker. Only explicit startup,
send, reconnect and cleanup operations have deadlines. Idle time and waiting for
a browser are not failures. Actual pairing input pauses BLE deadlines, not USB
deadlines. Numeric comparison resumes the deadline when the user answers.
Advertising supervision runs across all screens and waits for queued GAP work to
complete, including background advertising interval changes.

Initial budgets are 5 seconds for USB and 20 seconds for BLE/Closing, sampled every
25 ms. These are conservative progress thresholds, not measured hardware-fault
proof. BLE warnings precede the native 33-second HCI assertion when the scheduler
and GUI remain responsive. Hardware measurements must validate the thresholds.

Normal exit stops new work, displays `Closing...`, waits for a committed display
frame, then synchronously restores USB and Bluetooth. Callback contexts, queue,
USB semaphores, app state, and executable code stay alive until cleanup completes.
USB semaphores also survive automatic USB reinitialization during a session.

Deploy emits each key-down and key-up in one typing step, before entering any
BLE call. With functioning USB, a BLE wait cannot leave a key held for host
auto-repeat. Queued BACK
input is consumed before another character can be emitted after that wait.
A blocked BLE call can still delay the abort screen transition; keyboard
emission is already stopped during the wait.

A missed deadline latches `Operation stalled / No progress detected / Restart
device manually` and requests cooperative exit. It never kills the I/O thread,
frees a live owner, automatically resets the device, or restarts traffic. Late
completion may finish normal cleanup and return to the desktop.

This is not universal Bluetooth fault detection. A native assertion, CPU2
hardfault, stalled scheduler, or blocked GUI can prevent the warning from appearing.
Those cases require a manual full-device restart. Existing on-loop link watchdogs
still handle ordinary advertising/reconnect situations; they are not the fault
supervisor.

## Compatibility And Verification

The ownership refactor at API 87.14 intentionally removed the old product exports
without a major bump. Current API **87.15** includes the HIDS advertising control.
Only minor API bumps are permitted. Rebuild and deploy firmware plus Pocket
AirBridge together. Old AirBridge FAPs are unsupported; unrelated API exports
and loader validation are preserved.

Run `python3 airbridge/tests/run_tests.py`, `./fbt`, and
`./fbt fap_pocket_airbridge`. Host tests include an independently blocked cleanup
thread through the production lifecycle controller, retained ownership, late
completion, pairing pauses, tick wrap and stale clock samples, callback draining,
the real USB driver's reinitialization and detach paths, protocol crypto checks,
browser transport races, and identity generation.

Host success is not hardware acceptance. Required device checks remain bidirectional
text, file SHA-256, USB/BLE Deploy and BACK abort, browser protocol harness,
active-link exit/relaunch/reconnect, and stalled-operation warning/late cleanup.

### Verification Record (2026-09-15)

The BLE Deploy restoration and shared HID event-routing fix are in `90a19db1`.
Firmware/FAP builds, 16 host C modules, 213 JavaScript tests, 14 bundle tests,
and the 275/275 browser harness passed. The new dispatcher regression fails on
the original HID handler and passes with either registration order after the
fix, including under sanitizers. The owner reported “all works perfectly” after
the firmware update. See [troubleshooting](troubleshooting.md) for the full
incident record and distinction between recorded tests and owner-reported hardware
results. The earlier pending deployment notes below describe the September 7
session, not the current restoration status.

### Verification Record (2026-09-07)

- Host runner: ten native test programs, the input-wrap test, seven Python
  invariants, two identity checks, and twelve Node tests passed.
- Real USB/BLE module tests, the production cleanup controller, and typing/screen
  policy tests also passed with AddressSanitizer and UndefinedBehaviorSanitizer.
  The typing test models BACK at the resumption boundary; it does not exercise
  the real runtime/GUI input queue.
- Persistent Chrome protocol harness: 35/35 passed. Screenshot:
  `/tmp/airbridge-boundary-protocol-harness.png` (temporary evidence).
- Firmware and FAP builds passed, including normal SDK checks for API 87.14.
- `./fbt flash_usb` completed. The device subsequently reported API 87.14,
  `radio_alive: true`, and `system_lock: 0`.
- Exactly one AirBridge FAP was present before deployment, at the canonical
  `/ext/apps/Tools/pocket_airbridge.fap`. The new 57,192-byte FAP was uploaded.
  The subsequent size check/launch was interrupted; launch is not verified.
  A later Deploy key-release correction produced a 57,392-byte FAP that has
  not been uploaded. The on-device FAP must be replaced before acceptance tests.
- The FAP's code, read-only data, initialized data, BSS and unwind index total
  24,978 bytes, excluding loader metadata and relocation overhead. The I/O worker
  adds a 4,096-byte stack. Peak device heap use still needs measurement.
- Live text/file transfers, USB Deploy/abort, counters, exit/relaunch/reconnect,
  and hardware fault/late-cleanup checks remain pending. Device access became
  unavailable under the session's filesystem permissions before these checks.

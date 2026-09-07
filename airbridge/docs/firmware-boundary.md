# Firmware Ownership

Pocket AirBridge's USB profiles, BLE profile, DIS strings, serial UUIDs, GATT
service, relay routing, and reconnect policy live in
`applications_user/pocket_airbridge/`. Browser assets and their wire protocol are
unchanged. `gen_identity.py` reads the app-owned UUID header.

The comparison base is firmware commit `c9ab2b68`. The vendor submodule is back
at `133182d5583e998bb263cd947105be4df9c29cb3`. The USB HAL and `furi/core/` match
that base. The only remaining `lib/` source change is generic native HID
partial-creation cleanup, needed alongside the GATT allocator's failure reporting.
Normal advertising returns to the firmware's low-power interval after its initial
fast period.

Against `c9ab2b68`, the shared-source delta went from 49 paths (+4,128/-334)
at implementation baseline `3b253f12` to 19 paths (+1,111/-209): about 70% fewer
changed lines. This counts `applications/services/`, `targets/`, `furi/`, and
`lib/`, including the vendor gitlink; it excludes the app and product assets.

## Remaining Shared Changes

| Area | Reason it remains in firmware |
| --- | --- |
| `bt.c`, `bt_i.h`, `bt_profile_quiescence.h` | Service-owned profile publication and reader references prevent destruction during direct GATT operations. A mailbox avoids blocking the BLE event worker on the service queue. SD reload leaves external profiles alone; retries retain only the firmware's default profile. |
| `bt_api.c`, `bt.h`, `bt_status_registration.h` | Callback registration, ordered initial status, and draining must synchronize with the firmware dispatcher. A pairing-wait flag lets the app exclude human input from deadlines. |
| `furi_hal_bt.c`, `ble_event_thread.c` | Join the event worker before destroying service callbacks; late IRQ notifications must not address a freed thread. Completed teardown is synchronous. |
| `ble_app.c`, `ble_glue.c` | Idempotent failed-start cleanup, with one owner for BLE app destruction. |
| `gap.c`, `gap.h`, `gap_command.h` | Custom complete names and manufacturer data share a scan response; honest connection/disconnection state and timer/worker draining prevent stale events accessing freed state. |
| `furi_ble/gatt.c`, `services/battery_service.c` | Partial GATT creation reports failure and unwinds allocations; the app cannot implement this inside the existing firmware GATT allocator. |
| `lib/ble_profile/extra_services/hid_service.c` | Native HID creation unwinds partial allocations and unregisters its event callback before freeing the service, including allocation-failure paths. |
| `target.json`, `api_symbols.csv` | Existing GAP state query, public service-header lookup, profile references and status registration exports. No AirBridge-specific exports. |

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

API 87.14 intentionally removes the old product exports without a major bump.
Rebuild and deploy firmware plus Pocket AirBridge together. Old AirBridge FAPs
are unsupported; unrelated API exports and loader validation are preserved.

Run `python3 airbridge/tests/run_tests.py`, `./fbt`, and
`./fbt fap_pocket_airbridge`. Host tests include an independently blocked cleanup
thread through the production lifecycle controller, retained ownership, late
completion, pairing pauses, tick wrap and stale clock samples, callback draining,
the real USB driver's reinitialization and detach paths, protocol crypto checks,
browser transport races, and identity generation.

Host success is not hardware acceptance. Required device checks remain bidirectional
text, file SHA-256, USB Deploy and BACK abort, browser protocol harness,
active-link exit/relaunch/reconnect, and stalled-operation warning/late cleanup.

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
  `/ext/apps/USB/pocket_airbridge.fap`. The new 57,192-byte FAP was uploaded.
  The subsequent size check/launch was interrupted; launch is not verified.
  A later Deploy key-release correction produced a 57,392-byte FAP that has
  not been uploaded. The on-device FAP must be replaced before acceptance tests.
- The FAP's code, read-only data, initialized data, BSS and unwind index total
  24,978 bytes, excluding loader metadata and relocation overhead. The I/O worker
  adds a 4,096-byte stack. Peak device heap use still needs measurement.
- Live text/file transfers, USB Deploy/abort, counters, exit/relaunch/reconnect,
  and hardware fault/late-cleanup checks remain pending. Device access became
  unavailable under the session's filesystem permissions before these checks.

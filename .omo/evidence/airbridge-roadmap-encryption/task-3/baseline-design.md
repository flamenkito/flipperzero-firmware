## Task 3 baseline/design

- Live FAP path discovered before edits: `/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c` (see `live-fap-path.txt`).
- Existing Waiting-screen pump: when `app->screen == AirbridgeScreenWaiting` and the active deploy transport is BLE, the main loop periodically calls `furi_hal_bt_start_advertising()` every `BLE_WAITING_PUMP_MS` (`2500`) and separately kicks only the BLE Deploy squatter case after `BLE_WAITING_SQUATTER_KICK_MS`.
- Why Bridge lacked the refresh: the Bridge dispatch path had no periodic advertising maintenance at all; after the BLE profile was installed, only disconnect callbacks and BLE Deploy Waiting could restart advertising.
- GAP safety contract used: `furi_hal_bt_start_advertising()` calls `gap_start_advertising()` only when `gap_get_state() == GapStateIdle`; active advertising, pairing, or connected chat sessions are no-ops.
- Implementation: add `BLE_BRIDGE_ADV_WATCHDOG_MS`, `ble_bridge_last_watchdog_tick`, and `app_bridge_adv_watchdog()`, dispatched only from `AirbridgeScreenBridge`.
- Active session preservation: the Bridge watchdog returns immediately while `app->ble_connected` is true and does not call `bt_disconnect()`; it only invokes the existing idle-gated advertising start helper.
- Task 2 deploy safety preserved: `0x42` still starts streaming only from `AirbridgeScreenWaiting`; `AirbridgeScreenBridge` remains relay traffic and the watchdog does not touch relay data paths.

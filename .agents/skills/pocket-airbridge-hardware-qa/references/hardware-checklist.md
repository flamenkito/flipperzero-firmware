# Pocket AirBridge Hardware Evidence Checklist

Record observed values, not expected values.

## Build identity

- Firmware branch and commit:
- Firmware build result:
- FAP build result:
- FAP byte size on SD:
- Bootstrap byte size on SD:
- Bundle byte size on SD:

## Flipper

- App screen visible:
- USB identity shown:
- USB connected indicator:
- BLE active/connected indicator:
- Heartbeat observed:
- Initial `DROP` / `TXERR`:

## Host USB

- OS:
- Browser and version:
- Secure origin:
- Enumerated manufacturer/product:
- Enumerated VID/PID:
- WebHID picker selection confirmed:
- Report ID observed:

## Host BLE

- OS:
- Browser and version:
- Picker selection confirmed:
- Pairing confirmation completed:
- Serial service discovered:
- TX subscription active:
- RX write succeeded:

## Transfer matrix

- USB to BLE text:
- BLE to USB text:
- Attachment name, size, direction, SHA-256 result:
- Sender cancel:
- Receiver cancel:
- Disconnect behavior:
- Final `U->B` / `B->U` / `DROP` / `TXERR`:

## Verdict

- PASS only if both transports and all requested scenarios were observed.
- Record any skipped physical action or unavailable platform as unverified.

# USB Identity and Maintenance

The firmware boots as a passive composite HID identity. The default is Logitech;
Settings can select Logitech or Dell for the next boot. Desktop latches that RTC
selection and asks the CLI service to install the corresponding active identity
at startup (`applications/services/desktop/desktop.c:526-534`,
`targets/f7/furi_hal/furi_hal_usb_spoof.c:297-307`).

## Settings

Settings, System contains two separate entries:

- **USB Identity** offers `Logitech` and `Dell`. It saves the
  boot choice in RTC-backed state, so the identity applies at the next boot rather
  than switching a live USB device (`applications/settings/system/system_settings.c:94-108`,
  `targets/f7/furi_hal/furi_hal_rtc.c:263-281`).
- **Flipper USB** offers `OFF` and `ON`. It is volatile CDC intent, not
  a boot-identity setting, and it resets on reboot
  (`applications/settings/system/system_settings.c:110-128`).

The settings screen refuses a **Flipper USB** change while USB is busy. It restores
the displayed value to the current intent and logs the refusal, rather than
claiming a change that did not happen (`applications/settings/system/system_settings.c:115-124`).

The idle desktop status bar shows a quiet outlined USB glyph whenever the observed
USB interface is a spoof or FAP composite, or no interface is current. It shows a
filled USB warning glyph only while the observed interface is single or dual CDC;
this reports the host-visible state rather than the **Flipper USB** intent, including
the USB-UART legacy exception. **Pending hardware verification:** confirm both glyphs
and their periodic transition on the physical display.

## MAINTENANCE WORKFLOW

1. In Settings, System, set **Flipper USB** to `ON` to enable Flipper
   USB and CDC for maintenance.
2. Run qFlipper, the CLI, `./fbt flash_usb`, `scripts/storage.py`, or
   `scripts/runfap.py` as needed.
3. After the post-update reboot, CDC is gone by design because the boot identity
   is installed again. A maintenance tool can report a reconnect failure even
   when the update succeeded. **Pending hardware verification:** record the
   actual host enumeration and tool result during the hardware QA pass.
4. To recover maintenance CDC, set **Flipper USB** to `ON` again. If
   the UI is unavailable, enter ROM DFU with **BACK+LEFT**. Both recovery paths
   are designed to work independently of the active spoof identity. **Pending
   hardware verification:** exercise both paths on a physical device.

## COMPATIBILITY

Unmodified USB apps were not edited for this feature. BadUSB saves the active USB
interface before its run and restores it when freed
(`applications/main/bad_usb/bad_usb_app.c:151-165, 203-210`). U2F does the same
around its worker lifetime (`applications/main/u2f/u2f_hid.c:192-193, 284-289`).
The USB HID app also saves and restores the active interface around its run
(`applications/system/hid_app/hid.c:177-187`).

USB-UART bridge is the known exception. Its exit path unconditionally installs
`usb_cdc_single` and calls `cli_vcp_enable`
(`applications/main/gpio/usb_uart_bridge.c:300-309`; its initial VCP setup is at
`:107-116`). If it runs from keyboard mode, the host therefore sees CDC while
the **Flipper USB** setting still reads `OFF`: legacy caller state and the
volatile user intent have diverged. This does not create a self-restore trap.
When CLI VCP is already enabled and it observes CDC, its idempotent enable branch
returns without replacing that observed CDC interface
(`applications/services/cli/cli_vcp.c:237-267`).

Reconcile this exception by toggling **Flipper USB** `ON` then `OFF` once, or by
rebooting. Either path converges to the selected spoof identity. **Pending
hardware verification:** confirm the host's post-bridge enumeration and the
reconciliation on the target operating systems.

## CONTINUITY WITH POCKET AIRBRIDGE

The FAP keeps its existing `hp_kbd_vendor` default. No migration occurs
(`applications_user/pocket_airbridge/airbridge_config_defaults.c:46-50`,
`applications_user/pocket_airbridge/airbridge_usb.c:368-376`). For boot-to-FAP
continuity, set `/ext/apps_data/pocket_airbridge/config` once to either
`profile=logitech_kbd_vendor` or `profile=dell_kbd_vendor`. The config loader
accepts a profile label from that file, and both labels are shipped
(`applications_user/pocket_airbridge/airbridge_config.c:135-141, 184-215`,
`applications_user/pocket_airbridge/airbridge_usb.c:331-349`).

Stealth has priority over maintenance convenience. The normal boot path presents
the selected passive HID identity, while CDC is an explicit, temporary
maintenance choice.

## USB OWNERSHIP

While Pocket AirBridge runs, the FAP holds the USB mode lock. It takes over from
CLI VCP before configuring its profile and releases the lock during restore
(`applications_user/pocket_airbridge/airbridge_relay.c:152-189, 192-213`).
Settings therefore refuses a busy **Flipper USB** change until the FAP exits.

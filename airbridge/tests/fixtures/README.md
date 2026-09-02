# USB descriptor fixtures

`*.txt` files in this directory are evidence fixtures for W7's USB identity
comparison. They are intentionally plain text so code review and
`airbridge/tools/usb_descriptor_diff.py` can compare one normalized field per line.

## Format and provenance

Every fixture begins with the following comment header, followed by sections:

```text
# USB descriptor fixture format = 1
# captured_at_utc = 2026-07-21T12:34:56+00:00
# machine = host-name
# os_version = macOS-version (platform-string)
# tool = usb_descriptor_capture.py 1.0.0
# target = 0x03F0:0x5341
# command.system_profiler = system_profiler SPUSBDataType -detailLevel full
# command.ioreg = ioreg -p IOUSB -l -w 0
[normalized]
device.PID = 0x5341
device.VID = 0x03F0
...
[raw.system_profiler.target]
...
[raw.ioreg.target]
...
[raw.system_profiler.full]
...
[raw.ioreg.full]
...
```

The header's UTC capture time, machine, OS, tool version, VID:PID, and exact
command echo are mandatory provenance. `normalized` is sorted lexically and
is the only section read by the diff tool. Fields include the device descriptor
identity, descriptor-string values and indices, device class fields, EP0 packet
size, configuration count, and—when macOS exposes them—every interface,
endpoint, and HID report descriptor. `<unavailable>` means macOS did not expose
that field through the two recorded commands; it is not a claim that the value
is zero or absent from the hardware descriptor.

The raw target sections make review quick; the complete raw command output is
retained below them so the normalized parsing can be audited or improved later.
Never edit a capture's raw evidence to make a comparison pass. Take a new
capture after a device, OS, or tool change.

## macOS capture

Use a non-hub, directly visible target and preserve the generated file without
editing it. Run these commands from the monorepo root,
`/Users/asutov/projects/flipperzero-firmware`. The utility executes and embeds
both commands shown in its header:

```sh
python3 airbridge/tools/usb_descriptor_capture.py 03f0:5341 \
  --output airbridge/tests/fixtures/hp_03f0_5341_genuine.txt
python3 airbridge/tools/usb_descriptor_capture.py 03f0:5341 \
  --output airbridge/tests/fixtures/hp_03f0_5341_flipper.txt
python3 airbridge/tools/usb_descriptor_diff.py \
  airbridge/tests/fixtures/hp_03f0_5341_genuine.txt \
  airbridge/tests/fixtures/hp_03f0_5341_flipper.txt --explain
```

If two devices share the VID:PID, first obtain their `Location ID` from
`system_profiler SPUSBDataType -detailLevel full`, then rerun with the exact
hexadecimal value (for example, `--location-id 0x00150000`). The capture fails
loudly if the VID:PID is absent or still ambiguous. macOS often omits live
interface, endpoint, or HID-report-descriptor properties; in that case keep the
macOS fixture as provenance but collect the definitive descriptor evidence on
Windows below before declaring zero-diff identity.

## N50194 Windows capture: tree plus descriptor dump

The existing N50194 `Get-PnpDevice` evidence is a topology check only;
`USB\\VID_03F0&PID_5341` under the Microsoft **USB Composite Device** parent
(`usbccgp.sys`) is not a descriptor dump. Capture both artifacts while the
genuine dongle is the only `03F0:5341` device attached.

Open **PowerShell as Administrator** on N50194, change to the monorepo root,
and run this exact tree capture:

```powershell
Set-Location C:\path\to\flipperzero-firmware
```

```powershell
$id = (Get-PnpDevice -PresentOnly | Where-Object {
  $_.InstanceId -match '^USB\\VID_03F0&PID_5341'
} | Select-Object -First 1 -ExpandProperty InstanceId)
if (-not $id) { throw 'Genuine 03F0:5341 dongle not present' }
$parent = (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_Parent').Data
Get-PnpDevice -PresentOnly | Where-Object {
  $_.InstanceId -eq $id -or $_.InstanceId -eq $parent -or $_.InstanceId -like "$id&*"
} | Select-Object Status, Class, FriendlyName, InstanceId | Format-List |
  Out-File -Encoding utf8 airbridge\tests\fixtures\hp_03f0_5341_genuine_n50194_pnp.txt
Get-PnpDeviceProperty -InstanceId $id | Format-List KeyName, Type, Data |
  Out-File -Encoding utf8 -Append airbridge\tests\fixtures\hp_03f0_5341_genuine_n50194_pnp.txt
```

Then use the N50194 USB descriptor viewer (`descdump.exe`, if installed there,
or Microsoft USBView from the Windows SDK) against the **physical** `VID_03F0&PID_5341`
node—not the `usbccgp` parent—and save its complete device/configuration/HID
report descriptor text beside the tree capture:

```powershell
$descriptorDump = Get-Command descdump.exe -ErrorAction Stop
$descriptorDump.Source |
  Out-File -Encoding utf8 airbridge\tests\fixtures\hp_03f0_5341_genuine_n50194_descriptor_tool_path.txt
Start-Process -FilePath $descriptorDump.Source
```

The path file is provenance for the exact N50194 descriptor binary. Select the
physical `VID_03F0&PID_5341` device in its picker, then use its **Save** or
**Copy all** operation to write
`airbridge\tests\fixtures\hp_03f0_5341_genuine_n50194_descdump.txt`. Do not pipe a
GUI tool's startup output to `Out-File`: that captures no descriptors.

If N50194 has no `descdump.exe`, locate and launch the Windows SDK USBView
binary with this exact PowerShell fallback, record its resolved path in the
same provenance file, then select the physical node and choose **File → Save**
to the same `..._descdump.txt` target:

```powershell
$usbView = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10" -Recurse -Filter USBView.exe |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $usbView) { throw 'Install the Windows SDK USBView tool or descdump.exe' }
$usbView | Out-File -Encoding utf8 airbridge\tests\fixtures\hp_03f0_5341_genuine_n50194_descriptor_tool_path.txt
Start-Process -FilePath $usbView
```

The saved dump must include the device descriptor, each
configuration/interface/endpoint descriptor, and every HID report descriptor
before it can be normalized into the macOS-compatible fixture.

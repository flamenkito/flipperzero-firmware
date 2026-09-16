# ABT1 native TCP stream protocol

ABT1 carries one full-duplex TCP stream over the unchanged Pocket AirBridge
64-byte USB HID ↔ BLE serial relay. The executable is `abt`. This protocol is
separate from the browser AB2S/AB2W protocol and is not a browser crypto mode.
See [native usage](../native/README.md) and root ADR 0005 for the decision.

## Security and boundaries

ABT1 has **no encryption or peer authentication**. SSH or TLS must provide those
properties when required. CRC32 detects accidental damage; it is not a MAC.
Random stream IDs distinguish connection lifetimes, not authenticated peers.
A malicious transport peer can disrupt streams or forge ABT1 frames. SSH/TLS
must detect payload manipulation using their own authentication.

Each operator starts an endpoint. The listening endpoint accepts only loopback
TCP clients. The connecting endpoint dials only its locally configured target;
OPEN contains no destination address. Other local processes can use the listener.
No keyboard reports, browser bootstraps, device network interface, on-device keys,
whole-file buffers, or firmware changes are involved.

## Report layout

All integers are unsigned, big-endian. Every report is exactly 64 bytes. HIDAPI's
extra leading zero report-ID byte on writes is outside this wire layout.

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 4 | ASCII `ABT1` |
| 4 | 1 | Type |
| 5 | 1 | Reserved, zero |
| 6 | 1 | Payload length, 0–40 |
| 7 | 1 | Reserved, zero |
| 8 | 8 | Nonzero session ID |
| 16 | 4 | Sequence number |
| 20 | 40 | Payload followed by zero padding |
| 60 | 4 | IEEE CRC32 of bytes 0–59 |

CRC32 is the standard reflected IEEE polynomial (`0xedb88320`), initial state
`0xffffffff`, final XOR `0xffffffff`, as produced by `crc32fast::hash`.
Invalid length, magic, CRC, type, reserved bits, padding, zero session ID, or
capability payload causes the report to be discarded. Retries recover lost data.
The first byte is `0x41`, so these reports cannot match the device's `0x42` Deploy
trigger even outside Bridge mode. Operation still requires the Bridge screen.

| Type | Value | Sequence | Payload |
| --- | ---: | --- | --- |
| OPEN | 1 | 0 | `[window, 40]`: maximum window, DATA payload size |
| ACCEPT | 2 | 0 | `[window, 40]`: selected window, DATA payload size |
| DATA | 3 | Per-direction sequence | 1–40 bytes |
| ACK | 4 | Next expected sequence | Empty |
| FIN | 5 | Per-direction sequence | Empty |
| RESET | 6 | 0 | Empty |
| PING | 7 | 0 | Empty |
| PONG | 8 | 0 | Empty |

`abt` 0.2 accepts windows of **2 or 4**, with 4 the default maximum. The payload
size remains exactly 40. Other capabilities are rejected. Version 0.1 accepts
only `[2, 40]`; the report layout and DATA/control semantics are unchanged.

## Opening and connection lifetime

The listener chooses a random nonzero 64-bit session ID for each accepted TCP
connection. It repeats OPEN at the retry interval until ACCEPT, RESET, transport
failure, or the opening deadline. The connector attempts its fixed target once
for that ID. It drains duplicate OPENs while connecting; a new ID while busy gets
RESET. It sends ACCEPT only after the target connection succeeds. Failure sends
RESET and closes this attempt.

The first two OPEN attempts offer the configured maximum window. Subsequent
attempts offer 2 so a version 0.1 connector can accept; with default timers this
adds one second when connecting a new listener to an old connector. A connector
selects the smaller of its configured maximum and the first OPEN's window.
It latches that selection for the session, including while connecting the target.
ACCEPT reports the selection. A delayed ACCEPT of 4 is still valid after the
listener has started retrying with 2, if it originally offered 4. An ACCEPT
exceeding the listener's configured maximum fails the opening attempt.

A repeated OPEN during a running stream resends the same selected ACCEPT without
changing the window or opening another target socket. The last 16 completed/failed IDs are cached. Reopening a cached ID
gets RESET; duplicate DATA/FIN from a successfully completed ID gets its final
cumulative ACK. IDs outside that bounded cache are not permanent replay records.

Only one stream may run at a time. Extra local TCP clients are accepted and
closed immediately. SSH may multiplex many channels inside this one byte stream.
Physical disconnects do not resume streams or reconnect devices automatically.

## Delivery, flow control, and bounded memory

Each direction has independent sequence numbers starting at zero. DATA and FIN
consume one sequence number each. Neither side may wrap; exhaustion closes the
stream. ACK `N` confirms all DATA/FIN with sequence less than `N` have been written
to the receiving local TCP socket (or, for FIN, its write half was shut down).
An ACK is not proof the ultimate application has processed those bytes.

Each sender retains at most the negotiated **two or four** unacknowledged
DATA/FIN frames (80 or 160 payload bytes). It reads more
TCP bytes only when this window has room. Timed-out frames are retransmitted
with exactly the same bytes. A cumulative ACK beyond the next transmitted
sequence is a protocol error. Old ACKs have no effect.

The receiver normally sends a cumulative ACK after every two completed writes,
or when a 4 ms coalescing deadline expires, whichever happens first. FIN,
duplicates, and discarded future frames cause an immediate cumulative ACK.
Only completed local writes are acknowledged, even when an earlier copy is
still queued. `--ack-delay-ms 0` restores an ACK per completed frame; accepted
CLI values are 0–20 ms. The delay is also capped at one quarter of the retry
interval. This local ACK policy works with either negotiated window and with
version 0.1 senders; it needs no extra capability bit.

The receiver queues only the next in-order frame. Future frames are discarded
and receive the current cumulative ACK, so retries fill the gap. Duplicate
frames are never written twice. The most recent admitted frames, up to the
negotiated window size, are retained
for conflicting-retransmission detection; disagreement in a retained frame
resets the stream. Earlier duplicates are simply ACKed.

A separate TCP writer task prevents a slow local socket from blocking link ACKs
and heartbeats. Its queue has the negotiated window's capacity; the difference
between admitted and completed sequence numbers may never exceed that window.
Transport TX and RX channels
hold at most eight reports each. The USB driver limits each write burst to two
reports before checking reads. BLE writes are serialized. The device's existing
eight-event queue is unchanged. OS socket and Bluetooth buffers have their own
system-managed limits; ABT1 does not buffer a complete file or TCP stream.

## Half-close, liveness, and failure

Local TCP EOF sends sequenced FIN after previous bytes. The receiver completes
prior writes, shuts down only its local socket's write half, and ACKs FIN. Traffic
in the other direction continues. Once both EOFs are complete and all outgoing
frames are ACKed, the endpoint lingers for two retry intervals to answer late
FIN retries. The completed-ID cache handles subsequent duplicates.

| Timer | Default | Effect |
| --- | --- | --- |
| Retry | 500 ms | Resend unacknowledged DATA/FIN or OPEN |
| ACK coalescing | 4 ms | Flush a completed write without waiting for a second frame |
| Opening | 30 s | Fail if peer never accepts |
| Target connect | 10 s | Refuse OPEN if TCP target does not connect |
| Heartbeat | 2 s | PING after peer silence; matching peer responds PONG |
| Peer silence | 12 s | Fail if no valid current-session frame arrives |
| Delivery stall | 60 s | Fail if outstanding bytes make no ACK progress |
| Driver/send queue | 3 s | Fail a stalled transport write/queue |

Receiving any valid current-session frame refreshes peer liveness. ACK progress
refreshes delivery liveness; heartbeats alone cannot hide a blocked byte stream
forever. Active DATA/ACK traffic suppresses redundant PINGs so heartbeat bursts
do not compete with the full DATA window in the relay's eight slots. A blocked
TCP consumer still gets PING/PONG probes once its peer has gone quiet.
RESET, invalid stream sequencing, a socket error, or a deadline ends the
stream and attempts RESET. Socket halves and their writer task are released.
Transport closure ends the process; a target refusal permits a fresh session.

## Transport mapping

USB defaults to HP `03f0:5341`, vendor usage page `ff00`, usage `0001`, report ID
zero. A matching HID path may be selected explicitly. `0483` is refused. HIDAPI
uses shared-device mode on macOS. Both macOS and Windows builds select only the
vendor collection; the keyboard collection is never opened as a data pipe.

BLE scans for service `7b871228-baf0-c5b4-5f46-9c2613d627a3`, subscribes to TX
`87825ec0-7398-8cb7-3242-b083eaa34f27`, and writes RX
`152f7eeb-e3b7-5898-ba41-7ff66121c98d`. Write without response is preferred when
available; ABT1 ACKs supply delivery feedback. Incoming notifications must be
64 bytes. The macOS binary includes a Bluetooth usage description.

Transport and TCP roles are independent: either USB or BLE may listen locally
or connect to a fixed target. For Mac-to-WSL SSH, BLE listens on the Mac and USB
connects from the Windows host to WSL's SSH server through Windows localhost.
This changes neither the wire protocol nor the device's USB class. See the
[usage guide](../native/README.md#validation) for per-platform validation status.

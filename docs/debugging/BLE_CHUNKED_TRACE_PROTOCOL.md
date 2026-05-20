# BLE Chunked Trace Protocol – Current Firmware Branch

## Purpose

This branch implements the first robust BLE request/response protocol for the dummy hormone measurement firmware.

The firmware no longer sends only a single short response after receiving a command. Instead, it now supports:

1. Receiving a command as multiple BLE write fragments
2. Reconstructing the full command on the firmware side
3. Detecting command completion using a visible terminator character
4. Sending a longer trace response over multiple BLE notifications
5. Marking the trace response with explicit begin/end frames

This is an important step toward the final firmware goal:

```text
Flutter app writes measurement command
→ firmware reconstructs command
→ firmware generates fake trace data
→ firmware sends trace data back via BLE notifications
```

## BLE Roles

```text
Firmware / XIAO nRF54L15 = BLE Peripheral / GATT Server
Mobile App / nRF Connect = BLE Central / GATT Client
```

The firmware uses the Nordic UART Service pattern:

```text
NUS RX = app writes command fragments to firmware
NUS TX = firmware sends notifications back to app
```

## Current Command Input Protocol

A complete logical command is not expected to fit into one BLE write.

Instead, the central sends multiple smaller fragments.

Example command:

```text
START=-800;END=0;FREQ=100;RANGE=10
```

Manual test fragments sent through nRF Connect:

```text
START=-800;
END=0;
FREQ=100;
RANGE=10#
```

The `#` character is currently used as the command terminator.

Reason:

- nRF Connect text input does not reliably send `\n` as a real newline byte during manual testing.
- Typing `\n` sends two literal characters: backslash and `n`.
- `#` is visible, easy to type, and reliable for manual testing.

Later, the Flutter app may switch back to a real newline byte (`\n`) if desired.

## Firmware Input Behavior

For every non-final command fragment, the firmware responds with:

```text
FRAG_OK
```

Example:

```text
App writes: START=-800;
Firmware notifies: FRAG_OK
```

When the final fragment containing `#` is received, the firmware reconstructs the full command.

Example RTT log:

```text
received() - Len: 9, Fragment: RANGE=10#
Full command: START=-800;END=0;FREQ=100;RANGE=10
```

At that point, the firmware starts sending the trace response.

## Current Trace Output Protocol

The firmware now sends the trace response as a sequence of BLE notifications instead of one long notification.

The response is framed with:

```text
T_BEGIN
T=...
T=...
T=...
T_END
```

Meaning:

```text
T_BEGIN = trace transfer starts
T=...   = one trace data chunk
T_END   = trace transfer complete
```

Example successful transfer:

```text
T_BEGIN
T=-12,-10,-9,-4,3
T=12,30,18,5,-1,-3
T=-8,-11,-6,2,9,14
T=8,1,-2
T_END
```

The nRF Connect notification history confirmed that all packets were received successfully.

## Why Chunked Notifications Are Required

A BLE notification should not be assumed to carry an arbitrarily long response.

Earlier testing showed that longer BLE writes failed when treated as a single packet. The same architectural issue applies to firmware responses.

Therefore, the firmware must not assume:

```text
one response = one BLE notification
```

Instead, the correct model is:

```text
one logical trace response = multiple notification frames
```

This mirrors the input side:

```text
one logical command = multiple write fragments
```

## Current Firmware Architecture

The firmware separates two responsibilities:

### 1. Command reception

The `received()` callback handles incoming NUS RX writes.

Responsibilities:

- Log incoming fragments
- Append bytes to `rx_buf`
- Detect command terminator `#`
- Reconstruct full command
- Send `FRAG_OK` for intermediate fragments
- Start trace transfer after complete command

### 2. Trace transmission

Trace transmission is handled through a delayed work item instead of sending all notifications directly inside the RX callback.

This avoids coupling long response transmission too tightly to the BLE write callback.

Current response state machine:

```text
TRACE_TX_IDLE
TRACE_TX_BEGIN
TRACE_TX_DATA
TRACE_TX_END
```

Flow:

```text
TRACE_TX_BEGIN → send T_BEGIN
TRACE_TX_DATA  → send several T=... chunks
TRACE_TX_END   → send T_END
TRACE_TX_IDLE  → transfer complete
```

## Current Test Result

Successful RTT log:

```text
Sample - Bluetooth Peripheral NUS Fragmented Command Chunked Trace
Initialization complete
notif_enabled() - Enabled

received() - Len: 11, Fragment: START=-800;
Notify send: "FRAG_OK" result: 0

received() - Len: 6, Fragment: END=0;
Notify send: "FRAG_OK" result: 0

received() - Len: 9, Fragment: FREQ=100;
Notify send: "FRAG_OK" result: 0

received() - Len: 9, Fragment: RANGE=10#
Full command: START=-800;END=0;FREQ=100;RANGE=10
Starting trace transfer

Notify send: "T_BEGIN" result: 0
Notify send: "T=-12,-10,-9,-4,3" result: 0
Notify send: "T=12,30,18,5,-1,-3" result: 0
Notify send: "T=-8,-11,-6,2,9,14" result: 0
Notify send: "T=8,1,-2" result: 0
Notify send: "T_END" result: 0

Trace transfer finished
```

nRF Connect notification history confirmed successful reception of all trace packets.

## Flutter App Requirements

The Flutter app must treat TX notifications as a stream.

It must not assume that reading the final characteristic value gives the full trace.

Required behavior:

```text
Subscribe to NUS TX notifications before writing commands.
Write command fragments sequentially to NUS RX.
Wait for FRAG_OK after non-final fragments.
After final fragment, collect trace notifications.
On T_BEGIN: clear current trace buffer.
On T=...: parse and append trace values.
On T_END: mark trace complete.
```

Expected app-side protocol flow:

```text
write START=-800;
wait FRAG_OK

write END=0;
wait FRAG_OK

write FREQ=100;
wait FRAG_OK

write RANGE=10#
wait T_BEGIN
collect T=... chunks
wait T_END
parse complete trace
```

## Current Limitations

This is still an MVP protocol.

Known limitations:

1. The firmware does not yet parse command values.
2. The trace data is still hard-coded.
3. Only one trace transfer is supported at a time.
4. The `#` terminator is chosen for manual nRF Connect testing, not necessarily final production protocol.
5. There is no checksum, sequence number, or retry mechanism yet.
6. The app must rely on notification order and successful BLE delivery.
7. The trace chunk format is human-readable, not bandwidth-optimized.

## Next Reasonable Improvements

Recommended next steps:

1. Add chunk indices if debugging packet order becomes difficult.

Example:

```text
T0=-12,-10,-9
T1=-4,3,12
T2=30,18,5
```

2. Add command parsing.

Expected command format:

```text
START=-800;END=0;FREQ=100;RANGE=10
```

3. Generate trace data based on parsed command values.

4. Replace hard-coded trace with fake cyclic-voltammetry-like data.

5. Decide whether the final Flutter protocol should use:

```text
#
```

or a real newline byte:

```text
\n
```

6. Add timeout handling in Flutter for:

```text
FRAG_OK timeout
T_BEGIN timeout
T_END timeout
BLE disconnect during transfer
```

## Current Milestone

This branch proves the first realistic BLE communication architecture for the dummy device:

```text
fragmented command input
→ command reconstruction
→ multi-packet trace notification response
→ verified with nRF Connect notification history
```

This is no longer just a BLE sample. It is now the first working version of the project-specific firmware protocol.

## Suggested Commit Message

```bash
git commit -m "Implement chunked BLE trace response protocol" \
  -m "Add a framed multi-notification trace transfer using T_BEGIN, T=... chunks, and T_END. Keep fragmented command input via NUS RX with FRAG_OK acknowledgements and '#' command termination for reliable nRF Connect testing. Verified complete packet transfer through nRF Connect notification history."
```

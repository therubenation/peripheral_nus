# BLE Chunked Trace Protocol – Current Firmware Branch

## Purpose

This branch implements the first robust BLE request/response protocol for the dummy hormone measurement firmware.

The firmware supports:

1. Receiving a command as multiple BLE write fragments
2. Reconstructing the full command on the firmware side
3. Detecting command completion using a visible terminator character
4. Sending a longer trace response over multiple BLE notifications
5. Marking the trace response with explicit begin/end frames
6. Sending trace data as ordered voltage/current value pairs

# BLE Trace Protocol

This document describes the current client-facing BLE protocol used by the Flutter app to request and receive a cyclic-voltammetry-style trace from the firmware.

It is both:

1. a protocol contract for the Flutter mobile app, and
2. a firmware-facing best-practice guide for how trace data should be framed and transmitted over BLE.

The key architectural decision is that the firmware no longer sends anonymous y-values such as `T=-12,-10,-9`. A voltammetry trace is a sequence of voltage/current value pairs, so the BLE protocol must preserve that relationship explicitly.

---

## 1. Roles

```text
Firmware / device = BLE Peripheral / GATT Server
Flutter app       = BLE Central / GATT Client
```

The firmware uses a Nordic UART Service-style communication pattern:

```text
NUS RX = app writes command fragments to firmware
NUS TX = firmware sends notifications back to app
```

From the Flutter app perspective:

```text
Write command fragments to NUS RX.
Subscribe/listen to NUS TX notifications.
```

The app must not use BLE characteristic read as the source of trace data. Trace data is delivered as a notification stream.

---

## 2. Design Principles

The protocol is intentionally line-oriented, compact, and human-readable for the current MVP stage.

Important constraints:

```text
- BLE payloads are small.
- One BLE write is not necessarily one full command.
- One BLE notification is not necessarily one full trace.
- Trace data must preserve physical meaning.
- Flutter must be able to reconstruct a complete trace deterministically.
```

The protocol must avoid anonymous value streams such as:

```text
T=-12,-10,-9,-4,3
```

because these only represent current values and lose the physical relationship between voltage and current.

A cyclic-voltammetry trace point is a pair:

```text
voltage_mV -> current_nA
```

Therefore, the BLE trace protocol sends ordered voltage/current pairs.

---

## 3. Units and Value Semantics

The firmware does not send unit metadata in every response.

Instead, units are fixed by protocol contract:

```text
V = voltage in millivolts, integer
I = current in nanoamps, integer
```

Example:

```text
P0;V=-800;I=-21
```

Meaning:

```text
trace point index 0
voltage_mV = -800
current_nA = -21
```

Important naming decision:

```text
Use I = current_nA.
Do not call each y-value peak_current_nA.
```

Reason:

In cyclic voltammetry, each trace point represents measured current at a specific applied potential. Peak current is a derived feature of the whole curve, not the correct name for every y-value.

---

## 4. Command Write Protocol

A full command may be split into multiple BLE writes.

Example logical command:

```text
START=-800;END=0;FREQ=100;RANGE=10
```

Current manual-test terminator:

```text
#
```

Example BLE write fragments:

```text
START=-800;
END=0;
FREQ=100;
RANGE=10#
```

The `#` terminator marks the final command fragment.

Reason for `#`:

```text
# is currently chosen for reliable manual testing with nRF Connect.
Typing "\n" in nRF Connect may send two literal characters, backslash and n,
instead of a real newline byte.
```

The terminator may later be changed to a real newline byte if the app/firmware protocol changes.

---

## 5. Firmware Acknowledgements for Command Fragments

For each non-final command fragment, the firmware sends:

```text
FRAG_OK
```

Expected app behavior:

```text
write non-final fragment
wait for FRAG_OK
write next fragment
```

Example:

```text
App writes:        START=-800;
Firmware notifies: FRAG_OK

App writes:        END=0;
Firmware notifies: FRAG_OK

App writes:        FREQ=100;
Firmware notifies: FRAG_OK

App writes:        RANGE=10#
Firmware starts trace response
```

The app should send fragments sequentially, not in parallel.

---

## 6. Trace Notification Protocol

After the final command fragment, the firmware sends the trace over multiple NUS TX notifications.

The trace response is framed.

Frame format:

```text
TB;N=<point_count>
P<index>;V=<voltage_mV>;I=<current_nA>
P<index>;V=<voltage_mV>;I=<current_nA>
...
TE;N=<point_count>
```

Frame meanings:

```text
TB;N=...  = trace begin, expected number of points
P...      = one trace point
TE;N=...  = trace end, number of points sent
```

Example:

```text
TB;N=5
P0;V=-800;I=-21
P1;V=-770;I=-18
P2;V=-740;I=-14
P3;V=-710;I=-10
P4;V=-680;I=-7
TE;N=5
```

Each frame is sent as one BLE notification.

---

## 7. Why One Point Per Notification

For the current MVP, the firmware should send one trace point per notification:

```text
P0;V=-800;I=-21
```

Do not pack multiple points into one notification yet.

Avoid this for now:

```text
P0;V=-800;I=-21;P1;V=-770;I=-18
```

Reason:

```text
- harder to parse
- easier to exceed small BLE payload limits
- harder to debug manually in nRF Connect
- unnecessary at the current prototype stage
```

The current priority is correctness and observability, not bandwidth optimization.

Later, after MTU behavior and Flutter parsing are stable, the protocol may be optimized to pack multiple points per notification or use binary encoding.

---

## 8. Required Flutter App Behavior

The Flutter app must:

```text
1. Scan for the BLE device.
2. Connect to the device.
3. Discover services.
4. Find Nordic UART Service.
5. Subscribe to NUS TX notifications before writing commands.
6. Write command fragments sequentially to NUS RX.
7. Wait for FRAG_OK after every non-final fragment.
8. Send the final command fragment with # terminator.
9. Wait for TB;N=<count>.
10. Clear the current trace buffer on TB.
11. Parse every P<index>;V=<voltage_mV>;I=<current_nA> frame.
12. Store trace points by index.
13. Wait for TE;N=<count>.
14. Validate received point count and index continuity.
15. Convert the decoded points into the app's existing CvTrace structure.
```

The app must not assume:

```text
one notification = one complete trace
```

The correct model is:

```text
one trace = many notification frames
```

---

## 9. Flutter Trace Parsing Rules

### Begin frame

Example:

```text
TB;N=5
```

Expected behavior:

```text
- clear current trace buffer
- set expectedPointCount = 5
- enter ReceivingTrace state
```

### Point frame

Example:

```text
P2;V=-740;I=-14
```

Expected parsed values:

```text
index = 2
voltageMv = -740
currentNa = -14
```

Expected behavior:

```text
- validate index >= 0
- validate V exists and is integer
- validate I exists and is integer
- store point by index
```

Recommended app-side representation before mapping:

```dart
class BleTracePointDto {
  final int index;
  final int voltageMv;
  final int currentNa;

  const BleTracePointDto({
    required this.index,
    required this.voltageMv,
    required this.currentNa,
  });
}
```

Then map this DTO into the app/domain trace model.

### End frame

Example:

```text
TE;N=5
```

Expected behavior:

```text
- validate final count matches expectedPointCount
- validate exactly 5 points were received
- validate indices are continuous from 0 to 4
- sort by index if necessary
- emit complete trace
```

---

## 10. Expected End-to-End Flow

Command sent by app:

```text
START=-800;END=0;FREQ=100;RANGE=10
```

BLE write fragments:

```text
START=-800;
END=0;
FREQ=100;
RANGE=10#
```

Expected notification sequence:

```text
FRAG_OK
FRAG_OK
FRAG_OK
TB;N=5
P0;V=-800;I=-21
P1;V=-770;I=-18
P2;V=-740;I=-14
P3;V=-710;I=-10
P4;V=-680;I=-7
TE;N=5
```

Expected decoded trace points:

```text
[
  { voltage_mV: -800, current_nA: -21 },
  { voltage_mV: -770, current_nA: -18 },
  { voltage_mV: -740, current_nA: -14 },
  { voltage_mV: -710, current_nA: -10 },
  { voltage_mV: -680, current_nA: -7 }
]
```

---

## 11. App-Side State Machine

Recommended minimum BLE protocol states:

```text
Disconnected
Scanning
Connecting
DiscoveringServices
SubscribingToNotifications
Ready
SendingCommandFragment
WaitingForFragOk
WaitingForTraceBegin
ReceivingTrace
TraceComplete
Error
```

Valid high-level flow:

```text
Disconnected
-> Scanning
-> Connecting
-> DiscoveringServices
-> SubscribingToNotifications
-> Ready
-> SendingCommandFragment
-> WaitingForFragOk
-> SendingCommandFragment
-> WaitingForFragOk
-> SendingCommandFragment
-> WaitingForFragOk
-> SendingFinalCommandFragment
-> WaitingForTraceBegin
-> ReceivingTrace
-> TraceComplete
```

---

## 12. Required Failure Handling

The Flutter app should handle:

```text
missing Nordic UART Service
missing RX characteristic
missing TX characteristic
notifications not enabled
FRAG_OK timeout
TB timeout
TE timeout
BLE disconnect during command transfer
BLE disconnect during trace transfer
malformed frame
unknown notification frame
point frame before TB
TE before TB
duplicate point index
missing point index
non-continuous point indices
N mismatch between TB and TE
integer parse failure
unexpected response while idle
firmware busy response
firmware overflow response
```

Possible firmware error frames:

```text
ERR=RX_OVERFLOW
ERR=BUSY
```

Expected behavior:

```text
ERR=RX_OVERFLOW:
- discard current command attempt
- clear partial trace state
- report protocol error
- retry only from the beginning if appropriate

ERR=BUSY:
- do not send more fragments
- wait or retry later
```

---

## 13. Firmware Best Practices for This Protocol

The firmware should follow these implementation rules:

```text
1. Treat incoming BLE writes as fragments, not complete commands.
2. Accumulate command bytes until the # terminator is received.
3. Keep a fixed-size RX buffer and prevent overflow.
4. Send FRAG_OK only for non-final fragments.
5. Do not send long trace data directly inside the NUS received() callback.
6. Start a separate trace transfer state machine after full command reconstruction.
7. Use explicit trace framing: TB, P..., TE.
8. Send one trace point per notification for now.
9. Keep values as integers in fixed physical units.
10. Support only one active trace transfer at a time for the MVP.
11. Return ERR=BUSY if a second command arrives during an active trace transfer.
12. Log every outgoing frame and send result during firmware debugging.
```

The firmware should not use anonymous trace chunks anymore:

```text
T=-12,-10,-9,-4,3
```

Instead, it should use explicit point frames:

```text
P0;V=-800;I=-21
```

---

## 14. Recommended Firmware Data Model

Use an explicit point structure:

```c
struct trace_point {
    int16_t voltage_mv;
    int16_t current_na;
};
```

Example fake trace:

```c
static const struct trace_point fake_trace[] = {
    { -800, -21 },
    { -770, -18 },
    { -740, -14 },
    { -710, -10 },
    { -680, -7  },
};
```

Each notification should be generated from exactly one point:

```c
snprintk(line, sizeof(line),
         "P%d;V=%d;I=%d\n",
         index,
         fake_trace[index].voltage_mv,
         fake_trace[index].current_na);
```

The firmware should send:

```text
TB;N=5
P0;V=-800;I=-21
P1;V=-770;I=-18
P2;V=-740;I=-14
P3;V=-710;I=-10
P4;V=-680;I=-7
TE;N=5
```

---

## 15. Current MVP Limitations

The current firmware protocol is MVP-level.

Known limitations:

```text
- firmware may not yet parse command values
- trace data may still be hard-coded
- only one trace transfer is supported at a time
- # terminator may not be final
- no checksum
- no retry mechanism
- no binary encoding
- no compression
- app currently relies on notification order
- no explicit protocol version frame yet
```

These limitations are acceptable for the current prototype stage.

---

## 16. Future Protocol Improvements

Possible future improvements:

```text
1. Add protocol version frame.
2. Add checksum or CRC for full trace.
3. Add sequence numbers with explicit total count.
4. Add binary encoding for efficiency.
5. Add negotiated MTU support.
6. Add command parsing and realistic trace generation.
7. Add more realistic cyclic-voltammetry-like fake data.
8. Replace # terminator with newline if no longer needed.
9. Add cancellation command.
10. Add explicit measurement status frames.
```

Potential future frame examples:

```text
PROTO;V=1
TB;N=80;ID=42
P0;V=-800;I=-21
...
TE;N=80;ID=42;CRC=1234
```

Do not implement these prematurely. The current priority is a correct, observable, stable MVP protocol.

---

## 17. Current Protocol Summary

```text
Transport:
- Nordic UART Service over BLE

App role:
- BLE Central / GATT Client

Firmware role:
- BLE Peripheral / GATT Server

Write target:
- NUS RX

Notification source:
- NUS TX

Command terminator:
- #

Command fragments:
- START=-800;
- END=0;
- FREQ=100;
- RANGE=10#

Intermediate acknowledgement:
- FRAG_OK

Trace begin:
- TB;N=<point_count>

Trace point:
- P<index>;V=<voltage_mV>;I=<current_nA>

Trace end:
- TE;N=<point_count>

Units:
- V = millivolts
- I = nanoamps

Current trace model:
- ordered voltage/current point pairs
```

---

## 18. Critical Instruction for App Implementation

Do not implement this protocol as:

```text
write one long command
read one long trace response
```

That is wrong.

Implement it as:

```text
write command fragments to RX
receive acknowledgement notifications
receive trace frames from TX
reconstruct trace from TB/P/TE notification stream
```

The app must treat BLE notifications as a stream of protocol frames.

This is an important step toward the final firmware goal:

```text
Flutter app writes measurement command
→ firmware reconstructs command
→ firmware generates fake trace points
→ firmware sends voltage/current pairs back via BLE notifications
```

The important protocol upgrade in this branch is that the firmware no longer sends anonymous current-only chunks such as:

```text
T=-12,-10,-9,-4,3
```

Instead, it now sends explicit trace point frames:

```text
P0;V=-800;I=-21
```

That matters because a voltammetry trace is not just a list of y-values. It is an ordered sequence of applied-voltage / measured-current pairs.

---

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

From the central/client perspective:

```text
Write command fragments to NUS RX.
Subscribe to trace notifications on NUS TX.
```

---

## Current Command Input Protocol

A complete logical command is not expected to fit into one BLE write.

Instead, the central sends multiple smaller fragments.

Example logical command:

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

---

## Firmware Input Behavior

For every non-final command fragment, the firmware responds with:

```text
FRAG_OK
```

Example:

```text
App writes:        START=-800;
Firmware notifies: FRAG_OK
```

When the final fragment containing `#` is received, the firmware reconstructs the full command.

Example RTT log:

```text
received() - Len: 9, Fragment: RANGE=10#
Full command: START=-800;END=0;FREQ=100;RANGE=10
```

At that point, the firmware starts sending the trace response.

---

## Current Trace Output Protocol

The firmware sends the trace response as a sequence of BLE notifications instead of one long notification.

The response is framed with:

```text
TB;N=<point_count>
P<index>;V=<voltage_mV>;I=<current_nA>
P<index>;V=<voltage_mV>;I=<current_nA>
...
TE;N=<point_count>
```

Meaning:

```text
TB;N=...  = trace transfer begins, expected number of points
P...      = one ordered voltage/current trace point
TE;N=...  = trace transfer ends, number of points sent
```

Example successful transfer:

```text
TB;N=10
P0;V=-800;I=-21
P1;V=-770;I=-18
P2;V=-740;I=-14
P3;V=-710;I=-10
P4;V=-680;I=-7
P5;V=-650;I=-5
P6;V=-620;I=-3
P7;V=-590;I=-2
P8;V=-560;I=-1
P9;V=-530;I=0
TE;N=10
```

nRF Connect notification history confirmed that all packets were received successfully.

---

## Trace Value Semantics

The firmware does not send unit metadata in every response.

Units are fixed by protocol contract:

```text
V = voltage in millivolts, integer
I = current in nanoamps, integer
```

Example:

```text
P2;V=-740;I=-14
```

Meaning:

```text
trace point index = 2
voltage_mV = -740
current_nA = -14
```

Important terminology decision:

```text
Use I/current_nA for each trace y-value.
Do not call every y-value peak_current_nA.
```

Reason:

In cyclic voltammetry, each point is the measured current at a specific applied potential. Peak current is a feature derived from the complete curve, not the name of every point's y-value.

---

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

---

## Why One Point Per Notification

For the current MVP, the firmware sends one trace point per notification.

Example:

```text
P0;V=-800;I=-21
```

The firmware intentionally does not pack several points into one notification yet.

Avoid this for now:

```text
P0;V=-800;I=-21;P1;V=-770;I=-18
```

Reason:

- It is harder to parse.
- It is easier to exceed small BLE payload limits.
- It is harder to debug manually in nRF Connect.
- It is unnecessary at the current prototype stage.

The current priority is correctness, observability, and a clean protocol contract. Bandwidth optimization can come later.

---

## Current Firmware Architecture

The firmware separates two responsibilities.

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
TRACE_TX_BEGIN → send TB;N=<point_count>
TRACE_TX_DATA  → send one P<index>;V=<voltage_mV>;I=<current_nA> frame per point
TRACE_TX_END   → send TE;N=<point_count>
TRACE_TX_IDLE  → transfer complete
```

---

## Current Firmware Data Model

The firmware represents trace data as explicit voltage/current pairs.

Recommended model:

```c
struct trace_point {
    int16_t voltage_mv;
    int16_t current_na;
};
```

Example fake trace:

```c
static const struct trace_point fake_trace[] = {
    { -800, -21 },
    { -770, -18 },
    { -740, -14 },
    { -710, -10 },
    { -680, -7  },
    { -650, -5  },
    { -620, -3  },
    { -590, -2  },
    { -560, -1  },
    { -530,  0  },
};
```

Each trace point is serialized as:

```text
P<index>;V=<voltage_mV>;I=<current_nA>
```

Example:

```text
P0;V=-800;I=-21
```

---

## Current Test Result

Successful RTT log:

```text
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

Notify send: "TB;N=10" result: 0
Notify send: "P0;V=-800;I=-21" result: 0
Notify send: "P1;V=-770;I=-18" result: 0
Notify send: "P2;V=-740;I=-14" result: 0
Notify send: "P3;V=-710;I=-10" result: 0
Notify send: "P4;V=-680;I=-7" result: 0
Notify send: "P5;V=-650;I=-5" result: 0
Notify send: "P6;V=-620;I=-3" result: 0
Notify send: "P7;V=-590;I=-2" result: 0
Notify send: "P8;V=-560;I=-1" result: 0
Notify send: "P9;V=-530;I=0" result: 0
Notify send: "TE;N=10" result: 0

Trace transfer finished
```

nRF Connect notification history confirmed successful reception of all trace packets.

---

## Flutter App Requirements

The Flutter app must treat TX notifications as a stream.

It must not assume that reading the final characteristic value gives the full trace.

Required behavior:

```text
Subscribe to NUS TX notifications before writing commands.
Write command fragments sequentially to NUS RX.
Wait for FRAG_OK after non-final fragments.
After final fragment, collect trace notifications.
On TB;N=<count>: clear trace buffer and store expected point count.
On P<index>;V=<voltage_mV>;I=<current_nA>: parse and store trace point by index.
On TE;N=<count>: validate count and complete trace transfer.
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
wait TB;N=10
collect P... frames
wait TE;N=10
parse complete trace
```

The app should validate:

```text
TB count equals TE count
received point count equals expected count
point indices are continuous from 0 to N - 1
each point contains V and I
V and I parse as integers
```

---

## Current Limitations

This is still an MVP protocol.

Known limitations:

1. The firmware does not yet parse command values.
2. The trace data is still hard-coded.
3. Only one trace transfer is supported at a time.
4. The `#` terminator is chosen for manual nRF Connect testing, not necessarily final production protocol.
5. There is no checksum, sequence number, or retry mechanism yet.
6. The app must rely on notification order and successful BLE delivery.
7. The trace point format is human-readable, not bandwidth-optimized.
8. There is no protocol version frame yet.

---

## Next Reasonable Improvements

Recommended next steps:

1. Update the Flutter app to parse the new point-frame protocol.

Expected frames:

```text
TB;N=10
P0;V=-800;I=-21
...
TE;N=10
```

2. Add command parsing in firmware.

Expected command format:

```text
START=-800;END=0;FREQ=100;RANGE=10
```

3. Generate trace points based on parsed command values.

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
TB timeout
TE timeout
BLE disconnect during transfer
```

7. Later, consider protocol versioning:

```text
PROTO;V=1
```

8. Later, consider checksum/CRC for full-trace validation.

Do not optimize packet density or switch to binary too early. The current text protocol is still useful because it is visible and debuggable in nRF Connect.

---

## Current Milestone

This branch proves the first realistic BLE communication architecture for the dummy device:

```text
fragmented command input
→ command reconstruction
→ multi-packet trace notification response
→ explicit voltage/current point frames
→ verified with nRF Connect notification history
```

It is now the first working version of the project-specific firmware protocol for command-driven trace transfer.

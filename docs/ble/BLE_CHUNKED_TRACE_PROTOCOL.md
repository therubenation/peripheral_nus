# BLE Chunked Trace Protocol – Current Firmware Branch

## Purpose

This firmware branch implements a BLE dummy device that:

1. Receives a measurement command over BLE in multiple write fragments
2. Reconstructs the full command on the device
3. Sends a trace response back over multiple BLE notifications
4. Represents trace data as ordered voltage/current pairs with scale factors

The firmware uses Nordic UART Service-style communication.

---

## BLE Roles

```text
Firmware / XIAO nRF54L15 = BLE Peripheral / GATT Server
Mobile app / nRF Connect = BLE Central / GATT Client
```

```text
NUS RX = central writes command fragments to firmware
NUS TX = firmware sends notifications back to central
```

---

## Command Input Protocol

A full command may be split into several BLE writes.

Example logical command:

```text
START=-800;END=0;FREQ=100;RANGE=10
```

Current fragment format:

```text
START=-800;
END=0;
FREQ=100;
RANGE=10#
```

The `#` character marks the final fragment.

For every non-final fragment, firmware responds with:

```text
FRAG_OK
```

Example flow:

```text
write START=-800;  -> FRAG_OK
write END=0;       -> FRAG_OK
write FREQ=100;    -> FRAG_OK
write RANGE=10#    -> starts trace response
```

---

## Trace Output Protocol

After the final command fragment, firmware sends the trace as a notification sequence.

Frame format:

```text
B<count>
S<x_scale>,<y_scale>
<index>,<voltage_mv_scaled>,<current_na_scaled>
...
E<count>
```

Where:

```text
B   = trace begin, count appended directly (e.g. B35)
S   = scale metadata: x_scale then y_scale, comma-separated
idx = trace point index (0-based)
V   = voltage scaled integer
I   = current scaled integer
E   = trace end, count appended directly (e.g. E35)
```

All frames are ≤ 18 bytes and fit the default BLE payload without MTU negotiation.

---

## Scaled Values

`V` and `I` in data frames are **scaled integers**, not direct physical values.

To recover physical units, divide by the scale factors from the S frame:

```text
real_voltage_mV = V / XS
real_current_nA = I / YS
```

Example using `S1000,1000000`:

```text
0,-340040,406027  →  -340040 / 1000 = -340.040 mV,  406027 / 1000000 = 0.406027 nA
```

---

## Example Trace Response

```text
B35
S1000,1000000
0,-340040,406027
1,-330040,385046
2,-320041,374079
3,-310040,387907
4,-300040,367403
5,-290040,371695
6,-280040,387430
7,-270041,413179
8,-260040,455618
9,-250040,509024
10,-240040,583887
11,-230040,652080
12,-220040,734572
13,-210040,804191
14,-200118,817542
15,-190118,830894
16,-180118,796561
17,-170117,740294
18,-160118,673537
19,-150118,592470
20,-140118,535727
21,-130117,481367
22,-120117,446558
23,-110118,417948
24,-100118,395060
25,-90117,385046
26,-80117,382185
27,-70117,378370
28,-60196,382185
29,-50196,377417
30,-40195,378370
31,-30195,379801
32,-20195,393152
33,-10196,395536
34,-196,403643
E35
```

---

## Firmware Behavior

### Command reception

The `received()` callback:

- receives NUS RX writes
- treats each write as a fragment
- appends fragment bytes to `rx_buf`
- detects `#` as the command terminator
- logs the reconstructed command
- sends `FRAG_OK` for non-final fragments
- starts trace transmission after the full command is received

### Trace transmission

Trace transmission is handled by a delayed work item.

The trace transfer state machine is:

```text
TRACE_TX_IDLE
TRACE_TX_BEGIN
TRACE_TX_SCALE
TRACE_TX_DATA
TRACE_TX_END
```

Flow:

```text
TRACE_TX_BEGIN -> send B<count>
TRACE_TX_SCALE -> send S<x_scale>,<y_scale>
TRACE_TX_DATA  -> send one data frame per trace point
TRACE_TX_END   -> send E<count>
TRACE_TX_IDLE  -> transfer finished
```

---

## Current Trace Data Model

The firmware stores trace data as scaled integer pairs:

```c
struct trace_point {
    int32_t voltage_mv_scaled;
    int32_t current_na_scaled;
};
```

`int32_t` is required because scaled voltage values reach ±340,040, which overflows `int16_t`.

---

## Verified Test Scenario

Tested with nRF Connect:

1. Enable notifications on NUS TX
2. Write command fragments to NUS RX:

```text
START=-800;
END=0;
FREQ=100;
RANGE=10#
```

3. Confirm notification sequence begins with:

```text
B35
S1000,1000000
```

4. Confirm 35 data frames arrive and end with `E35`
5. Spot-check scale reconstruction using `XS=1000`, `YS=1000000`:
   - `0,-340040,406027` → `-340.040 mV`, `0.406027 nA`
   - `15,-190118,830894` → `-190.118 mV`, `0.830894 nA`
   - `34,-196,403643` → `-0.196 mV`, `0.403643 nA`
6. RTT log confirms `result: 0` for all `Notify send:` lines

---

## Current Limitations

```text
- command values are not parsed yet
- trace data is hard-coded (35 real measurement points)
- only one trace transfer is supported at a time
- # is used as command terminator for manual testing
- no checksum or retry mechanism yet
- no protocol version frame yet
- Flutter receiver not yet implemented
```

---

## Next Steps

Recommended next firmware steps:

1. Parse command values from the reconstructed command
2. Add validation for malformed commands
3. Add protocol versioning if the app integration becomes stable
4. Add checksum or transfer ID later if needed

Recommended mobile app steps:

1. Subscribe to NUS TX before writing
2. Write command fragments sequentially to NUS RX
3. Wait for `FRAG_OK` after non-final fragments
4. On `B<count>`: start transfer, store expected point count
5. On `S<xs>,<ys>`: store scale factors
6. On `<idx>,<V>,<I>`: extract and scale: `real_mV = V / xs`, `real_nA = I / ys`
7. On `E<count>`: validate count matches B, finish transfer
8. Map decoded points into the app trace model

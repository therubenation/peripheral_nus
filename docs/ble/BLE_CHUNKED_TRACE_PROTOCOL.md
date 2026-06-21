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
TB;N=<point_count>;XS=<x_scale>;YS=<y_scale>
P<index>;V=<voltage_mv_scaled>;I=<current_na_scaled>
...
TE;N=<point_count>
```

Where:

```text
TB  = trace begin
N   = number of trace points
XS  = x-axis scale factor (integer)
YS  = y-axis scale factor (integer)
P   = one trace point
V   = voltage scaled integer
I   = current scaled integer
TE  = trace end
```

---

## Scaled Values

`V` and `I` in P-frames are **scaled integers**, not direct physical values.

To recover physical units, divide by the scale factors from the TB header:

```text
real_voltage_mV = V / XS
real_current_nA = I / YS
```

Example using current scale factors (`XS=1000`, `YS=1000000`):

```text
P0;V=-340040;I=406027  →  -340040 / 1000 = -340.040 mV,  406027 / 1000000 = 0.406027 nA
```

Flutter receiver implementation is a separate future task.

---

## Example Trace Response

```text
TB;N=35;XS=1000;YS=1000000
P0;V=-340040;I=406027
P1;V=-330040;I=385046
P2;V=-320041;I=374079
P3;V=-310040;I=387907
P4;V=-300040;I=367403
P5;V=-290040;I=371695
P6;V=-280040;I=387430
P7;V=-270041;I=413179
P8;V=-260040;I=455618
P9;V=-250040;I=509024
P10;V=-240040;I=583887
P11;V=-230040;I=652080
P12;V=-220040;I=734572
P13;V=-210040;I=804191
P14;V=-200118;I=817542
P15;V=-190118;I=830894
P16;V=-180118;I=796561
P17;V=-170117;I=740294
P18;V=-160118;I=673537
P19;V=-150118;I=592470
P20;V=-140118;I=535727
P21;V=-130117;I=481367
P22;V=-120117;I=446558
P23;V=-110118;I=417948
P24;V=-100118;I=395060
P25;V=-90117;I=385046
P26;V=-80117;I=382185
P27;V=-70117;I=378370
P28;V=-60196;I=382185
P29;V=-50196;I=377417
P30;V=-40195;I=378370
P31;V=-30195;I=379801
P32;V=-20195;I=393152
P33;V=-10196;I=395536
P34;V=-196;I=403643
TE;N=35
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
TRACE_TX_DATA
TRACE_TX_END
```

Flow:

```text
TRACE_TX_BEGIN -> send TB;N=<point_count>;XS=<x_scale>;YS=<y_scale>
TRACE_TX_DATA  -> send one P frame per trace point
TRACE_TX_END   -> send TE;N=<point_count>
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
TB;N=35;XS=1000;YS=1000000
```

4. Confirm 35 P-frames arrive and end with `TE;N=35`
5. Spot-check scale reconstruction:
   - `P0;V=-340040;I=406027` → `-340.040 mV`, `0.406027 nA`
   - `P15;V=-190118;I=830894` → `-190.118 mV`, `0.830894 nA`
   - `P34;V=-196;I=403643` → `-0.196 mV`, `0.403643 nA`
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
4. Parse `TB` header — extract `N`, `XS`, `YS`
5. Parse `P` frames — extract index, `V`, `I`
6. Apply scale: `real_mV = V / XS`, `real_nA = I / YS`
7. Validate point count and point indices
8. Map decoded points into the app trace model

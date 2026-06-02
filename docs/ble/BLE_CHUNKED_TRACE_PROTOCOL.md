# BLE Chunked Trace Protocol – Current Firmware Branch

## Purpose

This firmware branch implements a BLE dummy device that:

1. Receives a measurement command over BLE in multiple write fragments
2. Reconstructs the full command on the device
3. Sends a trace response back over multiple BLE notifications
4. Represents trace data as ordered voltage/current pairs

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
TB;N=<point_count>
P<index>;V=<voltage_mV>;I=<current_nA>
...
TE;N=<point_count>
```

Example:

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

Meaning:

```text
TB = trace begin
N  = number of trace points
P  = one trace point
V  = voltage in millivolts
I  = current in nanoamps
TE = trace end
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
TRACE_TX_BEGIN -> send TB;N=<point_count>
TRACE_TX_DATA  -> send one P frame per trace point
TRACE_TX_END   -> send TE;N=<point_count>
TRACE_TX_IDLE  -> transfer finished
```

---

## Current Trace Data Model

The firmware stores fake trace data as voltage/current pairs:

```c
struct trace_point {
    int16_t voltage_mv;
    int16_t current_na;
};
```

Example:

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

Each point is serialized as:

```text
P<index>;V=<voltage_mV>;I=<current_nA>
```

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

3. Confirm notification sequence:

```text
FRAG_OK
FRAG_OK
FRAG_OK
TB;N=10
P0;V=-800;I=-21
...
P9;V=-530;I=0
TE;N=10
```

The notification history showed successful reception of all frames.

---

## Current Limitations

```text
- command values are not parsed yet
- trace data is hard-coded
- only one trace transfer is supported at a time
- # is used as command terminator for manual testing
- no checksum or retry mechanism yet
- no protocol version frame yet
- text protocol is optimized for debuggability, not bandwidth
```

---

## Next Steps

Recommended next firmware steps:

1. Parse command values from the reconstructed command
2. Generate fake trace points based on command values
3. Add validation for malformed commands
4. Add protocol versioning if the app integration becomes stable
5. Add checksum or transfer ID later if needed

Recommended mobile app steps:

1. Subscribe to NUS TX before writing
2. Write command fragments sequentially to NUS RX
3. Wait for `FRAG_OK` after non-final fragments
4. Parse `TB`, `P`, and `TE` notification frames
5. Validate point count and point indices
6. Map decoded points into the app trace model

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
START=-800;END=0;FREQ=100;RANGE=10;CHANNEL=7
```

Current fragment format:

```text
START=-800;
END=0;
FREQ=100;
RANGE=10;
CHANNEL=7#
```

Field units (for when `START`/`END`/`FREQ`/`RANGE` parsing is implemented —
see "Next Steps" below): `START`/`END` in mV, `FREQ` in Hz, `RANGE` in **µA**
(changed from nA). Do not confuse `RANGE`'s µA with the unrelated nA unit
used for *measured* trace-output current (`current_na_scaled`, the `I`
value in `<idx>,<V>,<I>` output frames below) — that's a different
quantity, unaffected by this change.

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
write RANGE=10;    -> FRAG_OK
write CHANNEL=7#   -> starts trace response
```

---

## Channel Field

`CHANNEL=<0..255>;` is a runtime device-routing / electrode-selection
parameter for the measurement about to run. Firmware treats it as an opaque
integer — it has no knowledge of, and does not need, what hormone, cartridge,
or analyte the channel corresponds to; that mapping lives upstream (mobile
app / backend).

- **Range:** integer `0..255` inclusive.
- **Optional:** a command without `CHANNEL` is accepted exactly as before —
  absence is not an error and is not treated as channel `0`.
- **Position:** may appear anywhere among the `;`-delimited fields, not only
  last.
- **Validation:** must be a plain base-10 integer with no extra characters
  (no `+`, no whitespace, no trailing garbage) and in range. Invalid values
  are rejected, not clamped.
- **Error response:** if `CHANNEL` is present but invalid, firmware replies
  `ERR=CHANNEL_INVALID` and does **not** start a trace transfer for that
  command.
- **Where it's applied:** the validated value is stored in `current_channel`
  (`src/main.c`) and logged at the start of trace transmission
  (`TRACE_TX_BEGIN`, `src/main.c`). **This firmware has no multiplexer,
  analog switch, or ADC-channel driver** — there is currently no hardware
  path to physically route a measurement to a specific electrode channel.
  Applying `CHANNEL` to real hardware is a separate, not-yet-implemented
  piece of work (chip selection, devicetree overlay, driver code).
- Parsing/validation lives in `src/cmd_parser.c` / `src/cmd_parser.h`, kept
  free of Zephyr dependencies so it can be unit-tested as a host binary
  (`tests/cmd_parser/test_cmd_parser.c`).

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

## Multi-Channel Measurement Request (UC8)

One request carries the configurations for up to 6 electrode channels. The
firmware owns the procedure: it runs the acquisitions and returns one
separately identifiable dataset block per requested slot.

This section is the shared contract between the firmware and the mobile app.
Firmware has no notion of hormones, analytes, cartridges or calibration; it
only knows slots, channels and measurement parameters.

### Request

```text
REQ=<id>;N=<n>;SLOT=0;CHANNEL=<ch>;START=<mV>;END=<mV>;FREQ=<Hz>;RANGE=<µA>;SLOT=1;...;RANGE=<µA>#
```

- **Legacy vs multi:** a command containing a `REQ=` field is a multi request.
  A command without it is a legacy single-channel command and behaves exactly
  as described above.
- **Header:**
  - `REQ` is a request ID from 1 to 65535, chosen by the app and echoed back.
  - `N` is the number of slots, from 1 to 6.
  - Both must appear before the first `SLOT`, in either order.
- **Slots:**
  - `SLOT=k` opens slot k. Slots must be numbered 0, 1, …, N−1 in order.
  - Each slot needs exactly one of each of `CHANNEL`, `START`, `END`, `FREQ`
    and `RANGE`, in any order. There must be no duplicates and no other keys.
- **Units:** the same as the legacy command:
  - `START` and `END` in mV
  - `FREQ` in Hz
  - `RANGE` in µA, the full-scale current range (not the nA used in the
    trace output)
- **Transport:** the same fragment, `FRAG_OK` and `#` mechanism as above.
  - Fragment boundaries are arbitrary; a write may even split a field.
  - Each write must be ≤ 20 bytes (default ATT MTU).
  - A 6-slot request is about 340 bytes, and the RX buffer holds 511.
  - A single trailing `;` before `#` is allowed.

### Slot vs channel

- **Slot** (0..N−1) is the identity of a dataset within one request. The app
  associates each dataset with its analyte by slot.
- **Channel** (0..255) is the physical electrode routing. It is echoed in the
  `C` frame only for verification.
- **The firmware accepts the same channel in several slots.** The app's
  cartridge configuration currently forbids duplicate channels. That is a
  stricter rule at the application level, and the two are compatible:
  because identity is the slot, lifting the app rule needs no firmware
  change.

### Response frames

Every frame is ≤ 20 bytes and ends with `\n`.

| Frame | Meaning |
|---|---|
| `R<id>,<n>,<t>` | Request accepted. `t` is the firmware's upper bound in seconds on the time until `D`, covering all acquisition and transfer |
| `C<slot>,<ch>` | Dataset block header. It is sent **only after that slot's acquisition has finished**. It is followed by the unchanged `B<count>`, `S<xs>,<ys>`, `<idx>,<V>,<I>`…, `E<count>` |
| `X<slot>,<code>` | The device states the slot produced no dataset. `ACQ_FAIL`: acquisition was attempted and failed; the electrode may be affected. `SKIPPED`: acquisition never started; the electrode was not touched (reserved, not emitted yet) |
| `D<id>,<ok>,<x>` | Procedure finished, with `ok + x == N` |
| `V<slot>,<key>,<code>` | Request rejected because of a slot field. Terminal: nothing was acquired |
| `ERR=<code>` | Request rejected at request level. Terminal: nothing was acquired |

Example (3 slots, simulated failure on channel 4):

```text
R42,3,19
C0,1
B35
S1000,1000000
0,-340040,406027
...
E35
X1,ACQ_FAIL
C2,7
B35
...
E35
D42,2,1
```

**Guarantees:**
- Blocks (`C` … `E`) never interleave.
- Slot order is **unspecified**, so the app must key datasets by slot.
- Every slot gets exactly one complete block or exactly one `X`, and then
  `D` follows.

**Deliberately left open:**
- *When* blocks are sent. The firmware may stream each block as soon as that
  slot is acquired, or stay silent during acquisitions and send several
  blocks later (for example, if the radio must be quiet while measuring).
- *How many* datasets the firmware buffers. The dummy firmware streams each
  block right after its simulated acquisition and buffers one.
- The only obligation is that `t` covers whichever strategy is used.

### Validation

Everything is validated **before** `R`. A rejected request never acquires
anything.

| Level | Checks | Rejection |
|---|---|---|
| 1. Syntax (final) | `REQ` and `N` present, before any `SLOT`, not duplicated, `REQ` in 1..65535 | `ERR=SYNTAX` |
| | `N` in 1..6, number of slots equals `N` | `ERR=SLOT_COUNT` |
| | `SLOT` sequence 0..N−1 in order, no empty or `=`-less field outside a slot | `ERR=SYNTAX` |
| | Each slot has all five keys, none duplicated, no unknown keys, strict base-10 int32 values | `V<slot>,<key>,MISSING` / `DUP` / `SYNTAX`, `V<slot>,?,UNKNOWN` |
| 2. Hardware-independent range (final) | `CHANNEL` 0..255, `FREQ` > 0, `RANGE` > 0 | `V<slot>,<key>,RANGE` |
| 3. Hardware capability | Voltage window, sweep direction, supported frequencies and ranges, channels that exist | `V<slot>,<key>,HW` |

- Level-1 errors take precedence over level-2 errors. Within a level, the
  first error in request order is reported.
- Level 3 is a hook only: the dummy firmware has no hardware limits and
  accepts everything. The `HW` code and "checked before `R`" are part of the
  contract; the limit values will come with real hardware.
- The app's own rules (voltages ≤ 0 and `START` ≤ `END`) are domain rules and
  are not enforced at level 2, because sign and direction depend on the
  technique and the hardware.
- `ERR=BUSY` and `ERR=RX_OVERFLOW` apply as for legacy commands.

### Timeouts

- **Before `R`:** use the `FRAG_OK` timeout for each fragment, then a short
  timeout for `R`, `V` or `ERR`.
- **After `R`:** use a single deadline of `R_time + t + margin`. Silence at
  the application level before the deadline is normal, so there is no
  per-frame inactivity timeout.
- **Lost link:** detected by the BLE link-layer supervision timeout and
  reported as a disconnect. That works even when the firmware sends nothing.

### Slot outcomes: what the app may conclude

| Outcome | Evidence | Safe conclusion |
|---|---|---|
| **RECEIVED** | `C…E` complete, `E` count equals `B` count, point indices have no gaps, echoed channel matches the request | Valid dataset. Keep it, whatever happens to later slots |
| **FAILED** | `X<slot>,ACQ_FAIL` | Acquisition was attempted and failed. The electrode may be affected |
| **NOT_RUN** | `X<slot>,SKIPPED` | The device states the electrode was not touched |
| **TRANSFER_INCOMPLETE** | `C` received, then disconnect, deadline, or a protocol violation before a valid `E` | Acquisition **completed** (that is what `C` means), but the data did not arrive. Discard the partial points; never analyse them. **Not** an acquisition failure |
| **UNKNOWN** | No `C` and no `X` for the slot when the procedure ended without `D` | The slot may not have started, may have been running, or may have finished without being sent. **Not** "not measured" and **not** "failed" |

Rules:
- Only `X` reports an acquisition failure. A disconnect, an expired deadline
  or a protocol violation is always a transmission-level outcome
  (TRANSFER_INCOMPLETE or UNKNOWN).
- **Protocol violation inside a block** (malformed frame, index gap, count
  mismatch, wrong echoed channel):
  - That block becomes TRANSFER_INCOMPLETE.
  - The app stops interpreting the procedure, and every slot without
    evidence becomes UNKNOWN.
  - Earlier RECEIVED blocks are kept.
- **No repeat guarantee.** Apart from NOT_RUN, the contract does not say a
  slot can be measured again on the same cartridge, so the app must not
  repeat slots automatically.
- **Firmware on disconnect (dummy):** the procedure is abandoned and the
  firmware returns to idle. Nothing is retained.
- `REQ` plus slot uniquely identify a measurement, so a later retrieval
  command can be added without changing any of the above.

### Deferred (additive later, no breaking change)

- `ABORT#` (cancel, which would produce `X<slot>,SKIPPED`)
- `GET=<id>,<slot>` retrieval and keeping results on the device across a
  disconnect
- procedure-level header keys (conditioning, incubation)
- level-3 limit values
- binary or MTU-negotiated block encoding

**Needs a contract version bump:**
- new per-slot keys (unknown keys are rejected)
- hardware that must stream a block while still acquiring it
- radio silence at the link level (which would need a disconnect plus
  retrieval)

### Dummy firmware simulation

- **Acquisition:** each slot waits `ACQ_SIM_MS` (2000 ms) and then streams
  the same hard-coded 35-point trace.
- **`t`:** computed as `N × (ACQ_SIM_MS + 39 frames × 60 ms) + 2 × 60 ms`,
  rounded up to seconds, plus 5 s of slack. For example, `t = 19` for N=3.
- **Failure injection:** build with
  `-- -DEXTRA_CFLAGS=-DSIM_FAIL_CHANNEL=<ch>` to make every slot on channel
  `<ch>` report `X<slot>,ACQ_FAIL`.

---

## Firmware Behavior

### Command reception

The `received()` callback:

- receives NUS RX writes
- treats each write as a fragment
- appends fragment bytes to `rx_buf`
- detects `#` as the command terminator
- logs the reconstructed command
- parses and validates the `CHANNEL` field (see "Channel Field" above);
  `START`/`END`/`FREQ`/`RANGE` are still reconstructed but not parsed
- sends `FRAG_OK` for non-final fragments
- starts trace transmission after the full command is received, unless
  `CHANNEL` was present and invalid (`ERR=CHANNEL_INVALID`, no transfer
  started)
- routes commands containing `REQ=` to the multi-channel path (see
  "Multi-Channel Measurement Request" above)

### Trace transmission

Trace transmission is handled by a delayed work item.

The trace transfer state machine is:

```text
TRACE_TX_IDLE
TRACE_TX_ACCEPT      (multi only)
TRACE_TX_ACQUIRE     (multi only)
TRACE_TX_CHAN_HDR    (multi only)
TRACE_TX_BEGIN
TRACE_TX_SCALE
TRACE_TX_DATA
TRACE_TX_END
TRACE_TX_SLOT_FAIL   (multi only)
TRACE_TX_DONE        (multi only)
```

For a multi request the phases run
`ACCEPT → (ACQUIRE → CHAN_HDR → BEGIN … END | ACQUIRE → SLOT_FAIL)` for each
slot, then `DONE`.

If a notification fails to send, the same frame is retried up to 3 times
with increasing back-off. After that the firmware disconnects, so the client
sees a disconnect rather than silence. This applies to legacy and multi
transfers alike.

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

### Channel Field — Not Yet Verified On-Target

The `CHANNEL` field is covered by the host-compiled unit tests in
`tests/cmd_parser/test_cmd_parser.c`, but end-to-end BLE behavior has not
been exercised on real hardware yet. To verify:

```text
write START=-800;END=0;FREQ=100;RANGE=10;CHANNEL=7#
```

- Confirm the trace response proceeds as usual (`B35`, ...).
- Confirm the RTT log shows `Starting trace on channel 7`.
- Write a command ending in `CHANNEL=256#` (or `CHANNEL=foo#`) and confirm
  the firmware replies `ERR=CHANNEL_INVALID` and sends no trace frames.

---

## Current Limitations

```text
- START/END/FREQ/RANGE command values are not parsed yet
- CHANNEL is parsed/validated but not physically applied (no mux/ADC
  channel-select hardware or driver exists in this firmware yet)
- trace data is hard-coded (35 real measurement points)
- only one trace transfer (legacy or multi) is supported at a time
- multi-channel requests: parameters are validated but not applied; every
  slot returns the same hard-coded trace after a simulated acquisition delay
- multi-channel requests are host-unit-tested but not yet verified on-target
- # is used as command terminator for manual testing
- no checksum yet
- no protocol version frame yet
- Flutter receiver not yet implemented
```

---

## Next Steps

Recommended next firmware steps:

1. Parse `START`/`END`/`FREQ`/`RANGE` from the reconstructed command
   (`CHANNEL` is done — see "Channel Field" above)
2. Add validation for malformed `START`/`END`/`FREQ`/`RANGE` values
3. Design and implement physical channel-select hardware (multiplexer or
   addressable analyzer) and wire `CHANNEL` to it — no such hardware exists
   in this firmware today
4. **Done:** send failures are now retried up to 3 times with back-off, then
   the firmware disconnects (see "Trace transmission"). Original note:
   Handle `send_text_notification()` / `bt_nus_send()` failure during trace
   transmission more gracefully. Currently (`trace_work_handler()`,
   `TRACE_TX_BEGIN`/`TRACE_TX_SCALE`/`TRACE_TX_DATA`/`TRACE_TX_END` cases),
   any send failure — observed as `-12`/`-ENOMEM` when the BLE stack's TX
   buffer pool is exhausted by notifications outpacing the connection
   interval — just calls `finish_trace_transfer()` and gives up silently.
   The client is never told; it only finds out via its own response
   timeout (30s in the Flutter app), well after the fact. Consider: retry
   with backoff instead of aborting on the first `-ENOMEM`, and/or send an
   `ERR=` frame before giving up so the client fails fast.
   Reproduced 2026-09-16 at point 20/35 during a real measurement — but
   only with OpenOCD/RTT attached at the same time (see the project's
   `rtt-disrupts-ble` memory note); did not reproduce with RTT detached in
   the same scenario. So this may mostly matter under debugger-induced
   timing pressure rather than normal operation, but the missing
   retry/error-reporting is a real gap either way (e.g. under adverse RF
   conditions in the field, with no debugger attached at all).
5. Add protocol versioning if the app integration becomes stable
6. Add checksum or transfer ID later if needed

Recommended mobile app steps — **done**, implemented in the Flutter app
(`hormone_test_connect`), confirmed working end-to-end 2026-09-16:

1. Subscribe to NUS TX before writing —
   `CvTraceRemoteDataSourceBleImpl.fetchTrace()` creates the notification
   `StreamIterator` and calls `txChar.setNotifyValue(true)` before writing
   the first fragment.
2. Write command fragments sequentially to NUS RX — same method, the
   per-fragment write loop.
3. Wait for `FRAG_OK` after non-final fragments — same method, checks each
   notification against `'FRAG_OK'` before writing the next fragment.
4. On `B<count>`: start transfer, store expected point count —
   `BleTraceFrameParser._handleB()`.
5. On `S<xs>,<ys>`: store scale factors — `BleTraceFrameParser._handleS()`.
6. On `<idx>,<V>,<I>`: extract and scale — `BleTraceFrameParser._handlePoint()`
   extracts the raw values; the `xs`/`ys` division happens during
   DTO→entity mapping, not in the parser itself.
7. On `E<count>`: validate count matches B, finish transfer —
   `BleTraceFrameParser._handleE()`.
8. Map decoded points into the app trace model —
   `CvTraceBleRepositoryImpl.getTraceFromDevice()` via `mapper.dtoToEntity()`.

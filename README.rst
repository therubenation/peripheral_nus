BLE Peripheral – NUS Voltage/Current Trace Protocol
####################################################

Overview
********

This firmware implements a BLE peripheral that receives measurement commands
over the Nordic UART Service (NUS) and responds with a fake cyclic voltammetry
(CV) trace as a sequence of voltage/current point notifications.

The device is targeted at the Seeed XIAO nRF54L15 (and nRF54LM20A) but runs
on any Zephyr-supported board with BLE.

How It Works
************

Command reception (NUS RX)
==========================

A full measurement command may be split across several BLE writes (fragments).
The firmware accumulates fragments in a 128-byte buffer until it sees the ``#``
terminator character.

Example fragmented command::

    START=-800;
    END=0;
    FREQ=100;
    RANGE=10;
    CHANNEL=7#

Field units (for when ``START``/``END``/``FREQ``/``RANGE`` parsing is
implemented — see "Current Limitations" below): ``START``/``END`` in mV,
``FREQ`` in Hz, ``RANGE`` in **µA** (changed from nA). Do not confuse
``RANGE``'s µA with the unrelated nA unit used for *measured* trace-output
current — that's a different quantity, unaffected by this change.

For every non-final fragment the firmware replies::

    FRAG_OK

When the ``#`` terminator is received the full command is logged, the
``CHANNEL`` field (if present) is validated, and a trace transfer is started.
If the buffer overflows before the terminator is seen the firmware discards
the buffer and sends::

    ERR=RX_OVERFLOW

If a new command arrives while a trace transfer is already in progress::

    ERR=BUSY

If ``CHANNEL`` is present but not a valid integer in ``0..255``, the command
is rejected and no trace transfer is started::

    ERR=CHANNEL_INVALID

``CHANNEL`` is optional; a command without it behaves exactly as before.

Trace response (NUS TX)
=======================

The trace is delivered as a sequence of NUS notifications separated by 30 ms.
Every frame is ≤ 18 bytes so it fits the default BLE payload without MTU
negotiation::

    B<count>
    S<x_scale>,<y_scale>
    <index>,<voltage_mv_scaled>,<current_na_scaled>
    ...
    E<count>

``V`` and ``I`` in data frames are scaled integers, not direct physical
values. Recover physical units from the ``S`` frame's scale factors::

    real_voltage_mV = voltage_mv_scaled / x_scale
    real_current_nA = current_na_scaled / y_scale

Current hardcoded trace data (35 real measurement points, excerpted)::

    B35
    S1000,1000000
    0,-340040,406027
    1,-330040,385046
    ...
    34,-196,403643
    E35

Field meanings:

* ``B``   – trace begin, point count appended directly (e.g. ``B35``)
* ``S``   – scale metadata: ``x_scale`` then ``y_scale``, comma-separated
* ``idx`` – trace point index (0-based)
* ``V``   – voltage, scaled integer
* ``I``   – current, scaled integer
* ``E``   – trace end, point count appended directly (e.g. ``E35``)

See ``docs/ble/BLE_CHUNKED_TRACE_PROTOCOL.md`` for the full 35-point example
and scale-reconstruction worked examples.

Trace state machine
===================

Trace transmission runs in a Zephyr delayable work item so it does not block
the NUS RX callback::

    TRACE_TX_IDLE → TRACE_TX_BEGIN → TRACE_TX_SCALE → TRACE_TX_DATA → TRACE_TX_END → TRACE_TX_IDLE

Requirements
************

* A board with Bluetooth LE support (tested on Seeed XIAO nRF54L15)
* nRF Connect SDK / Zephyr environment

Building and Running
********************

Build for the XIAO nRF54L15::

    west build -b xiao_nrf54l15

Flash::

    west flash

Console output uses Segger RTT (``CONFIG_RTT_CONSOLE=y``).  Connect with
J-Link RTT Viewer or ``west debug``.

Testing with nRF Connect
========================

1. Open nRF Connect on a phone or desktop.
2. Connect to the device advertising as ``CONFIG_BT_DEVICE_NAME``.
3. Enable notifications on the NUS TX characteristic.
4. Write command fragments to the NUS RX characteristic::

       START=-800;
       END=0;
       FREQ=100;
       RANGE=10;
       CHANNEL=7#

5. Observe ``FRAG_OK`` after each non-final write, then the full trace
   notification sequence.

Current Limitations
*******************

* Command parameters ``START``, ``END``, ``FREQ``, and ``RANGE`` are not
  parsed; the same hardcoded trace is returned for every command regardless
  of their values.
* ``CHANNEL`` is parsed and validated (integer, ``0..255``, optional) and is
  logged/stored for the duration of the run, but there is no hardware
  multiplexer/electrode-select path in this firmware yet, so the channel is
  not physically applied to a measurement.
* Only one trace transfer is active at a time.
* ``#`` is used as the command terminator for ease of manual testing.
* No protocol versioning, checksums, or retry mechanism.
* Text-based protocol is optimised for debuggability rather than bandwidth.

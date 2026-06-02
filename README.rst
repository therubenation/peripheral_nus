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
    RANGE=10#

For every non-final fragment the firmware replies::

    FRAG_OK

When the ``#`` terminator is received the full command is logged and a trace
transfer is started.  If the buffer overflows before the terminator is seen the
firmware discards the buffer and sends::

    ERR=RX_OVERFLOW

If a new command arrives while a trace transfer is already in progress::

    ERR=BUSY

Trace response (NUS TX)
=======================

The trace is delivered as a sequence of NUS notifications separated by 30 ms::

    TB;N=<point_count>
    P<index>;V=<voltage_mV>;I=<current_nA>
    ...
    TE;N=<point_count>

Current hardcoded trace data (10 points)::

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

Field meanings:

* ``TB`` – trace begin
* ``TE`` – trace end
* ``N``  – number of data points
* ``P``  – one data point (zero-indexed)
* ``V``  – voltage in millivolts
* ``I``  – current in nanoamps

Trace state machine
===================

Trace transmission runs in a Zephyr delayable work item so it does not block
the NUS RX callback::

    TRACE_TX_IDLE → TRACE_TX_BEGIN → TRACE_TX_DATA → TRACE_TX_END → TRACE_TX_IDLE

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
       RANGE=10#

5. Observe ``FRAG_OK`` after each non-final write, then the full trace
   notification sequence.

Current Limitations
*******************

* Command parameters (``START``, ``END``, ``FREQ``, ``RANGE``) are not parsed;
  the same hardcoded trace is returned for every command.
* Only one trace transfer is active at a time.
* ``#`` is used as the command terminator for ease of manual testing.
* No protocol versioning, checksums, or retry mechanism.
* Text-based protocol is optimised for debuggability rather than bandwidth.

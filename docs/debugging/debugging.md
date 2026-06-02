# RTT Debugging via OpenOCD + CMSIS-DAP on XIAO nRF54L15

## Purpose

This project uses RTT logging as the primary firmware debug channel.

The XIAO nRF54L15 does **not** provide a usable USB CDC ACM console path for this firmware project. The visible USB connection is not a native USB device interface of the nRF54L15 application core. UART-based console logging is also not suitable because the relevant UART paths are either not physically accessible or problematic on this board.

Therefore, debug output must be routed through:

```text
Firmware printk()
↓
SEGGER RTT buffer in RAM
↓
OpenOCD reads RAM via CMSIS-DAP/SWD
↓
OpenOCD exposes RTT channel on localhost:9090
↓
nc localhost 9090 displays logs
```

This is the established working debug model for the project.

## Confirmed Hardware Debug Architecture

The board uses Seeed's onboard CMSIS-DAP debug interface, not a SEGGER J-Link probe.

Confirmed by OpenOCD output:

```text
Using CMSIS-DAPv2 interface with VID:PID=0x2886:0x0066
CMSIS-DAP: SWD supported
[nrf54l.cpu] Cortex-M33 r1p0 processor detected
```

Important consequences:

- `JLinkRTTViewer` and `JLinkRTTClient` are **not** the correct tools for this board.
- They fail with messages like:

```text
No probes connected via USB
```

because no SEGGER J-Link probe is present.

RTT is still usable, but through **OpenOCD**, not through SEGGER J-Link tools.

## Required Firmware Configuration

The firmware must enable RTT console output.

Required `prj.conf` settings:

```ini
CONFIG_USE_SEGGER_RTT=y
CONFIG_RTT_CONSOLE=y
CONFIG_UART_CONSOLE=n
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
```

USB CDC ACM console settings must not be used for this board/project:

```ini
CONFIG_USB_DEVICE_STACK=n
CONFIG_USB_CDC_ACM=n
CONFIG_USB_UART_CONSOLE=n
```

Do not add `zephyr_udc0` or `cdc_acm_uart0` devicetree overlay nodes. The nRF54L15 application core does not expose a usable USB device controller for this setup.

## Starting RTT Logging

Use two terminals.

### Terminal 1: Start OpenOCD RTT Server

Run from the project environment:

```bash
openocd \
  -s /opt/nordic/ncs/v3.2.4/zephyr/boards/seeed/xiao_nrf54l15/support \
  -f openocd.cfg \
  -c "init" \
  -c "rtt setup 0x20000000 0x30000 \"SEGGER RTT\"" \
  -c "rtt start" \
  -c "rtt server start 9090 0"
```

Expected successful output includes:

```text
Using CMSIS-DAPv2 interface with VID:PID=0x2886:0x0066
[nrf54l.cpu] Cortex-M33 r1p0 processor detected
rtt: Searching for control block 'SEGGER RTT'
rtt: Control block found at 0x20000450
Listening on port 9090 for rtt connections
```

The exact RTT control block address may differ. The important part is:

```text
rtt: Control block found
```

### Terminal 2: Connect to RTT Output

```bash
nc localhost 9090
```

Expected boot output:

```text
*** Booting nRF Connect SDK ...
*** Using Zephyr OS ...
BLE enabled
Advertising started
```

If this output appears, RTT logging is working.

## Flashing Rule

`west flash` and the OpenOCD RTT server both need access to the same CMSIS-DAP debug probe.

Before flashing, stop both RTT terminals:

1. Stop Terminal 2 (`nc localhost 9090`) with:

```text
Ctrl+C
```

2. Stop Terminal 1 (`openocd ...`) with:

```text
Ctrl+C
```

3. Verify that no OpenOCD process is still running:

```bash
ps aux | grep openocd
```

4. If needed, kill remaining OpenOCD processes:

```bash
pkill openocd
```

5. Flash:

```bash
west flash
```

6. Restart RTT logging afterward using the two-terminal workflow above.

Important distinction:

```bash
west build
```

only builds locally and does not require stopping OpenOCD.

```bash
west flash
```

programs the board and requires exclusive debug-probe access.

## Interpreting BLE Logs

Example output:

```text
cmd: 'START=-800;END=0;FREQ=100;RANGE=10'
notify ret=-128 val='TRACE:-12,-10,-9,-4,3,12'
```

Interpretation:

- `cmd: ...` means the BLE write characteristic successfully received the command.
- `notify ret=0` means the notification succeeded.
- `notify ret=-128` means the notification failed.

A failed notification usually means the BLE client was not subscribed to the notify characteristic at the time the firmware attempted to send the notification.

A full successful BLE write + notify cycle should look like:

```text
CCC changed: 1
cmd: 'START=-800;END=0;FREQ=100;RANGE=10'
notify ret=0 val='TRACE:-12,-10,-9,-4,3,12'
```

If `CCC changed: 0` appears before the command, the client unsubscribed and notification failure is expected.

## Required Debug Discipline

Do not guess based on nRF Connect UI alone.

Always interpret BLE behavior using RTT logs.

Layer-by-layer validation order:

1. Firmware boots.
2. BLE stack is enabled.
3. Advertising starts.
4. Client connects.
5. Notify characteristic is subscribed (`CCC changed: 1`).
6. Command characteristic receives write.
7. Firmware attempts notification.
8. Notification return value is checked.

Do not call a BLE test fully successful unless both are true:

```text
cmd: '<expected command>'
notify ret=0
```

Receiving the command alone is only partial success.

## Known Working Debug Pipeline

```text
XIAO nRF54L15
↓
CMSIS-DAP / SWD
↓
OpenOCD
↓
RTT memory buffer
↓
TCP localhost:9090
↓
nc localhost 9090
```

This is the canonical debug path for this project.
#!/usr/bin/env python3
"""F0 hardware check for the B2 multi-channel protocol (UC8 plan 3).

Talks to the board over the Nordic UART Service exactly like the app does:
one TX notification subscription, fragments of <= 20 bytes, FRAG_OK after
every non-final fragment, '#' on the last one. Prints every frame with its
time since the final write, validates the response against
docs/ble/BLE_CHUNKED_TRACE_PROTOCOL.md and reports the final-write -> R gap.

Setup (once):
    python3 -m venv .venv && . .venv/bin/activate && pip install bleak

Examples:
    python3 tools/f0_b2_check.py --list                  # find the board
    python3 tools/f0_b2_check.py multi3                  # 3 slots, field per write
    python3 tools/f0_b2_check.py multi3 --packed         # 3 slots, 20-byte writes
    python3 tools/f0_b2_check.py multi3 --repeat 5       # R-gap statistics
    python3 tools/f0_b2_check.py legacy                  # B35 ... E35, no C frame
    python3 tools/f0_b2_check.py reject-range            # expects V1,FREQ,RANGE
    python3 tools/f0_b2_check.py reject-slot-count       # expects ERR=SLOT_COUNT
    python3 tools/f0_b2_check.py --command "REQ=9;N=1;SLOT=0;CHANNEL=0;START=-800;END=0;FREQ=100;RANGE=10"

SIM_FAIL_CHANNEL: build the firmware with -DSIM_FAIL_CHANNEL=4, then run
`multi3 --expect-fail 1` (slot 1 uses channel 4).

Disconnect in a block: `multi3 --wait 60 --disconnect-in-block 0` makes the
script drop the link itself in the middle of slot 0's block and then checks
that the board advertises again. For a board-side loss instead, run
`multi3 --wait 60` and reset or unplug the board right after "C0,1" is
printed (frames are printed live; the block takes about 2 s until E35). The script reports the disconnect and which slots were
complete, incomplete or without evidence. Afterwards run `--list` to confirm
the board advertises again.

Only uses this Mac's Bluetooth; it writes nothing to disk.
"""

from __future__ import annotations

import argparse
import asyncio
import re
import statistics
import sys
import time
from dataclasses import dataclass, field

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # pragma: no cover
    sys.exit("bleak is missing: pip install bleak")

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # app writes
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # firmware notifies

MAX_WRITE = 20
FRAG_OK_TIMEOUT = 5.0
ACCEPTANCE_TIMEOUT = 3.0  # the app's current default
DEADLINE_MARGIN = 10.0  # the app's current default

SLOTS3 = [
    dict(CHANNEL=1, START=-800, END=0, FREQ=100, RANGE=10),
    dict(CHANNEL=4, START=-600, END=-100, FREQ=50, RANGE=5),
    dict(CHANNEL=7, START=-400, END=0, FREQ=25, RANGE=1),
]


def multi_request(req_id: int, slots: list[dict], n: int | None = None) -> str:
    fields = [f"REQ={req_id}", f"N={len(slots) if n is None else n}"]
    for i, s in enumerate(slots):
        fields.append(f"SLOT={i}")
        fields += [f"{k}={s[k]}" for k in ("CHANNEL", "START", "END", "FREQ", "RANGE")]
    return ";".join(fields)


def scenario_command(name: str, req_id: int) -> tuple[str, str]:
    """Returns (logical command without '#', kind)."""
    if name == "multi3":
        return multi_request(req_id, SLOTS3), "multi"
    if name == "multi6":
        slots = [dict(SLOTS3[0], CHANNEL=c) for c in (0, 1, 2, 3, 4, 5)]
        return multi_request(req_id, slots), "multi"
    if name == "reject-range":
        slots = [SLOTS3[0], dict(SLOTS3[1], FREQ=0), SLOTS3[2]]
        return multi_request(req_id, slots), "multi"
    if name == "reject-slot-count":
        return multi_request(req_id, SLOTS3, n=4), "multi"
    if name == "legacy":
        return "START=-800;END=0;FREQ=100;RANGE=10;CHANNEL=7", "legacy"
    raise ValueError(name)


def fragments(command: str, packed: bool) -> list[str]:
    """Field per write (the app's strategy) or packed 20-byte writes."""
    if packed:
        full = command + "#"
        return [full[i : i + MAX_WRITE] for i in range(0, len(full), MAX_WRITE)]
    parts = command.split(";")
    return [p + ";" for p in parts[:-1]] + [parts[-1] + "#"]


@dataclass
class Frame:
    t: float  # seconds since the final write (negative = before)
    text: str


@dataclass
class Run:
    frames: list[Frame] = field(default_factory=list)
    final_write_at: float | None = None
    write_error: str | None = None
    disconnected_at: float | None = None
    ended_by: str = ""


class Device:
    def __init__(self, client: BleakClient, verbose: bool):
        self.client = client
        self.verbose = verbose
        self.queue: asyncio.Queue[tuple[float, str]] = asyncio.Queue()
        self.disconnected = asyncio.Event()
        self.disconnected_at: float | None = None

    def on_notify(self, _sender, data: bytearray) -> None:
        now = time.monotonic()
        for line in data.decode(errors="replace").splitlines():
            line = line.strip()
            if line:
                self.queue.put_nowait((now, line))

    def on_disconnect(self, _client) -> None:
        self.disconnected_at = time.monotonic()
        self.disconnected.set()

    async def next_frame(self, timeout: float) -> tuple[float, str] | None:
        """Next frame, or None on timeout / disconnect."""
        get = asyncio.ensure_future(self.queue.get())
        disc = asyncio.ensure_future(self.disconnected.wait())
        done, _ = await asyncio.wait({get, disc}, timeout=max(timeout, 0), return_when=asyncio.FIRST_COMPLETED)
        disc.cancel()
        if get in done:
            return get.result()
        get.cancel()
        return None

    async def send(self, frags: list[str], run: Run) -> None:
        for i, frag in enumerate(frags):
            data = frag.encode()
            assert len(data) <= MAX_WRITE, f"fragment {i} is {len(data)} bytes: {frag!r}"
            final = i == len(frags) - 1
            if self.verbose:
                print(f"  -> {frag!r} ({len(data)} B)")
            if final:
                run.final_write_at = time.monotonic()
                try:
                    await self.client.write_gatt_char(NUS_RX, data, response=True)
                except Exception as e:  # the device may still have the request
                    run.write_error = repr(e)
                return
            await self.client.write_gatt_char(NUS_RX, data, response=True)
            got = await self.next_frame(FRAG_OK_TIMEOUT)
            if got is None:
                raise RuntimeError(f"no FRAG_OK for fragment {i} ({frag!r})")
            if got[1] != "FRAG_OK":
                raise RuntimeError(f"expected FRAG_OK for fragment {i}, got {got[1]!r}")


class LivePrinter:
    """Prints frames as they arrive; at most [max_points] point frames per block."""

    def __init__(self, max_points: int):
        self.max_points = max_points
        self.points = 0

    def __call__(self, frame: Frame) -> None:
        if re.fullmatch(r"\d+,-?\d+,-?\d+", frame.text):
            self.points += 1
            if self.points > self.max_points:
                if self.points == self.max_points + 1:
                    print("               ...", flush=True)
                return
        elif frame.text.startswith("B"):
            self.points = 0
        print(f"  {frame.t * 1000:9.1f} ms  {frame.text}", flush=True)


async def run_once(dev: Device, command: str, kind: str, packed: bool, wait: float | None,
                   show: LivePrinter, disconnect_in_block: int | None = None) -> Run:
    run = Run()
    block_slot: int | None = None
    block_points = 0
    frags = fragments(command, packed)
    print(f"Sending {len(frags)} write(s), {len(command) + 1} bytes: {command}#")
    await dev.send(frags, run)
    t0 = run.final_write_at

    accepted = kind == "legacy"
    deadline = time.monotonic() + (wait if wait is not None else (30.0 if kind == "legacy" else ACCEPTANCE_TIMEOUT))
    while True:
        got = await dev.next_frame(deadline - time.monotonic())
        if got is None:
            if dev.disconnected.is_set():
                run.disconnected_at = dev.disconnected_at - t0
                run.ended_by = "disconnect"
            else:
                run.ended_by = "acceptance timeout (no R/V/ERR)" if not accepted else "deadline"
            break
        at, text = got
        run.frames.append(Frame(at - t0, text))
        show(run.frames[-1])
        if c := re.fullmatch(r"C(\d+),\d+", text):
            block_slot, block_points = int(c[1]), 0
        elif re.fullmatch(r"\d+,-?\d+,-?\d+", text):
            block_points += 1
            if block_slot == disconnect_in_block and block_points == 3:
                print("               <script disconnects now, inside the block>", flush=True)
                await dev.client.disconnect()
        if not accepted and (m := re.fullmatch(r"R(\d+),(\d+),(\d+)", text)):
            accepted = True
            if wait is None:
                deadline = at + int(m[3]) + DEADLINE_MARGIN
        if kind == "multi" and (text.startswith("D") or (not accepted and (text.startswith("V") or text.startswith("ERR=")))):
            run.ended_by = text
            break
        if kind == "legacy" and (re.fullmatch(r"E\d+", text) or text.startswith("ERR=")):
            run.ended_by = text
            break
    return run


# ---------------------------------------------------------------- validation


def check_block(lines: list[str]) -> str | None:
    """None if the B..E block is complete and valid, else the problem."""
    if not lines or not re.fullmatch(r"B\d+", lines[0]):
        return "block does not start with B<count>"
    count = int(lines[0][1:])
    if len(lines) < 2 or not re.fullmatch(r"S[0-9.]+,[0-9.]+", lines[1]):
        return "missing or malformed S frame"
    points = lines[2:-1]
    if not re.fullmatch(r"E\d+", lines[-1]):
        return "block does not end with E<count>"
    if int(lines[-1][1:]) != count:
        return f"E count {lines[-1][1:]} != B count {count}"
    if len(points) != count:
        return f"{len(points)} points, expected {count}"
    for i, p in enumerate(points):
        parts = p.split(",")
        if len(parts) != 3 or parts[0] != str(i):
            return f"bad point frame {p!r} at index {i}"
    return None


def validate_multi(run: Run, req_id: int, channels: list[int], expect_fail: set[int]) -> list[str]:
    problems: list[str] = []
    texts = [f.text for f in run.frames]
    if not texts:
        return ["no response at all"]
    if texts[0].startswith(("V", "ERR=")):
        return []  # a rejection; checked separately
    m = re.fullmatch(r"R(\d+),(\d+),(\d+)", texts[0])
    if not m:
        return [f"first frame is {texts[0]!r}, expected R{req_id},{len(channels)},<t>"]
    if int(m[1]) != req_id or int(m[2]) != len(channels):
        problems.append(f"R echoes id/n {m[1]}/{m[2]}, expected {req_id}/{len(channels)}")

    outcome: dict[int, str] = {}
    i = 1
    while i < len(texts):
        t = texts[i]
        if c := re.fullmatch(r"C(\d+),(\d+)", t):
            slot, ch = int(c[1]), int(c[2])
            j = i + 1
            while j < len(texts) and not re.fullmatch(r"E\d+", texts[j]):
                j += 1
            block = texts[i + 1 : j + 1]
            if slot in outcome:
                problems.append(f"slot {slot} reported twice")
            elif slot >= len(channels):
                problems.append(f"C for unknown slot {slot}")
            elif ch != channels[slot]:
                problems.append(f"slot {slot}: C echoes channel {ch}, expected {channels[slot]}")
                outcome[slot] = "wrong channel"
            elif j >= len(texts):
                outcome[slot] = "TRANSFER_INCOMPLETE"
            else:
                err = check_block(block)
                outcome[slot] = "RECEIVED" if err is None else f"invalid block: {err}"
                if err:
                    problems.append(f"slot {slot}: {err}")
            i = j + 1
        elif x := re.fullmatch(r"X(\d+),([A-Z_]+)", t):
            slot = int(x[1])
            if slot in outcome:
                problems.append(f"slot {slot} reported twice")
            outcome[slot] = {"ACQ_FAIL": "FAILED", "SKIPPED": "NOT_RUN"}.get(x[2], f"unknown X code {x[2]}")
            i += 1
        elif d := re.fullmatch(r"D(\d+),(\d+),(\d+)", t):
            ok, xs = int(d[2]), int(d[3])
            received = sum(1 for o in outcome.values() if o == "RECEIVED")
            failed = sum(1 for o in outcome.values() if o in ("FAILED", "NOT_RUN"))
            if int(d[1]) != req_id:
                problems.append(f"D echoes id {d[1]}, expected {req_id}")
            if ok + xs != len(channels) or ok != received or xs != failed:
                problems.append(f"D{d[1]},{ok},{xs} does not match {received} received / {failed} X")
            i += 1
            if i < len(texts):
                problems.append(f"frames after D: {texts[i:]}")
            break
        else:
            problems.append(f"unexpected frame between blocks: {t!r}")
            i += 1

    print("\nSlot outcomes:")
    for s, ch in enumerate(channels):
        print(f"  slot {s} (channel {ch}): {outcome.get(s, 'UNKNOWN (no C/X)')}")
    for s in expect_fail:
        if outcome.get(s) != "FAILED":
            problems.append(f"slot {s} expected X{s},ACQ_FAIL, got {outcome.get(s, 'nothing')}")
    if not run.ended_by.startswith("D") and run.disconnected_at is None:
        problems.append(f"procedure ended by {run.ended_by}, not D")
    return problems


def print_run(run: Run) -> None:
    """Frames were printed live; this prints how the run ended."""
    if run.write_error:
        print(f"  final write raised: {run.write_error}")
    if run.disconnected_at is not None:
        print(f"  {run.disconnected_at * 1000:9.1f} ms  <disconnected>")
    print(f"Ended by: {run.ended_by}")


# ---------------------------------------------------------------------- main


async def find_device(args) -> str:
    if args.address:
        return args.address
    print("Scanning 5 s for NUS devices ...")
    found = await BleakScanner.discover(timeout=5.0, service_uuids=[NUS_SERVICE])
    if args.name:
        found = [d for d in found if d.name == args.name]
    if not found:
        sys.exit("No NUS device found. Is the board advertising (and not connected to a phone)?")
    for d in found:
        print(f"  {d.address}  {d.name}")
    if len(found) > 1 and not args.list:
        sys.exit("Several devices found; pass --address or --name.")
    return found[0].address


async def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("scenario", nargs="?", default="multi3",
                   choices=["multi3", "multi6", "legacy", "reject-range", "reject-slot-count"])
    p.add_argument("--command", help="custom logical command (without '#'); overrides the scenario")
    p.add_argument("--list", action="store_true", help="only scan and list NUS devices")
    p.add_argument("--address", help="device address / macOS UUID")
    p.add_argument("--name", help="advertised device name")
    p.add_argument("--req", type=int, default=42, help="REQ id for generated multi requests")
    p.add_argument("--packed", action="store_true", help="20-byte writes instead of one field per write")
    p.add_argument("--repeat", type=int, default=1, help="run N times on one connection (R-gap statistics)")
    p.add_argument("--expect-fail", type=int, action="append", default=[], help="slot expected to give X<slot>,ACQ_FAIL")
    p.add_argument("--wait", type=float, help="listen this long after the final write (e.g. for the disconnect test)")
    p.add_argument("--disconnect-in-block", type=int, metavar="SLOT",
                   help="disconnect after the 3rd point of this slot's block, then check advertising")
    p.add_argument("--points", type=int, default=3, help="point frames shown per block")
    p.add_argument("-v", "--verbose", action="store_true", help="print every write")
    args = p.parse_args()

    address = await find_device(args)
    if args.list:
        return 0

    gaps: list[float] = []
    all_ok = True
    dev: Device | None = None

    def on_disconnect(c):
        if dev:
            dev.on_disconnect(c)

    async with BleakClient(address, disconnected_callback=on_disconnect) as client:
        dev = Device(client, args.verbose)
        print(f"Connected to {address}, MTU {client.mtu_size}")
        await client.start_notify(NUS_TX, dev.on_notify)
        try:
            for n in range(args.repeat):
                req_id = (args.req + n - 1) % 65535 + 1
                if args.command:
                    command = args.command
                    kind = "multi" if "REQ=" in command else "legacy"
                else:
                    command, kind = scenario_command(args.scenario, req_id)
                print(f"\n=== Run {n + 1}/{args.repeat}: {args.command and 'custom' or args.scenario} ===")
                run = await run_once(dev, command, kind, args.packed, args.wait,
                                     LivePrinter(args.points), args.disconnect_in_block)
                print_run(run)

                if run.frames and kind == "multi" and re.fullmatch(r"R\d+,\d+,\d+", run.frames[0].text):
                    gap = run.frames[0].t
                    gaps.append(gap)
                    t = int(run.frames[0].text.split(",")[2])
                    last = run.frames[-1].t
                    print(f"Final write -> R: {gap * 1000:.1f} ms (app waits {ACCEPTANCE_TIMEOUT:.0f} s)")
                    print(f"R promised t = {t} s; last frame at {last:.2f} s (app deadline t + {DEADLINE_MARGIN:.0f} s)")

                if kind == "multi":
                    channels = [int(c) for c in re.findall(r"CHANNEL=(\d+)", command)]
                    problems = validate_multi(run, int(re.search(r"REQ=(\d+)", command)[1]), channels, set(args.expect_fail))
                    first = run.frames[0].text if run.frames else ""
                    if first.startswith(("V", "ERR=")):
                        print(f"Rejected before R: {first} (frames after it: {len(run.frames) - 1})")
                        if len(run.frames) > 1:
                            problems.append("frames after a rejection")
                else:
                    texts = [f.text for f in run.frames]
                    block_err = check_block(texts) if texts and texts[0].startswith("B") else "no B frame"
                    problems = [] if block_err is None else [block_err]
                    if any(t.startswith("C") for t in texts):
                        problems.append("legacy response contains a C frame")

                if problems:
                    all_ok = False
                    print("PROBLEMS:")
                    for pr in problems:
                        print(f"  - {pr}")
                else:
                    print("OK")
                if dev.disconnected.is_set():
                    break
        finally:
            if client.is_connected:
                await client.stop_notify(NUS_TX)

    if args.disconnect_in_block is not None:
        await asyncio.sleep(2.0)
        print("\nScanning 8 s: does the board advertise again?")
        again = await BleakScanner.discover(timeout=8.0, service_uuids=[NUS_SERVICE])
        if any(d.address == address for d in again):
            print("Advertising again: yes")
        else:
            print("Advertising again: NO")
            all_ok = False

    if len(gaps) > 1:
        print(f"\nR gap over {len(gaps)} runs: min {min(gaps) * 1000:.1f} ms, "
              f"median {statistics.median(gaps) * 1000:.1f} ms, max {max(gaps) * 1000:.1f} ms")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))

#!/usr/bin/env python
"""Non-interactive UART capture for automated firmware verification.

Resets the ESP32 over DTR/RTS (exactly like esptool does on the CP2102 based
Waveshare driver board), then streams the serial output until a marker line
appears, echoing every line with an elapsed-time prefix.

Run it with the ESP-IDF virtualenv python (it needs pyserial):

    source tools/env.sh
    python tools/capture.py --timeout 30 --expect "flash=4MB"

Exit codes:
    0  the --until marker was seen and all --expect substrings appeared
    1  the --error marker was seen
    2  timeout before the --until marker
    3  pyserial is missing (IDF environment not activated)
    4  stopped normally but some --expect substrings never appeared
"""

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write(
        "capture.py: pyserial is not available.\n"
        "Hint: activate the ESP-IDF environment first:\n"
        "    source tools/env.sh\n"
        "and run this script with the IDF venv python.\n"
    )
    sys.exit(3)

FALLBACK_PORT = "/dev/cu.usbserial-110"


def detect_port():
    """First CP2102N style device node, or FALLBACK_PORT if none is present.

    The Waveshare driver board enumerates as /dev/cu.usbserial-<n>, where <n>
    depends on the USB port it happens to be plugged into (it has been -110 and
    -10 on this Mac), so hard-coding one node breaks every time the cable moves.
    Sorted for a deterministic choice when several boards are attached; --port
    overrides it, and the port actually used is printed in the [capture] line.
    """
    ports = sorted(glob.glob("/dev/cu.usbserial*"))
    return ports[0] if ports else FALLBACK_PORT


DEFAULT_PORT = detect_port()
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 90.0
DEFAULT_UNTIL = "[EPD-TEST] DONE"
DEFAULT_ERROR = "[EPD-TEST] ERROR"

EXIT_OK = 0
EXIT_ERROR_MARKER = 1
EXIT_TIMEOUT = 2
EXIT_NO_EXPECT = 4


def parse_args():
    p = argparse.ArgumentParser(
        description="Capture ESP32 UART output until a marker line appears."
    )
    p.add_argument("--port", default=DEFAULT_PORT, help="serial port (default: %(default)s)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="baud rate (default: %(default)s)")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                   help="overall timeout in seconds (default: %(default)s)")
    p.add_argument("--until", dest="until", default=DEFAULT_UNTIL,
                   help="stop successfully on a line containing this (default: %(default)r)")
    p.add_argument("--error", dest="error", default=DEFAULT_ERROR,
                   help="stop with failure on a line containing this (default: %(default)r)")
    p.add_argument("--expect", dest="expect", action="append", default=[], metavar="SUBSTR",
                   help="substring that must appear before stopping (repeatable)")
    p.add_argument("--no-reset", dest="no_reset", action="store_true",
                   help="do not pulse DTR/RTS to reset the board")
    return p.parse_args()


def emit(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def reset_board(ser):
    """ESP32 hardware reset into normal boot: RTS drives EN, DTR drives GPIO0."""
    ser.dtr = False   # GPIO0 high -> normal boot (not download mode)
    ser.rts = True    # EN low  -> hold in reset
    time.sleep(0.1)
    ser.rts = False   # EN high -> run
    # DTR intentionally left False.


def main():
    args = parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        emit("[capture] result=FAIL reason=OPEN_FAILED port=%s detail=%s" % (args.port, exc))
        return EXIT_TIMEOUT

    outcome = EXIT_TIMEOUT
    reason = "TIMEOUT"
    seen = set()
    buf = bytearray()
    start = time.monotonic()

    try:
        if args.no_reset:
            emit("[capture] port=%s baud=%d timeout=%.1fs reset=skipped"
                 % (args.port, args.baud, args.timeout))
        else:
            reset_board(ser)
            emit("[capture] port=%s baud=%d timeout=%.1fs reset=dtr/rts"
                 % (args.port, args.baud, args.timeout))
        # Drop anything produced before the clock starts.
        ser.reset_input_buffer()
        start = time.monotonic()

        stop = False
        while not stop:
            elapsed = time.monotonic() - start
            if elapsed > args.timeout:
                reason = "TIMEOUT"
                outcome = EXIT_TIMEOUT
                break

            chunk = ser.read(max(1, ser.in_waiting))
            if chunk:
                buf.extend(chunk)

            while b"\n" in buf:
                raw, _, rest = buf.partition(b"\n")
                buf = bytearray(rest)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                emit("[+%.3f] %s" % (time.monotonic() - start, line))

                for needle in args.expect:
                    if needle in line:
                        seen.add(needle)

                if args.error and args.error in line:
                    reason = "ERROR_MARKER"
                    outcome = EXIT_ERROR_MARKER
                    stop = True
                    break
                if args.until and args.until in line:
                    reason = "UNTIL_MARKER"
                    outcome = EXIT_OK
                    stop = True
                    break

        # Flush a trailing partial line so nothing is silently lost.
        if buf:
            tail = bytes(buf).decode("utf-8", "replace").rstrip("\r")
            if tail:
                emit("[+%.3f] %s" % (time.monotonic() - start, tail))
                for needle in args.expect:
                    if needle in tail:
                        seen.add(needle)
    except KeyboardInterrupt:
        reason = "INTERRUPTED"
        outcome = EXIT_TIMEOUT
    finally:
        ser.close()

    missing = [n for n in args.expect if n not in seen]

    # An explicit ERROR marker is the most specific failure, so it keeps exit 1;
    # otherwise missing --expect substrings take precedence over the stop reason.
    if missing and outcome != EXIT_ERROR_MARKER:
        outcome = EXIT_NO_EXPECT
        reason = "MISSING_EXPECT"

    if missing:
        emit("[capture] missing expects: %s" % ", ".join(repr(m) for m in missing))

    emit("[capture] result=%s reason=%s exit=%d expects=%d/%d elapsed=%.3fs"
         % ("OK" if outcome == EXIT_OK else "FAIL",
            reason, outcome, len(seen), len(args.expect),
            time.monotonic() - start))
    return outcome


if __name__ == "__main__":
    sys.exit(main())

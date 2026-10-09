#!/usr/bin/env python3
"""Grab what the pod is showing right now and save it as a PNG.

Usage:  tools/.venv/bin/python tools/pod_screenshot.py [output.png] [--port /dev/cu.usbmodem...]
        [--msg "L|160|Outer Ring Rd" --msg "TRIP|18:42|6.0" ...] [--test-off]

--msg shows a line on the pod as if the phone had sent it (the phone is then ignored until
--test-off), so screens can be checked without riding.

The pod firmware (firmware/pod/Screenshot.cpp) answers a "shot" command on the USB serial
port with run-length-encoded RGB565 pixels in base64 lines. Close any serial monitor first:
only one program can use the port at a time.
"""
import argparse
import base64
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
from PIL import Image, ImageDraw

DEFAULT_PORT = "/dev/cu.usbmodem5B910646211"
BAUD = 921600


class Garbled(Exception):
    """A line arrived damaged (e.g. a log message cut into it); the capture is retried."""


def capture_with_retry(port: str, attempts: int = 3) -> Image.Image:
    for attempt in range(attempts):
        try:
            return capture(port)
        except Garbled:
            print(f"Damaged data, retrying ({attempt + 1}/{attempts})", file=sys.stderr)
    sys.exit("Screenshot kept arriving damaged.")


def send_commands(port: str, lines: list[str]) -> None:
    with serial.Serial(port, BAUD, timeout=0.5) as ser:
        time.sleep(0.3)   # the USB-serial chip drops bytes written straight after opening
        for line in lines:
            ser.write((line + "\n").encode())
            ser.flush()
            time.sleep(0.02)
        time.sleep(0.5)   # let the pod draw the new screen before it is captured


def capture(port: str) -> Image.Image:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.5
    # Don't touch DTR/RTS: the board's auto-reset circuit reboots it when only one of them changes.
    # Opening the port asserts both together, which leaves the board running.
    ser.open()
    try:
        time.sleep(0.3)   # the USB-serial chip drops bytes written straight after opening
        ser.reset_input_buffer()
        ser.write(b"shot\n")
        width = height = None
        chunks = []
        deadline = time.time() + 15
        while time.time() < deadline:
            line = ser.readline().decode("ascii", errors="ignore").strip()
            if line.startswith("SHOT_BEGIN"):
                _, w, h = line.split()
                width, height, chunks = int(w), int(h), []
            elif line.startswith("SHOT:") and width:
                try:
                    chunks.append(base64.b64decode(line[5:], validate=True))
                except ValueError:
                    raise Garbled()
            elif line.startswith("SHOT_END") and width:
                break
            elif line.startswith("SHOT_ERROR"):
                sys.exit(line)
        else:
            sys.exit("No screenshot received. Is the pod running the latest firmware, and the serial monitor closed?")
    finally:
        ser.close()

    pixels = bytearray()
    for raw in chunks:
        for count, color in struct.iter_unpack("<HH", raw):
            r = (color >> 11) & 0x1F
            g = (color >> 5) & 0x3F
            b = color & 0x1F
            pixels += bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31)) * count
    expected = width * height * 3
    if len(pixels) != expected:
        raise Garbled()
    return Image.frombytes("RGB", (width, height), bytes(pixels))


def round_mask(image: Image.Image) -> Image.Image:
    """The panel is round: make the corners transparent so the PNG looks like the real screen."""
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, image.width - 1, image.height - 1), fill=255)
    out = image.convert("RGBA")
    out.putalpha(mask)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", nargs="?", help="PNG path (default: tools/screenshots/pod-<time>.png)")
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--msg", action="append", default=[], help="line to show as if sent by the phone")
    parser.add_argument("--msg-file", help="file with one phone line per line (e.g. a whole map area)")
    parser.add_argument("--test-off", action="store_true", help="go back to listening to the phone")
    args = parser.parse_args()

    lines = list(args.msg)
    if args.msg_file:
        lines += [l.strip() for l in Path(args.msg_file).read_text().splitlines() if l.strip() and not l.startswith("#")]
    commands = [f"msg {m}" for m in lines] + (["test off"] if args.test_off else [])
    if commands:
        send_commands(args.port, commands)

    output = Path(args.output) if args.output else (
        Path(__file__).parent / "screenshots" / f"pod-{datetime.now():%Y%m%d-%H%M%S}.png")
    output.parent.mkdir(parents=True, exist_ok=True)
    round_mask(capture_with_retry(args.port)).save(output)
    print(output)


if __name__ == "__main__":
    main()

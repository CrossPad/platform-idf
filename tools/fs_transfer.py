#!/usr/bin/env python3
"""File and assets transfer over USB CDC for CrossPad ESP32-S3.

Two things the app OTA path cannot do:

  * `assets` — reflash the whole LittleFS `assets` partition (app icons, fonts
    and main/stm32_fw/stm32_fw.bin, the bundled STM32 co-processor image).
    tools/ota_flash.py writes the app partition only, so a device kept up to
    date over CDC keeps whatever assets image its last UART flash left behind,
    and STM_FW_INFO goes on reporting that stale bundled version forever.

  * `push` / `pull` / `ls` / `rm` / `mkdir` / `roots` — arbitrary files, both
    directions, across the three mounts the device exposes:
      /assets    LittleFS, 1 MB, the partition above
      /spiflash  FAT "storage", 4 MB internal, mounted on first use
      /sdcard    the SD card

MSC is not available on this board (MIDI + CDC + UAC2 already use every
endpoint the descriptor has), so everything rides the CDC control port, the
same pipe OTA uses. Binary payloads are length-prefixed and unescaped: the
device switches out of newline framing for exactly the byte count announced in
the header line.

Exit codes: 0 success, 1 the device refused or failed, 2 the bench is wrong
(no board, port busy, missing file) — same convention as crosspad-hil.
"""

import argparse
import os
import sys
import time
import zlib

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ota_flash import ensure_port, board_build_artifact  # noqa: E402  (VID:PID detect + MIDI mode switch)

CHUNK_SIZE = 4096
ACK_WINDOW = 2   # chunks in flight; see stream()

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_BENCH = 2


class DeviceError(Exception):
    """The device answered with an error, or stopped answering."""


def open_port(port):
    try:
        ser = serial.Serial(port, 115200, timeout=30)
    except serial.SerialException as e:
        print(f"Error: cannot open {port}: {e}")
        sys.exit(EXIT_BENCH)
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def readline(ser):
    return ser.readline().decode(errors='ignore').strip()


def command(ser, line, expect=None, skip_noise=True):
    """Send one text command; return the first reply line.

    `expect` is a prefix the reply must have. Unrelated lines (log echo, a
    stale progress line) are skipped unless skip_noise is False.
    """
    ser.write((line + '\n').encode())
    deadline = time.time() + ser.timeout
    while time.time() < deadline:
        resp = readline(ser)
        if not resp:
            break
        if expect and not resp.startswith(expect):
            if resp.startswith(('FS_ERR', 'ASSETS_ERROR', 'ERR ')):
                raise DeviceError(resp)
            if skip_noise:
                continue
        return resp
    raise DeviceError(f"no reply to '{line}'")


def show_progress(line, pct, last_decile):
    """A terminal gets one line rewritten in place; a pipe (bench.py, a log)
    gets one line per 10 %, since it keeps every \\r-overwritten update."""
    if sys.stdout.isatty():
        print(f"\r{line}", end='', flush=True)
    elif pct // 10 != last_decile:
        print(line, flush=True)
        return pct // 10
    return last_decile


def stream(ser, data, label, final_ok, final_err):
    """Push `data`, keeping ACK_WINDOW chunks in flight.

    The device acks on 4096-byte boundaries, which is why the chunk size is
    exactly 4096 — a short chunk in the middle would leave both sides waiting.
    Strict lockstep costs a USB turnaround per chunk (measured 91 KB/s); one
    extra chunk in flight hides it (139 KB/s). The window cannot grow without
    bound: the device's CDC RX buffer is 128 B and the acks are what stops the
    host from overrunning it.
    """
    total = len(data)
    sent = acked = 0
    started = time.time()
    deciles = -1

    while sent < total:
        while sent < total and (sent - acked) < ACK_WINDOW * CHUNK_SIZE:
            end = min(sent + CHUNK_SIZE, total)
            ser.write(data[sent:end])
            sent = end

        resp = readline(ser)
        if not resp:
            raise DeviceError(f"timeout after {sent}/{total} bytes")
        if resp.startswith('OK '):
            acked = int(resp[3:].split('/')[0])
            elapsed = time.time() - started
            speed = acked / elapsed / 1024 if elapsed > 0 else 0
            deciles = show_progress(f"{label}: {acked * 100 // total}% ({acked}/{total}) "
                                    f"{speed:.0f} KB/s", acked * 100 // total, deciles)
        elif resp.startswith(final_err):
            raise DeviceError(resp)
        elif resp.startswith(final_ok):
            print()
            return resp
        # anything else is noise from the console or a stale line

    # Everything is on the device; the commit (CRC check, flash erase+write and
    # read-back verify, or the file rename) still has to finish.
    print()
    ser.timeout = 180
    while True:
        resp = readline(ser)
        if not resp:
            raise DeviceError("timeout waiting for the device to commit")
        if resp.startswith(final_ok):
            return resp
        if resp.startswith(final_err):
            raise DeviceError(resp)


# ── Commands ──────────────────────────────────────────────────────────────


def cmd_assets(ser, args):
    path = args.image or board_build_artifact('assets.bin')
    if path is None:
        print("Error: no assets.bin for this board — build first or pass a path")
        return EXIT_BENCH
    if not os.path.exists(path):
        print(f"Error: assets image not found: {path}")
        print("Build it first (idf.py -B build_<rev> -DSDKCONFIG=sdkconfig.<rev> build, "
              "idf.py board names <rev>) — it lands in that build dir as assets.bin")
        return EXIT_BENCH

    with open(path, 'rb') as f:
        image = f.read()
    crc = zlib.crc32(image) & 0xFFFFFFFF
    print(f"Assets image: {path} ({len(image)} bytes, crc {crc:08x})")

    flag = ' FORCE' if args.force else ''
    resp = command(ser, f"ASSETS_BEGIN {len(image)} {crc:08x}{flag}",
                   expect='ASSETS_READY')
    if resp != 'ASSETS_READY':
        raise DeviceError(resp)

    print("Device ready. Streaming (nothing is erased until the CRC matches)...")
    stream(ser, image, 'Assets', 'ASSETS_OK', 'ASSETS_ERROR')
    print("Assets written and verified. The device is restarting.")
    return EXIT_OK


def cmd_push(ser, args):
    if not os.path.exists(args.local):
        print(f"Error: local file not found: {args.local}")
        return EXIT_BENCH
    with open(args.local, 'rb') as f:
        data = f.read()
    if not data:
        print("Error: refusing to push an empty file")
        return EXIT_BENCH

    remote = args.remote
    if remote.endswith('/'):
        remote += os.path.basename(args.local)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f"Push: {args.local} -> {remote} ({len(data)} bytes, crc {crc:08x})")

    resp = command(ser, f"FS_PUT {len(data)} {crc:08x} {remote}", expect='FS_READY')
    if resp != 'FS_READY':
        raise DeviceError(resp)

    ok = stream(ser, data, 'Push', 'FS_OK', 'FS_ERR')
    print(f"Stored as {remote} ({ok})")
    return EXIT_OK


def cmd_dump(ser, args):
    """Read the assets partition back as a raw image. Reads the flash, not the
    mounted filesystem, so what comes back is what `assets` takes."""
    return receive(ser, "ASSETS_DUMP", args.local, "assets partition")


def cmd_pull(ser, args):
    remote = args.remote
    local = args.local or os.path.basename(remote)
    if os.path.isdir(local):
        local = os.path.join(local, os.path.basename(remote))
    return receive(ser, f"FS_GET 0 0 {remote}", local, remote)


def receive(ser, command, local, label):
    ser.write((command + "\n").encode())
    header = None
    deadline = time.time() + ser.timeout
    while time.time() < deadline:
        line = readline(ser)
        if not line:
            break
        if line.startswith('FS_DATA '):
            header = line
            break
        if line.startswith(('FS_ERR', 'ERR ')):
            raise DeviceError(line)
    if header is None:
        raise DeviceError(f"no FS_DATA header for {label}")

    size = int(header.split()[1])
    print(f"Pull: {label} -> {local} ({size} bytes)")

    got = bytearray()
    started = time.time()
    deciles = -1
    while len(got) < size:
        block = ser.read(min(65536, size - len(got)))
        if not block:
            raise DeviceError(f"timeout after {len(got)}/{size} bytes")
        got += block
        elapsed = time.time() - started
        speed = len(got) / elapsed / 1024 if elapsed > 0 else 0
        pct = len(got) * 100 // size if size else 100
        deciles = show_progress(f"Pull: {pct}% ({len(got)}/{size}) {speed:.0f} KB/s",
                                pct, deciles)
    print()

    trailer = readline(ser)
    while trailer == '':
        trailer = readline(ser)
    if trailer.startswith('FS_TRUNC'):
        raise DeviceError(trailer)
    if not trailer.startswith('FS_OK '):
        raise DeviceError(f"unexpected trailer: '{trailer}'")

    want = int(trailer.split()[1], 16)
    have = zlib.crc32(bytes(got)) & 0xFFFFFFFF
    if want != have:
        raise DeviceError(f"crc mismatch: device {want:08x}, received {have:08x}")

    with open(local, 'wb') as f:
        f.write(got)
    print(f"Wrote {local} ({size} bytes, crc {have:08x} verified)")
    return EXIT_OK


def _read_until_end(ser, first):
    lines = []
    if first:
        lines.append(first)
    while True:
        line = readline(ser)
        if line == 'FS_END':
            return lines
        if line.startswith(('FS_ERR', 'ERR ')):
            raise DeviceError(line)
        if not line:
            raise DeviceError("listing did not terminate with FS_END")
        lines.append(line)


def cmd_ls(ser, args):
    ser.write(f"FS_LIST {args.remote}\n".encode())
    entries = _read_until_end(ser, None)
    if not entries:
        print("(empty)")
    for e in entries:
        _, kind, size, name = e.split(' ', 3)
        print(f"{'d' if kind == 'D' else '-'} {int(size):>10}  {name}")
    return EXIT_OK


def cmd_roots(ser, args):
    ser.write(b"FS_ROOTS\n")
    lines = _read_until_end(ser, None)
    print(f"{'mount':<12}{'total':>12}{'used':>12}")
    for line in lines:
        parts = line.split()
        if len(parts) != 4 or parts[0] != 'FS_ROOTS:':
            continue
        root, total, used = parts[1:]
        print(f"{root:<12}{total:>12}{used:>12}")
    return EXIT_OK


def cmd_stat(ser, args):
    print(command(ser, f"FS_STAT {args.remote}", expect='FS_STAT'))
    return EXIT_OK


def cmd_rm(ser, args):
    resp = command(ser, f"FS_DELETE {args.remote}", expect='OK')
    if resp != 'OK':
        raise DeviceError(resp)
    print(f"Deleted {args.remote}")
    return EXIT_OK


def cmd_mkdir(ser, args):
    resp = command(ser, f"FS_MKDIR {args.remote}", expect='OK')
    if resp != 'OK':
        raise DeviceError(resp)
    print(f"Created {args.remote}")
    return EXIT_OK


# ── Entry point ───────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description='Assets flashing and file transfer over USB CDC for CrossPad')
    parser.add_argument('-p', '--port', default=None,
                        help='Serial port (auto-detect by VID:PID if omitted)')
    sub = parser.add_subparsers(dest='action', required=True)

    p = sub.add_parser('assets', help='flash a whole LittleFS assets image')
    p.add_argument('image', nargs='?', default=None,
                   help='assets image (default: the build of the connected/remembered board)')
    p.add_argument('--force', action='store_true',
                   help='skip the LittleFS superblock check on the image')
    p.set_defaults(func=cmd_assets)

    p = sub.add_parser('push', help='host -> device')
    p.add_argument('local')
    p.add_argument('remote', help='e.g. /sdcard/crosspad/kits/x.json')
    p.set_defaults(func=cmd_push)

    p = sub.add_parser('pull', help='device -> host')
    p.add_argument('remote')
    p.add_argument('local', nargs='?', default=None)
    p.set_defaults(func=cmd_pull)

    p = sub.add_parser('dump', help='read the assets partition back as an image')
    p.add_argument('local', help='output .img')
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser('ls', help='list a directory')
    p.add_argument('remote')
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser('roots', help='show the mounts and their usage')
    p.set_defaults(func=cmd_roots)

    p = sub.add_parser('stat', help='size and type of one path')
    p.add_argument('remote')
    p.set_defaults(func=cmd_stat)

    p = sub.add_parser('rm', help='delete a file or an empty directory')
    p.add_argument('remote')
    p.set_defaults(func=cmd_rm)

    p = sub.add_parser('mkdir', help='create a directory')
    p.add_argument('remote')
    p.set_defaults(func=cmd_mkdir)

    args = parser.parse_args()

    try:
        port = ensure_port(args.port)
    except SystemExit:
        # ota_flash.ensure_port exits 1 when it finds no board; here "no board"
        # is a bench problem, not a firmware one.
        return EXIT_BENCH
    ser = open_port(port)
    try:
        return args.func(ser, args)
    except DeviceError as e:
        print(f"\nDevice error: {e}")
        return EXIT_FAIL
    except serial.SerialException as e:
        print(f"\nSerial error: {e}")
        return EXIT_BENCH
    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())

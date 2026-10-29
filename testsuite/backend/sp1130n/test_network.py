#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic loopback only: real CLI, protocol, TCP transport and SANE backend."""
import ctypes
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("SP1130N_NETWORK_BUILD", str(ROOT / "build")))
CLI = BUILD / "sp1130nctl"
FIXTURE = BUILD / "network_fixture"


def u32(data, offset=0):
    return struct.unpack_from("<I", data, offset)[0]


def recv_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            if not data:
                return None
            raise AssertionError("Client truncated a request")
        data.extend(chunk)
    return bytes(data)


class Scanner:
    """Not a device emulator claim: responses are authored protocol fixtures."""
    def __init__(self, images=(), encrypted=0, busy=False, hold=False, short=False,
                 sheets=1, last_status=0x1006, short_blocks=False, fallback=False,
                 back_first=False, fault=None):
        self.images = images
        self.encrypted, self.busy = encrypted, busy
        self.hold, self.short = hold, short
        self.sheets, self.last_status = sheets, last_status
        self.short_blocks, self.fallback = short_blocks, fallback
        self.back_first, self.fault = back_first, fault
        self.commands, self.sessions, self.params, self.errors = [], 0, [], []
        self.closed_sessions = 0
        self.closed = threading.Condition()
        self.token = bytes(40)
        self.source = 4
        self.block = 0
        self.stop = threading.Event()
        self.image_ready = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.name = f"sp1130n:tcp:127.0.0.1:{self.port}"
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(3)
        self.listener.close()
        if self.thread.is_alive():
            raise AssertionError("Fixture thread did not stop")
        if self.errors and exc[0] is None:
            raise self.errors[0]

    def send(self, connection, data):
        # Include a split across byte 30 to exercise encryption-flag tracking.
        chunk_size = 4096 if len(data) > 1024 else 3
        for offset in range(0, len(data), chunk_size):
            try:
                connection.sendall(data[offset:offset + chunk_size])
            except (BrokenPipeError, ConnectionResetError):
                if self.encrypted or self.short or self.fault or self.stop.is_set():
                    return
                raise
            time.sleep(0.0001)

    def wait_closed(self, expected):
        with self.closed:
            if not self.closed.wait_for(lambda: self.closed_sessions == expected, timeout=1):
                raise AssertionError(f"Only {self.closed_sessions}/{expected} sessions closed")

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    connection, _ = self.listener.accept()
                except socket.timeout:
                    continue
                with connection:
                    connection.settimeout(2)
                    self.sessions += 1
                    blocks_on_session = 0
                    while not self.stop.is_set():
                        header = recv_exact(connection, 16)
                        if header is None:
                            break
                        assert u32(header) == 0x230
                        assert header[5] == header[7] == header[10] == header[11] == 0
                        op, sub = header[4], header[6]
                        selector = struct.unpack_from("<H", header, 8)[0]
                        value = u32(header, 12)
                        self.commands.append((op, sub, selector, value))
                        if self.hold:
                            self.stop.wait(0.5)
                            break
                        if (op, sub) == (0x12, 0x7a):
                            reply = bytearray(108)
                            reply[30] = self.encrypted
                            struct.pack_into("<I", reply, 32, int(self.busy))
                            self.send(connection, reply[:20] if self.short else reply)
                            if self.short:
                                break
                        elif (op, sub) == (0x12, 2):
                            reply = bytearray(32)
                            reply[11] = 0xc5  # LAN status is not a USB paper bitfield.
                            self.send(connection, reply)
                        elif (op, sub) == (0x2a, 0x78):
                            assert value == 40 and selector in (1, 2)
                            self.send(connection, bytes(32))
                            token = recv_exact(connection, 40)
                            assert token is not None
                            if selector == 1:
                                self.token = token
                            else:
                                assert token == self.token
                        elif (op, sub) == (0x28, 0x78):
                            assert selector == 1 and value == 40
                            self.send(connection, bytes(32) + self.token)
                        elif (op, sub) == (0x2a, 9):
                            assert selector == 4 and value == 260
                            self.send(connection, bytes(32))
                            params = recv_exact(connection, 260)
                            assert params is not None
                            self.params.append(params)
                            self.source = u32(params)
                            assert self.source == 4
                            self.block = 0
                        elif (op, sub) == (0x14, 5) or (op, sub) == (0x14, 4):
                            self.send(connection, bytes(32))
                        elif (op, sub) == (0x14, 6):
                            self.send(connection, bytes(32))
                        elif (op, sub) == (0x28, 0x10):
                            assert selector == 0 and value == 288
                            self.send(connection, bytes(32) + bytes(range(256)) + bytes(32))
                        elif (op, sub) == (0x28, 4):
                            self.image_ready.set()
                            assert selector == value == 0
                            # Every block request must remain on one image connection.
                            assert blocks_on_session == self.block
                            blocks_on_session += 1
                            if self.fault == "reset" and self.block == 4:
                                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                                      struct.pack("ii", 1, 0))
                                break
                            if self.block == self.sheets * 4:
                                break  # Empty feeder closes only at a sheet boundary.
                            if self.fault == "missing_back" and self.block == 2:
                                break
                            if self.fault == "wait_image" and self.block == 0:
                                self.stop.wait(0.3)
                                break
                            if self.fault == "next_header" and self.block == 4:
                                self.send(connection, bytes(16))
                                break
                            if self.fault == "next_payload" and self.block == 4:
                                reply = bytearray(64)
                                struct.pack_into("<I", reply, 36, 100)
                                self.send(connection, reply + bytes(10))
                                break
                            if self.fault == "timeout" and self.block == 4:
                                self.stop.wait(6)
                                break
                            sides = (1, 0) if self.back_first else (0, 1)
                            side = sides[(self.block // 2) % 2]
                            image = self.images[side]
                            if self.fault == ("bad_back" if side else "bad_front"):
                                image = image[:-2]
                            half = 0x80000 if self.short_blocks else len(image) // 2
                            payload = image[:half] if self.block % 2 == 0 else image[half:]
                            final = self.block % 2 == 1
                            reply = bytearray(32 if self.short_blocks and not final else 64)
                            error = self.last_status if self.block == self.sheets * 4 - 1 else 0x1005
                            struct.pack_into("<I", reply, 0, error if final else 0)
                            reply[11] = side
                            if not final:
                                struct.pack_into("<II", reply, 16, 3, 4)
                            if len(reply) == 32:
                                struct.pack_into("<II", reply, 0, 0x80000, 0x80000)
                                reply[8:] = bytes(24)  # No reliable side or geometry.
                            else:
                                struct.pack_into("<I", reply, 32 if self.fallback else 36, len(payload))
                            if self.fault == "bad_short" and self.block == 0:
                                struct.pack_into("<II", reply, 0, 0x80000, 0)
                            if self.fault == "oversize" and self.block == 0:
                                struct.pack_into("<I", reply, 36, 64 * 1024 * 1024 + 1)
                                payload = b""
                            if self.fault == "jam" and self.block == 4:
                                struct.pack_into("<I", reply, 0, 0x2002)
                                self.send(connection, reply[:32])
                                break
                            self.send(connection, reply + payload)
                            self.block += 1
                        else:
                            raise AssertionError(f"Unexpected command {(op, sub, selector, value)}")
                with self.closed:
                    self.closed_sessions += 1
                    self.closed.notify_all()
        except BaseException as error:
            self.errors.append(error)


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.images = tuple(
            subprocess.run([str(FIXTURE), "jpeg", side], capture_output=True, check=True).stdout
            for side in ("front", "back"))

    def cli(self, *args, **kwargs):
        return subprocess.run([str(CLI), *map(str, args)], capture_output=True, timeout=10,
                              **kwargs)

    def test_numeric_address_validation_without_network(self):
        for name in ("tcp:127.0.0.1", "sp1130n:tcp:192.0.2.3:16888",
                     "tcp:127.0.0.1:65535"):
            result = subprocess.run([str(FIXTURE), "parse", name], capture_output=True, check=True)
            self.assertEqual(result.stdout.strip(), b"0")
        for name in ("tcp:", "tcp:localhost", "tcp:::1", "tcp:0.0.0.0",
                     "tcp:224.0.0.1", "tcp:255.255.255.255", "tcp:300.1.1.1",
                     "tcp:127.0.0.1:0", "tcp:127.0.0.1:-1", "tcp:127.0.0.1:65536",
                     "tcp:127.0.0.1:1234567890", "tcp:127.0.0.1:1x", "tcp:127.0.0.1:"):
            result = subprocess.run([str(FIXTURE), "parse", name], capture_output=True, check=True)
            self.assertEqual(result.stdout.strip(), b"4", name)

    def test_fragmented_extended_inquiry(self):
        with Scanner() as scanner:
            result = self.cli("status", scanner.name)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"Firmware error: 0x00000000", result.stdout)
        self.assertEqual(scanner.commands, [(0x12, 0x7a, 108, 0)])
        self.assertEqual(scanner.sessions, 1)

    def test_encrypted_mode_refused_before_lock_or_scan(self):
        for flag in (1, 2, 255):
            with Scanner(encrypted=flag) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
                result = self.cli("scan", scanner.name, "--experimental", *paths)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"Unsupported operation", result.stderr)
                self.assertTrue(all(path.stat().st_size == 0 for path in paths))
            self.assertEqual(scanner.commands, [(0x12, 0x7a, 108, 0)])

    def test_short_reply_is_not_success(self):
        with Scanner(short=True) as scanner:
            result = self.cli("status", scanner.name)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"I/O or protocol error", result.stderr)

    def test_busy_scanner_is_not_unlocked(self):
        with Scanner(busy=True) as scanner, tempfile.TemporaryDirectory() as directory:
            result = self.cli("scan", scanner.name, "--experimental",
                              Path(directory) / "front.ppm", Path(directory) / "back.ppm")
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"Device busy", result.stderr)
        self.assertEqual(scanner.commands, [(0x12, 0x7a, 108, 0)])

    def test_real_cli_duplex_and_front_back_sources(self):
        for source, sides in (("duplex", (0, 1)), ("front", (0,)), ("back", (1,))):
            with Scanner(self.images) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / f"{side}.ppm" for side in sides]
                result = self.cli("scan", scanner.name, "--experimental", "--source", source,
                                  *paths)
                self.assertEqual(result.returncode, 0, result.stderr)
                for side, path in zip(sides, paths):
                    data = path.read_bytes()
                    self.assertTrue(data.startswith(b"P6\n4 3\n255\n"), data[:30])
                    self.assertEqual(len(data.split(b"\n", 3)[3]), 36)
                    self.assertLess(abs(data[-1] - (200 if side else 40)), 3)
            self.assertEqual(scanner.sessions, 9)
            self.assertEqual(u32(scanner.params[0]), 4)
            self.assertEqual(struct.unpack_from("<H", scanner.params[0], 16)[0], 300)
            self.assertEqual(u32(scanner.params[0], 20), 0)
            self.assertEqual(u32(scanner.params[0], 24), 0)
            self.assertEqual(u32(scanner.params[0], 32), 3508)
            self.assertEqual(scanner.params[0][60], 0x75)
            self.assertEqual(scanner.params[0][67], 4)
            self.assertEqual(u32(scanner.params[0], 100), 0)
            self.assertEqual(sum(op == 0x28 and sub == 4 for op, sub, _, _ in scanner.commands),
                             4)
            self.assertNotIn((0x14, 6, 0, 0), scanner.commands)
            self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

    def test_unverified_lan_geometry_refused_before_device_io(self):
        for arguments in (("--resolution", "600"), ("--left", "1"),
                          ("--page-width", "216"), ("--scan-height", "200")):
            with Scanner() as scanner, tempfile.TemporaryDirectory() as directory:
                result = self.cli("scan", scanner.name, "--experimental", *arguments,
                                  Path(directory) / "front.ppm", Path(directory) / "back.ppm")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"Unsupported operation", result.stderr)
            self.assertEqual(scanner.sessions, 0)

    @staticmethod
    def padded_jpeg(image):
        # Valid APP records push image data past the 512 KiB short-block boundary.
        return image[:2] + (
            b"\xff\xef\xff\xff" + b"\xff\xd9\xff\xd8" + bytes(65529)
        ) * 9 + image[2:]

    def test_short_headers_and_fallback_lengths(self):
        padded = tuple(self.padded_jpeg(image) for image in self.images)
        for images, short_blocks, fallback in ((padded, True, False),
                                               (self.images, False, True)):
            with Scanner(images, short_blocks=short_blocks, fallback=fallback,
                         back_first=True) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
                result = self.cli("scan", scanner.name, "--experimental", *paths)
                self.assertEqual(result.returncode, 0, result.stderr)
                for path, expected in zip(paths, (40, 200)):
                    self.assertLess(abs(path.read_bytes()[-1] - expected), 3)

    def test_bad_headers_and_incomplete_duplex_fail(self):
        for fault, message in (("missing_back", b"I/O or protocol error"),
                               ("bad_short", b"I/O or protocol error"),
                               ("oversize", b"Out of memory")):
            with Scanner(self.images, fault=fault) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
                result = self.cli("scan", scanner.name, "--experimental", *paths)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertTrue(all(path.stat().st_size == 0 for path in paths))
            self.assertEqual(scanner.commands[-2:], [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])

    def test_simplex_still_validates_discarded_side(self):
        for source, fault in (("front", "bad_back"), ("back", "bad_front")):
            with Scanner(self.images, fault=fault) as scanner, tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "side.ppm"
                result = self.cli("scan", scanner.name, "--experimental", "--source", source, path)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"I/O or protocol error", result.stderr)
                self.assertEqual(path.stat().st_size, 0)

    def test_read_only_maintenance_over_lan(self):
        with Scanner() as scanner, tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "snapshot.bin"
            result = self.cli("maintenance", scanner.name, "--experimental", path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(path.read_bytes(), bytes(range(256)) + bytes(32))
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(scanner.sessions, 5)
        self.assertEqual(scanner.commands[-2:], [(0x28, 0x10, 0, 288), (0x2a, 0x78, 2, 40)])

    def test_poll_timeout_and_cancellation(self):
        for operation in ("cancel", "timeout"):
            with Scanner(hold=True) as scanner:
                start = time.monotonic()
                subprocess.run([str(FIXTURE), operation, scanner.name[8:]], check=True, timeout=2)
                self.assertLess(time.monotonic() - start, 1)

    def test_no_connection_for_dry_run_or_missing_opt_in(self):
        with Scanner() as scanner:
            result = self.cli("scan", scanner.name, "--dry-run")
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.cli("scan", scanner.name, "front.ppm", "back.ppm")
            self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(scanner.sessions, 0)

    def test_dry_run_rejects_unverified_lan_profiles_without_connecting(self):
        with Scanner() as scanner:
            for arguments in (("--resolution", "600"), ("--left", "1"),
                              ("--page-width", "216"), ("--scan-height", "200")):
                with self.subTest(arguments=arguments):
                    result = self.cli("scan", scanner.name, "--dry-run", *arguments)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(b"Unsupported operation", result.stderr)
                    self.assertFalse(result.stdout)
            result = self.cli("scan", scanner.name, "--dry-run", "--scan-width", "210",
                              "--scan-height", "297", "--source", "back")
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(scanner.sessions, 0)

    def test_dry_run_validates_targets_offline(self):
        invalid = ("not-a-device", "usb:001:002", "sp1130n:usb:1:2",
                   "sp1130n:usb:256:001", "sp1130n:usb:001:999",
                   "sp1130n:usb:00x:002", "sp1130n:tcp:localhost",
                   "sp1130n:tcp:0.0.0.0", "sp1130n:tcp:224.0.0.1",
                   "sp1130n:tcp:127.0.0.1:0", "sp1130n:tcp:127.0.0.1:65536",
                   "sp1130n:tcp:127.0.0.1:1:2")
        valid = ("sp1130n:usb:001:002", "sp1130n:usb:255:255",
                 "sp1130n:tcp:192.0.2.1", "tcp:192.0.2.1:65535")
        for command in ("scan", "maintenance"):
            for device in invalid:
                with self.subTest(command=command, device=device):
                    result = self.cli(command, device, "--dry-run")
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(b"Invalid argument", result.stderr)
                    self.assertFalse(result.stdout)
            for device in valid:
                with self.subTest(command=command, device=device):
                    result = self.cli(command, device, "--dry-run")
                    self.assertEqual(result.returncode, 0, result.stderr)
        result = self.cli("scan", "sp1130n:usb:001:002", "--dry-run", "--resolution", "600")
        self.assertEqual(result.returncode, 0, result.stderr)

    @staticmethod
    def sane_library():
        library = ctypes.CDLL(os.environ.get(
            "SP1130N_NETWORK_LIBRARY", str(BUILD / "libsane-sp1130n.so.1")))
        library.sane_sp1130n_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        library.sane_sp1130n_start.argtypes = [ctypes.c_void_p]
        library.sane_sp1130n_read.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        library.sane_sp1130n_close.argtypes = [ctypes.c_void_p]
        library.sane_sp1130n_cancel.argtypes = [ctypes.c_void_p]
        library.sane_sp1130n_control_option.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int)]
        return library

    def sane_read_page(self, library, handle, expected):
        output = bytearray()
        while True:
            data = ctypes.create_string_buffer(7)
            count = ctypes.c_int(-1)
            status = library.sane_sp1130n_read(handle, data, 7, ctypes.byref(count))
            if status == 5:
                self.assertEqual(count.value, 0)
                break
            self.assertEqual(status, 0)
            self.assertGreater(count.value, 0)
            output.extend(data.raw[:count.value])
        self.assertEqual(len(output), 36)
        self.assertLess(abs(output[-1] - expected), 3)

    def test_sane_real_duplex_tcp_and_eof(self):
        library = self.sane_library()
        old = os.environ.get("SP1130N_EXPERIMENTAL")
        os.environ["SP1130N_EXPERIMENTAL"] = "1"
        try:
            for last_status in (0x1005, 0x1006):
                with Scanner(self.images, sheets=3, last_status=last_status,
                             back_first=True) as scanner:
                    handle = ctypes.c_void_p()
                    self.assertEqual(library.sane_sp1130n_open(scanner.name[8:].encode(),
                                                            ctypes.byref(handle)), 0)
                    try:
                        for expected in (40, 200) * 3:
                            self.assertEqual(library.sane_sp1130n_start(handle), 0)
                            self.sane_read_page(library, handle, expected)
                        self.assertEqual(library.sane_sp1130n_start(handle), 7)
                        before = len(scanner.commands)
                        self.assertEqual(library.sane_sp1130n_start(handle), 7)
                        self.assertEqual(len(scanner.commands), before)
                        scanner.wait_closed(9)
                    finally:
                        library.sane_sp1130n_close(handle)
                self.assertEqual(scanner.sessions, 9)
                self.assertEqual(len(scanner.params), 1)
                self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
                self.assertNotIn((0x14, 6, 0, 0), scanner.commands)
        finally:
            if old is None:
                os.environ.pop("SP1130N_EXPERIMENTAL", None)
            else:
                os.environ["SP1130N_EXPERIMENTAL"] = old

    def test_sane_partial_next_sheet_is_not_end_of_batch(self):
        library = self.sane_library()
        old = os.environ.get("SP1130N_EXPERIMENTAL")
        os.environ["SP1130N_EXPERIMENTAL"] = "1"
        try:
            for fault, expected in (("next_header", 9), ("next_payload", 9), ("jam", 6),
                                     ("timeout", 9), ("reset", 9)):
                with Scanner(self.images, sheets=2, fault=fault) as scanner:
                    handle = ctypes.c_void_p()
                    self.assertEqual(library.sane_sp1130n_open(scanner.name[8:].encode(),
                                                            ctypes.byref(handle)), 0)
                    try:
                        for byte in (40, 200):
                            self.assertEqual(library.sane_sp1130n_start(handle), 0)
                            self.sane_read_page(library, handle, byte)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        before = len(scanner.commands)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        self.assertEqual(len(scanner.commands), before)
                    finally:
                        library.sane_sp1130n_close(handle)
        finally:
            if old is None:
                os.environ.pop("SP1130N_EXPERIMENTAL", None)
            else:
                os.environ["SP1130N_EXPERIMENTAL"] = old

    def test_sane_cancel_releases_live_batch_and_allows_new_job(self):
        library = self.sane_library()
        old = os.environ.get("SP1130N_EXPERIMENTAL")
        os.environ["SP1130N_EXPERIMENTAL"] = "1"
        try:
            with Scanner(self.images, sheets=2) as scanner:
                handle = ctypes.c_void_p()
                self.assertEqual(library.sane_sp1130n_open(scanner.name[8:].encode(),
                                                        ctypes.byref(handle)), 0)
                try:
                    invalid_dpi = ctypes.c_int(600)
                    self.assertEqual(library.sane_sp1130n_control_option(
                        handle, 3, 1, ctypes.byref(invalid_dpi), None), 4)
                    self.assertEqual(library.sane_sp1130n_start(handle), 0)
                    self.sane_read_page(library, handle, 40)
                    library.sane_sp1130n_cancel(handle)
                    # Cancellation itself is signal-safe; a normal call reaps it.
                    byte = ctypes.create_string_buffer(1)
                    count = ctypes.c_int(-1)
                    self.assertEqual(library.sane_sp1130n_read(
                        handle, byte, 1, ctypes.byref(count)), 2)
                    self.assertEqual(count.value, 0)
                    scanner.wait_closed(10)
                    self.assertEqual(scanner.commands[-2:],
                                     [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])
                    self.assertEqual(library.sane_sp1130n_start(handle), 0)
                    self.sane_read_page(library, handle, 40)
                finally:
                    library.sane_sp1130n_close(handle)
            self.assertEqual(len(scanner.params), 2)
            self.assertEqual(scanner.sessions, 20)
        finally:
            if old is None:
                os.environ.pop("SP1130N_EXPERIMENTAL", None)
            else:
                os.environ["SP1130N_EXPERIMENTAL"] = old

    def test_sane_cancel_during_image_read(self):
        library = self.sane_library()
        old = os.environ.get("SP1130N_EXPERIMENTAL")
        os.environ["SP1130N_EXPERIMENTAL"] = "1"
        try:
            with Scanner(self.images, fault="wait_image") as scanner:
                handle = ctypes.c_void_p()
                self.assertEqual(library.sane_sp1130n_open(scanner.name[8:].encode(),
                                                        ctypes.byref(handle)), 0)
                result = []
                worker = threading.Thread(target=lambda: result.append(
                    library.sane_sp1130n_start(handle)))
                try:
                    worker.start()
                    self.assertTrue(scanner.image_ready.wait(2))
                    library.sane_sp1130n_cancel(handle)
                    worker.join(3)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(result, [2])
                    scanner.wait_closed(10)
                finally:
                    library.sane_sp1130n_close(handle)
                    worker.join(3)
        finally:
            if old is None:
                os.environ.pop("SP1130N_EXPERIMENTAL", None)
            else:
                os.environ["SP1130N_EXPERIMENTAL"] = old

    def test_sane_blank_filter_uses_same_feeder_job(self):
        library = self.sane_library()
        old = os.environ.get("SP1130N_EXPERIMENTAL")
        os.environ["SP1130N_EXPERIMENTAL"] = "1"
        try:
            with Scanner(self.images, sheets=2) as scanner:
                handle = ctypes.c_void_p()
                self.assertEqual(library.sane_sp1130n_open(scanner.name[8:].encode(),
                                                        ctypes.byref(handle)), 0)
                try:
                    for option, value in ((16, 1), (17, 0)):
                        word = ctypes.c_int(value)
                        self.assertEqual(library.sane_sp1130n_control_option(
                            handle, option, 1, ctypes.byref(word), None), 0)
                    self.assertEqual(library.sane_sp1130n_start(handle), 7)
                    scanner.wait_closed(9)
                    self.assertEqual(scanner.block, 8)
                finally:
                    library.sane_sp1130n_close(handle)
            self.assertEqual(len(scanner.params), 1)
            self.assertNotIn((0x14, 6, 0, 0), scanner.commands)
        finally:
            if old is None:
                os.environ.pop("SP1130N_EXPERIMENTAL", None)
            else:
                os.environ["SP1130N_EXPERIMENTAL"] = old


if __name__ == "__main__":
    unittest.main(verbosity=2)

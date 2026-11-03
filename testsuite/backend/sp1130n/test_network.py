#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic loopback only: real CLI, protocol, TCP transport and SANE backend."""
import ctypes
import json
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
                 back_first=False, fault=None, start_image=False,
                 provisional_height=False, boundary_record=False,
                 paper_tag=0xc5000002, paper_status4=0x04000000,
                 paper_error=0, paper_width=0, paper_height=0,
                 end_boundary=None, end_paper_tag=0xc5000000,
                 end_paper_error=0, sheet_images=(), image_width=4, image_height=3,
                 first_image_delay=0):
        self.images = images
        self.image_width, self.image_height = image_width, image_height
        self.first_image_delay = first_image_delay
        self.sheet_images = sheet_images
        if sheet_images:
            assert len(sheet_images) == sheets
            assert all(len(pair) == 2 for pair in sheet_images)
        self.encrypted, self.busy = encrypted, busy
        self.hold, self.short = hold, short
        self.sheets, self.last_status = sheets, last_status
        self.short_blocks, self.fallback = short_blocks, fallback
        self.back_first, self.fault = back_first, fault
        self.start_image = start_image
        self.provisional_height, self.boundary_record = provisional_height, boundary_record
        self.paper_tag = paper_tag
        self.paper_status4 = paper_status4
        self.paper_error = paper_error
        self.paper_width, self.paper_height = paper_width, paper_height
        self.end_boundary, self.end_paper_tag = end_boundary, end_paper_tag
        self.end_paper_error = end_paper_error
        self.end_record_sent = False
        self.boundaries = set()
        self.commands, self.sessions, self.params, self.errors = [], 0, [], []
        self.image_sides = []
        self.closed_sessions = 0
        self.closed = threading.Condition()
        self.token = bytes(40)
        self.source = 4
        self.block = 0
        self.stop = threading.Event()
        self.image_ready = threading.Event()
        self.next_sheet_ready = threading.Event()
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
                if (self.encrypted or self.short or self.fault or
                        self.first_image_delay or self.stop.is_set()):
                    return
                raise
            time.sleep(0.0001)

    def wait_closed(self, expected):
        with self.closed:
            if not self.closed.wait_for(lambda: self.closed_sessions == expected, timeout=1):
                raise AssertionError(f"Only {self.closed_sessions}/{expected} sessions closed")

    def image(self, side):
        pair = self.sheet_images[self.block // 4] if self.sheet_images else self.images
        return pair[side]

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
                            ended = self.block == self.sheets * 4
                            struct.pack_into("<III", reply, 0,
                                             self.end_paper_error if ended else self.paper_error,
                                             self.paper_status4,
                                             self.end_paper_tag if ended else self.paper_tag)
                            struct.pack_into("<II", reply, 16, self.paper_height,
                                             self.paper_width)
                            self.send(connection, reply)
                        elif (op, sub) == (0x2a, 0x78):
                            assert value == 40 and selector in (1, 2)
                            if selector == 2 and self.fault == "unlock":
                                reply = bytearray(32)
                                struct.pack_into("<I", reply, 0, 0x4004)
                                self.send(connection, reply)
                                break
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
                            self.boundaries.clear()
                            self.end_record_sent = False
                        elif (op, sub) == (0x14, 4) and self.start_image:
                            # START may return image metadata instead of an idle ACK.
                            # The first prefix has width but no final height yet.
                            side = int(self.back_first)
                            image = self.image(side)
                            payload = image[:len(image) // 2]
                            reply = bytearray(64)
                            reply[11] = 1 - side  # Face-down: 1 FRONT, 0 BACK.
                            self.image_sides.append(reply[11])
                            struct.pack_into("<I", reply, 20,
                                             5 if self.fault == "start_width" else 4)
                            struct.pack_into("<I", reply, 36, len(payload))
                            if self.fault == "start_header":
                                self.send(connection, reply[:16])
                                break
                            if self.fault == "start_payload":
                                self.send(connection, reply + payload[:-1])
                                break
                            if self.fault == "wait_start_payload":
                                self.send(connection, reply[:32])
                                self.image_ready.set()
                                self.stop.wait(0.3)
                                break
                            self.send(connection, reply + payload)
                            self.block = blocks_on_session = 1
                            self.image_ready.set()
                        elif (op, sub) == (0x14, 5) or (op, sub) == (0x14, 4):
                            reply = bytearray(32)
                            reply[11] = 0xc5  # Ordinary control metadata is not a side.
                            self.send(connection, reply)
                        elif (op, sub) == (0x14, 6):
                            self.send(connection, bytes(32))
                        elif (op, sub) == (0x28, 0x10):
                            assert selector == 0 and value == 288
                            self.send(connection, bytes(32) + bytes(range(256)) + bytes(32))
                        elif (op, sub) == (0x28, 4):
                            self.image_ready.set()
                            assert selector == value == 0
                            if not self.block and self.stop.wait(self.first_image_delay):
                                break
                            # Every block request must remain on one image connection.
                            assert blocks_on_session == self.block
                            if self.end_boundary and self.block == self.sheets * 4:
                                if self.end_boundary == "direct_close":
                                    break
                                if not self.end_record_sent:
                                    reply = bytearray(64)
                                    struct.pack_into("<I", reply, 12, 1)
                                    self.send(connection, reply)
                                    self.end_record_sent = True
                                    if self.end_boundary in ("reset", "partial_header",
                                                             "partial_payload"):
                                        continue
                                if self.end_boundary == "reset":
                                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                                          struct.pack("ii", 1, 0))
                                elif self.end_boundary == "timeout":
                                    self.stop.wait(6)
                                elif self.end_boundary == "partial_header":
                                    self.send(connection, bytes(16))
                                elif self.end_boundary == "partial_payload":
                                    reply = bytearray(64)
                                    struct.pack_into("<II", reply, 16, 3, 4)
                                    struct.pack_into("<I", reply, 36, 100)
                                    self.send(connection, reply + bytes(10))
                                break
                            if (self.boundary_record and self.block and
                                    self.block % 4 == 0 and self.block not in self.boundaries):
                                reply = bytearray(64)
                                struct.pack_into("<I", reply, 12, 1)
                                self.send(connection, reply)
                                self.boundaries.add(self.block)
                                continue
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
                            if self.fault == "initial_device_error" and self.block == 0:
                                reply = bytearray(32)
                                struct.pack_into("<I", reply, 0, 0x4004)
                                self.send(connection, reply)
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
                            if self.fault == "wait_next" and self.block == 4:
                                self.next_sheet_ready.set()
                                self.stop.wait(0.3)
                                break
                            sides = (1, 0) if self.back_first else (0, 1)
                            side = sides[(self.block // 2) % 2]
                            image = self.image(side)
                            if self.fault == ("bad_back" if side else "bad_front"):
                                image = image[:-2]
                            half = 0x80000 if self.short_blocks else len(image) // 2
                            payload = image[:half] if self.block % 2 == 0 else image[half:]
                            final = self.block % 2 == 1
                            reply = bytearray(32 if self.short_blocks and not final else 64)
                            error = self.last_status if self.block == self.sheets * 4 - 1 else 0x1005
                            struct.pack_into("<I", reply, 0, error if final else 0)
                            reply[11] = 1 - side
                            if self.provisional_height:
                                struct.pack_into("<II", reply, 16,
                                                 self.image_height if final else 0,
                                                 self.image_width)
                            elif not final:
                                struct.pack_into("<II", reply, 16,
                                                 self.image_height, self.image_width)
                            if len(reply) == 32:
                                struct.pack_into("<II", reply, 0, 0x80000, 0x80000)
                                reply[8:] = bytes(24)  # No reliable side or geometry.
                            else:
                                self.image_sides.append(reply[11])
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
                result = self.cli("scan", scanner.name, *paths)
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
            result = self.cli("scan", scanner.name,
                              Path(directory) / "front.ppm", Path(directory) / "back.ppm")
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"Device busy", result.stderr)
        self.assertEqual(scanner.commands, [(0x12, 0x7a, 108, 0)])

    def test_empty_paper_status_prevents_start_and_latches_no_documents(self):
        library = self.sane_library()
        with Scanner(paper_tag=0xc5000000) as scanner:
            handle = ctypes.c_void_p()
            self.assertEqual(library.sane_sp1130n_open(
                scanner.name[8:].encode(), ctypes.byref(handle)), 0)
            try:
                self.assertEqual(library.sane_sp1130n_start(handle), 7)
                before = len(scanner.commands)
                self.assertEqual(library.sane_sp1130n_start(handle), 7)
                self.assertEqual(len(scanner.commands), before)
                data = ctypes.create_string_buffer(1)
                count = ctypes.c_int(-1)
                self.assertEqual(library.sane_sp1130n_read(
                    handle, data, 1, ctypes.byref(count)), 4)
                self.assertEqual(count.value, 0)
            finally:
                library.sane_sp1130n_close(handle)
        self.assertEqual(scanner.commands,
                         [(0x12, 0x7a, 108, 0), (0x2a, 0x78, 1, 40),
                          (0x28, 0x78, 1, 40), (0x12, 2, 32, 0),
                          (0x2a, 0x78, 2, 40)])
        self.assertEqual(scanner.sessions, 5)
        self.assertFalse(scanner.params)

    def test_unrecognized_paper_status_is_not_guessed_empty(self):
        with Scanner(self.images, paper_tag=0) as scanner, tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
            result = self.cli("scan", scanner.name, *paths)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)

    def test_cover_paper_status_prevents_start_and_recovers_after_cancel(self):
        library = self.sane_library()
        with Scanner(paper_tag=0xc5000001) as scanner:
            handle = ctypes.c_void_p()
            self.assertEqual(library.sane_sp1130n_open(
                scanner.name[8:].encode(), ctypes.byref(handle)), 0)
            try:
                self.assertEqual(library.sane_sp1130n_start(handle), 8)
                before = len(scanner.commands)
                self.assertEqual(library.sane_sp1130n_start(handle), 8)
                self.assertEqual(len(scanner.commands), before)
                data = ctypes.create_string_buffer(1)
                count = ctypes.c_int(-1)
                self.assertEqual(library.sane_sp1130n_read(
                    handle, data, 1, ctypes.byref(count)), 4)
                self.assertEqual(count.value, 0)
                self.assertEqual(scanner.commands,
                                 [(0x12, 0x7a, 108, 0), (0x2a, 0x78, 1, 40),
                                  (0x28, 0x78, 1, 40), (0x12, 2, 32, 0),
                                  (0x2a, 0x78, 2, 40)])
                scanner.paper_tag = 0xc5000000
                library.sane_sp1130n_cancel(handle)
                self.assertEqual(library.sane_sp1130n_start(handle), 7)
                before = len(scanner.commands)
                self.assertEqual(library.sane_sp1130n_start(handle), 7)
                self.assertEqual(len(scanner.commands), before)
                scanner.paper_tag = 0xc5000002
                scanner.images = self.images
                library.sane_sp1130n_cancel(handle)
                for expected in (40, 200):
                    self.assertEqual(library.sane_sp1130n_start(handle), 0)
                    self.sane_read_page(library, handle, expected)
            finally:
                library.sane_sp1130n_close(handle)
        self.assertEqual(len(scanner.params), 1)
        self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
        self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

    def test_cover_status_conflicts_do_not_start_or_hide_firmware_errors(self):
        library = self.sane_library()
        for width, height, error, expected in (
                (4, 0, 0, 9), (0, 3, 0, 9), (0, 0, 0x2002, 6),
                (0, 0, 0x4004, 9), (0, 0, 0x2009, 8)):
            with self.subTest(width=width, height=height, error=error):
                with Scanner(paper_tag=0xc5000001, paper_width=width,
                             paper_height=height, paper_error=error) as scanner:
                    handle = ctypes.c_void_p()
                    self.assertEqual(library.sane_sp1130n_open(
                        scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                    try:
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        before = len(scanner.commands)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        self.assertEqual(len(scanner.commands), before)
                    finally:
                        library.sane_sp1130n_close(handle)
                self.assertFalse(scanner.params)
                self.assertNotIn((0x14, 5, 0, 0), scanner.commands)
                self.assertNotIn((0x14, 4, 0, 0), scanner.commands)
                self.assertNotIn((0x14, 6, 0, 0), scanner.commands)
                self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

    def test_empty_boundary_close_requires_confirmed_paper_status(self):
        library = self.sane_library()
        for ending, tag, error, expected in (
                ("close", 0xc5000000, 0, 7), ("reset", 0xc5000000, 0, 7),
                ("close", 0xc5000002, 0, 9), ("close", 0, 0, 9),
                ("close", 0xc5000001, 0, 8),
                ("reset", 0xc5000000, 0x2002, 6),
                ("partial_header", 0xc5000000, 0, 9),
                ("partial_payload", 0xc5000000, 0, 9),
                ("timeout", 0xc5000000, 0, 9)):
            with self.subTest(ending=ending, tag=tag, error=error):
                with Scanner(self.images, last_status=0x1005,
                             end_boundary=ending, end_paper_tag=tag,
                             end_paper_error=error) as scanner:
                    handle = ctypes.c_void_p()
                    self.assertEqual(library.sane_sp1130n_open(
                        scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                    try:
                        for value in (40, 200):
                            self.assertEqual(library.sane_sp1130n_start(handle), 0)
                            self.sane_read_page(library, handle, value)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        before = len(scanner.commands)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        self.assertEqual(len(scanner.commands), before)
                    finally:
                        library.sane_sp1130n_close(handle)
                self.assertEqual(scanner.commands.count((0x12, 2, 32, 0)),
                                 1 if ending in ("timeout", "partial_header",
                                                 "partial_payload") else 2)
                self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))
                self.assertEqual((0x14, 6, 0, 0) in scanner.commands, expected != 7)

    def test_direct_sheet_boundary_close_requires_confirmed_paper_status(self):
        library = self.sane_library()
        for tag, error, expected in (
                (0xc5000000, 0, 7), (0xc5000002, 0, 9), (0, 0, 9),
                (0xc5000001, 0, 8), (0xc5000000, 0x2002, 6)):
            with self.subTest(tag=tag, error=error):
                with Scanner(self.images, last_status=0x1005,
                             end_boundary="direct_close", end_paper_tag=tag,
                             end_paper_error=error) as scanner:
                    handle = ctypes.c_void_p()
                    self.assertEqual(library.sane_sp1130n_open(
                        scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                    try:
                        for value in (40, 200):
                            self.assertEqual(library.sane_sp1130n_start(handle), 0)
                            self.sane_read_page(library, handle, value)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        before = len(scanner.commands)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        self.assertEqual(len(scanner.commands), before)
                    finally:
                        library.sane_sp1130n_close(handle)
                self.assertEqual(scanner.commands.count((0x12, 2, 32, 0)), 2)
                self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
                self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))
                self.assertEqual((0x14, 6, 0, 0) in scanner.commands, expected != 7)

    def test_real_cli_duplex_and_front_back_sources(self):
        for source, sides in (("duplex", (0, 1)), ("front", (0,)), ("back", (1,))):
            with Scanner(self.images) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / f"{side}.ppm" for side in sides]
                result = self.cli("scan", scanner.name, "--source", source,
                                  *paths)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn(b"Protocol ", result.stderr)
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

    def test_cli_protocol_trace_is_opt_in_metadata_only(self):
        with Scanner(self.images) as scanner, tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / f"{side}.ppm" for side in ("front", "back")]
            result = self.cli("scan", scanner.name, "--trace-protocol", *paths)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = [line for line in result.stderr.splitlines()
                     if line.startswith(b"Protocol ")]
            self.assertGreater(len(lines), 5)
            for line in lines:
                self.assertRegex(
                    line, rb"^Protocol [A-Za-z ]+: status=-?[0-9]+"
                    rb"( error=0x[0-9a-f]{8} event=[0-9]+ tag=0x[0-9a-f]{8}"
                    rb" side=[0-9]+ width=[0-9]+ height=[0-9]+ bytes=[0-9]+)?$")
            self.assertIn(b"Protocol LAN image header: status=0", result.stderr)
            self.assertIn(b"Protocol Acquisition unlock: status=0", result.stderr)
            for path, expected in zip(paths, (40, 200)):
                self.assertLess(abs(path.read_bytes()[-1] - expected), 3)
        self.assertEqual(scanner.sessions, 9)
        self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))
        with Scanner() as scanner:
            result = self.cli("scan", scanner.name, "--trace-protocol", "--dry-run")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(b"Protocol ", result.stderr)
            result = self.cli("scan", scanner.name, "--trace-protocol=yes", "--dry-run")
            self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(scanner.sessions, 0)

    def test_first_image_startup_can_exceed_five_seconds(self):
        width = 210 * 400 * 10 // 254
        images = tuple(subprocess.run(
            [str(FIXTURE), "jpeg-gray", str(value), str(width), "3"],
            capture_output=True, check=True).stdout for value in (40, 200))
        with Scanner(images, image_width=width, first_image_delay=6) as scanner:
            with tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / f"{side}.ppm" for side in ("front", "back")]
                result = self.cli("scan", scanner.name, "--resolution", "400",
                                  "--trace-protocol", *paths)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(b"Protocol LAN image prefix: status=0", result.stderr)
                for path, expected in zip(paths, (40, 200)):
                    data = path.read_bytes()
                    self.assertTrue(data.startswith(f"P6\n{width} 3\n255\n".encode()))
                    self.assertLess(abs(data[-1] - expected), 3)
        self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
        self.assertEqual(scanner.commands.count((0x28, 4, 0, 0)), 4)
        self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

    def test_unverified_lan_geometry_refused_before_device_io(self):
        for arguments in (("--left", "1"), ("--resolution", "50", "--left", "1"),
                          ("--page-width", "216"), ("--scan-height", "200")):
            with Scanner() as scanner, tempfile.TemporaryDirectory() as directory:
                result = self.cli("scan", scanner.name, *arguments,
                                  Path(directory) / "front.ppm", Path(directory) / "back.ppm")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"Unsupported operation", result.stderr)
            self.assertEqual(scanner.sessions, 0)

    def test_start_image_prefix_retains_connection_and_payload(self):
        for back_first in (False, True):
            with Scanner(self.images, start_image=True,
                         back_first=back_first) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
                result = self.cli("scan", scanner.name, *paths)
                self.assertEqual(result.returncode, 0, result.stderr)
                for path, expected in zip(paths, (40, 200)):
                    self.assertLess(abs(path.read_bytes()[-1] - expected), 3)
            self.assertEqual(scanner.sessions, 8)
            self.assertEqual(scanner.commands.count((0x28, 4, 0, 0)), 3)
            self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)

    def test_start_image_geometry_conflict_is_not_success(self):
        with Scanner(self.images, start_image=True,
                     fault="start_width") as scanner, tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
            result = self.cli("scan", scanner.name, *paths)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertTrue(all(path.stat().st_size == 0 for path in paths))
        self.assertEqual(scanner.commands[-2:],
                         [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])

    def test_truncated_start_header_or_payload_is_not_success(self):
        for fault in ("start_header", "start_payload"):
            with Scanner(self.images, start_image=True,
                         fault=fault) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
                result = self.cli("scan", scanner.name, *paths)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"I/O or protocol error", result.stderr)
                self.assertTrue(all(path.stat().st_size == 0 for path in paths))
            self.assertEqual(scanner.commands[-2:],
                             [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])
            self.assertNotIn((0x28, 4, 0, 0), scanner.commands)

    def test_width_only_headers_and_sheet_boundary_records(self):
        library = self.sane_library()
        for back_first in (False, True):
            with Scanner(self.images, sheets=2, back_first=back_first,
                         provisional_height=True, boundary_record=True) as scanner:
                handle = ctypes.c_void_p()
                self.assertEqual(library.sane_sp1130n_open(
                    scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                try:
                    for expected in (40, 200) * 2:
                        self.assertEqual(library.sane_sp1130n_start(handle), 0)
                        self.sane_read_page(library, handle, expected)
                    self.assertEqual(library.sane_sp1130n_start(handle), 7)
                finally:
                    library.sane_sp1130n_close(handle)
            self.assertEqual(scanner.sessions, 9)
            self.assertEqual(scanner.commands.count((0x28, 4, 0, 0)), 9)
            self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)

    def test_face_down_side_tags_preserve_two_distinct_duplex_pairs(self):
        library = self.sane_library()
        second = tuple(
            subprocess.run([str(FIXTURE), "jpeg-gray", str(value)],
                           capture_output=True, check=True).stdout
            for value in (80, 160))
        for back_first in (False, True):
            with self.subTest(back_first=back_first):
                with Scanner(sheets=2, sheet_images=(self.images, second),
                             back_first=back_first, provisional_height=True,
                             boundary_record=True, last_status=0x1005,
                             end_boundary="close") as scanner:
                    handle = ctypes.c_void_p()
                    self.assertEqual(library.sane_sp1130n_open(
                        scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                    try:
                        for expected in (40, 200, 80, 160):
                            self.assertEqual(library.sane_sp1130n_start(handle), 0)
                            self.sane_read_page(library, handle, expected)
                        self.assertEqual(library.sane_sp1130n_start(handle), 7)
                        before = len(scanner.commands)
                        self.assertEqual(library.sane_sp1130n_start(handle), 7)
                        self.assertEqual(len(scanner.commands), before)
                    finally:
                        library.sane_sp1130n_close(handle)
                # Byte1 arrived first in the physical face-down capture.
                tags = (0, 0, 1, 1) if back_first else (1, 1, 0, 0)
                self.assertEqual(scanner.image_sides, list(tags) * 2)
                self.assertEqual(len(scanner.params), 1)
                self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
                self.assertNotIn((0x14, 6, 0, 0), scanner.commands)
                self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

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
                result = self.cli("scan", scanner.name, *paths)
                self.assertEqual(result.returncode, 0, result.stderr)
                for path, expected in zip(paths, (40, 200)):
                    self.assertLess(abs(path.read_bytes()[-1] - expected), 3)

    def test_bad_headers_and_incomplete_duplex_fail(self):
        for fault, message in (("missing_back", b"I/O or protocol error"),
                               ("bad_short", b"I/O or protocol error"),
                               ("oversize", b"Out of memory")):
            with Scanner(self.images, fault=fault) as scanner, tempfile.TemporaryDirectory() as directory:
                paths = [Path(directory) / "front.ppm", Path(directory) / "back.ppm"]
                result = self.cli("scan", scanner.name, *paths)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertTrue(all(path.stat().st_size == 0 for path in paths))
            self.assertEqual(scanner.commands[-2:], [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])

    def test_simplex_still_validates_discarded_side(self):
        for source, fault in (("front", "bad_back"), ("back", "bad_front")):
            with Scanner(self.images, fault=fault) as scanner, tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "side.ppm"
                result = self.cli("scan", scanner.name, "--source", source, path)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"I/O or protocol error", result.stderr)
                self.assertEqual(path.stat().st_size, 0)

    def test_read_only_maintenance_over_lan(self):
        with Scanner() as scanner, tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "snapshot.bin"
            result = self.cli("maintenance", scanner.name, path)
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

    def test_no_connection_for_dry_run_or_missing_outputs(self):
        with Scanner() as scanner:
            result = self.cli("scan", scanner.name, "--dry-run")
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.cli("scan", scanner.name)
            self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(scanner.sessions, 0)

    def test_dry_run_rejects_unverified_lan_profiles_without_connecting(self):
        with Scanner() as scanner:
            for arguments in (("--left", "1"), ("--resolution", "50", "--left", "1"),
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

    def test_resolution_trials_and_unsupported_1200(self):
        for device in ("sp1130n:usb:001:002", "sp1130n:tcp:192.0.2.1"):
            for dpi in range(50, 601):
                with self.subTest(device=device, dpi=dpi):
                    result = self.cli("scan", device, "--dry-run",
                                      "--resolution", str(dpi))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(json.loads(result.stdout)["resolution"], dpi)
        for dpi in ("0", "49", "601", "1200", "-1", "300.5", "4294967295"):
            with self.subTest(dpi=dpi):
                result = self.cli("scan", "sp1130n:usb:001:002", "--dry-run",
                                  "--resolution", dpi)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse(result.stdout)
        with Scanner() as scanner:
            for dpi in (50, 51, 200, 299, 301, 599, 600):
                with self.subTest(lan_dpi=dpi):
                    result = self.cli("scan", scanner.name, "--dry-run",
                                      "--resolution", str(dpi))
                    self.assertEqual(result.returncode, 0, result.stderr)
            result = self.cli("scan", scanner.name, "--dry-run",
                              "--resolution", "1200")
            self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(scanner.sessions, 0)

    def test_lan_resolution_trials_preserve_requested_dpi(self):
        for dpi in (50, 51, 75, 100, 150, 200, 299, 301, 450, 599, 600):
            width = 210 * dpi * 10 // 254
            images = tuple(subprocess.run(
                [str(FIXTURE), "jpeg-gray", str(value), str(width), "3"],
                capture_output=True, check=True).stdout for value in (40, 200))
            with self.subTest(dpi=dpi):
                with Scanner(images, image_width=width) as scanner:
                    with tempfile.TemporaryDirectory() as directory:
                        paths = [Path(directory) / side for side in ("front.ppm", "back.ppm")]
                        result = self.cli("scan", scanner.name, "--resolution", dpi, *paths)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        for path in paths:
                            data = path.read_bytes()
                            self.assertTrue(data.startswith(f"P6\n{width} 3\n255\n".encode()))
                            self.assertEqual(len(data.split(b"\n", 3)[3]), width * 3 * 3)
                    self.assertEqual(len(scanner.params), 1)
                    self.assertEqual(struct.unpack_from("<HH", scanner.params[0], 16),
                                     (dpi, dpi))
                    self.assertEqual(u32(scanner.params[0], 8), width)
                    self.assertEqual(u32(scanner.params[0], 32),
                                     (297 * dpi * 10 + 253) // 254)
                    self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

    def test_lan_trial_rejects_300_dpi_or_oversized_images(self):
        for width, height in ((2480, 3), (413, 3508)):
            images = tuple(subprocess.run(
                [str(FIXTURE), "jpeg-gray", str(value), str(width), str(height)],
                capture_output=True, check=True).stdout for value in (40, 200))
            with self.subTest(width=width, height=height):
                with Scanner(images, image_width=width, image_height=height) as scanner:
                    with tempfile.TemporaryDirectory() as directory:
                        paths = [Path(directory) / side for side in ("front.ppm", "back.ppm")]
                        result = self.cli("scan", scanner.name, "--resolution", "50", *paths)
                        self.assertEqual(result.returncode, 1, result.stderr)
                        self.assertIn(b"Resolution mismatch at 50 dpi", result.stderr)
                        self.assertTrue(all(path.stat().st_size == 0 for path in paths))
                    self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

    @staticmethod
    def sane_library():
        if os.environ.get("SP1130N_NETWORK_SHARED") == "no":
            raise unittest.SkipTest("ctypes backend tests require a shared build")
        library = ctypes.CDLL(os.environ.get(
            "SP1130N_NETWORK_LIBRARY", str(BUILD / "libsane-sp1130n.so.1")))
        library.sane_sp1130n_init.argtypes = [
            ctypes.POINTER(ctypes.c_int), ctypes.c_void_p]
        if library.sane_sp1130n_init(None, None) != 0:
            raise AssertionError("backend initialization failed")
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

    def test_sane_first_sheet_drains_before_returning_selected_sides(self):
        library = self.sane_library()
        second = tuple(
            subprocess.run([str(FIXTURE), "jpeg-gray", str(value)],
                           capture_output=True, check=True).stdout
            for value in (80, 160))
        for last_status in (0x1005, 0x1006):
            for back_first in (False, True):
                for source, sides in (("ADF Duplex", (40, 200)),
                                      ("ADF Front", (40,)), ("ADF Back", (200,))):
                    with self.subTest(last_status=last_status, back_first=back_first,
                                      source=source):
                        with Scanner(sheets=3, sheet_images=(self.images, second, second),
                                     last_status=last_status, back_first=back_first,
                                     end_boundary="direct_close") as scanner:
                            handle = ctypes.c_void_p()
                            self.assertEqual(library.sane_sp1130n_open(
                                scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                            try:
                                batch = ctypes.c_int(0)
                                self.assertEqual(library.sane_sp1130n_control_option(
                                    handle, 20, 1, ctypes.byref(batch), None), 0)
                                self.assertEqual(library.sane_sp1130n_control_option(
                                    handle, 2, 1, ctypes.create_string_buffer(
                                        source.encode()), None), 0)
                                self.assertEqual(library.sane_sp1130n_start(handle), 0)
                                scanner.wait_closed(9 + (last_status == 0x1005))
                                self.assertEqual(scanner.block, 12)
                                before = len(scanner.commands)
                                for index, expected in enumerate(sides):
                                    if index:
                                        self.assertEqual(
                                            library.sane_sp1130n_start(handle), 0)
                                    self.sane_read_page(library, handle, expected)
                                self.assertEqual(library.sane_sp1130n_start(handle), 7)
                                self.assertEqual(library.sane_sp1130n_start(handle), 7)
                                self.assertEqual(len(scanner.commands), before)
                            finally:
                                library.sane_sp1130n_close(handle)
                        self.assertEqual(len(scanner.params), 1)
                        self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
                        self.assertNotIn((0x14, 6, 0, 0), scanner.commands)
                        self.assertEqual(scanner.commands[-1], (0x2a, 0x78, 2, 40))

    def test_sane_first_sheet_drain_fault_exposes_no_image(self):
        library = self.sane_library()
        for fault, last_status, expected in (("jam", 0x1006, 6),
                                             ("next_payload", 0x1006, 9),
                                             ("unlock", 0x1005, 9),
                                             ("unlock", 0x1006, 9)):
            with self.subTest(fault=fault, last_status=last_status):
                with Scanner(self.images, sheets=2, fault=fault,
                             last_status=last_status) as scanner:
                    handle = ctypes.c_void_p()
                    self.assertEqual(library.sane_sp1130n_open(
                        scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                    try:
                        batch = ctypes.c_int(0)
                        self.assertEqual(library.sane_sp1130n_control_option(
                            handle, 20, 1, ctypes.byref(batch), None), 0)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        before = len(scanner.commands)
                        self.assertEqual(library.sane_sp1130n_start(handle), expected)
                        self.assertEqual(len(scanner.commands), before)
                        data = ctypes.create_string_buffer(b"\xa5", 1)
                        count = ctypes.c_int(-1)
                        self.assertEqual(library.sane_sp1130n_read(
                            handle, data, 1, ctypes.byref(count)), 4)
                        self.assertEqual(count.value, 0)
                        self.assertEqual(data.raw, b"\xa5")
                    finally:
                        library.sane_sp1130n_close(handle)

    def test_sane_cancel_during_first_sheet_drain(self):
        library = self.sane_library()
        with Scanner(self.images, sheets=2, fault="wait_next") as scanner:
            handle = ctypes.c_void_p()
            self.assertEqual(library.sane_sp1130n_open(
                scanner.name[8:].encode(), ctypes.byref(handle)), 0)
            batch = ctypes.c_int(0)
            self.assertEqual(library.sane_sp1130n_control_option(
                handle, 20, 1, ctypes.byref(batch), None), 0)
            result = []
            worker = threading.Thread(target=lambda: result.append(
                library.sane_sp1130n_start(handle)))
            try:
                worker.start()
                self.assertTrue(scanner.next_sheet_ready.wait(2))
                library.sane_sp1130n_cancel(handle)
                worker.join(3)
                self.assertFalse(worker.is_alive())
                self.assertEqual(result, [2])
                data = ctypes.create_string_buffer(b"\xa5", 1)
                count = ctypes.c_int(-1)
                self.assertEqual(library.sane_sp1130n_read(
                    handle, data, 1, ctypes.byref(count)), 2)
                self.assertEqual(count.value, 0)
                self.assertEqual(data.raw, b"\xa5")
                scanner.wait_closed(10)
                self.assertEqual(scanner.commands[-2:],
                                 [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])
            finally:
                library.sane_sp1130n_close(handle)
                worker.join(3)

    def test_sane_real_duplex_tcp_and_eof(self):
        library = self.sane_library()
        for last_status, start_image in ((0x1005, False), (0x1006, False),
                                         (0x1005, True), (0x1006, True)):
            with Scanner(self.images, sheets=3, last_status=last_status,
                         back_first=True, start_image=start_image) as scanner:
                handle = ctypes.c_void_p()
                self.assertEqual(library.sane_sp1130n_open(scanner.name[8:].encode(),
                                                        ctypes.byref(handle)), 0)
                try:
                    loading = ctypes.c_int(-1)
                    self.assertEqual(library.sane_sp1130n_control_option(
                        handle, 19, 0, ctypes.byref(loading), None), 0)
                    self.assertEqual(loading.value, 0)
                    loading.value = 1
                    self.assertEqual(library.sane_sp1130n_control_option(
                        handle, 19, 1, ctypes.byref(loading), None), 4)
                    for expected in (40, 200) * 3:
                        self.assertEqual(library.sane_sp1130n_start(handle), 0)
                        self.sane_read_page(library, handle, expected)
                    self.assertEqual(library.sane_sp1130n_start(handle), 7)
                    before = len(scanner.commands)
                    self.assertEqual(library.sane_sp1130n_start(handle), 7)
                    self.assertEqual(len(scanner.commands), before)
                    scanner.wait_closed((8 if start_image else 9) +
                                        (last_status == 0x1005))
                finally:
                    library.sane_sp1130n_close(handle)
            self.assertEqual(scanner.sessions, (8 if start_image else 9) +
                             (last_status == 0x1005))
            self.assertEqual(len(scanner.params), 1)
            self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
            self.assertNotIn((0x14, 6, 0, 0), scanner.commands)

    def test_sane_partial_next_sheet_is_not_end_of_batch(self):
        library = self.sane_library()
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

    def test_initial_device_error_is_latched_without_retry(self):
        library = self.sane_library()
        with Scanner(fault="initial_device_error") as scanner:
            handle = ctypes.c_void_p()
            self.assertEqual(library.sane_sp1130n_open(
                scanner.name[8:].encode(), ctypes.byref(handle)), 0)
            try:
                self.assertEqual(library.sane_sp1130n_start(handle), 9)
                before = len(scanner.commands)
                self.assertEqual(library.sane_sp1130n_start(handle), 9)
                self.assertEqual(len(scanner.commands), before)
                data = ctypes.create_string_buffer(1)
                count = ctypes.c_int(-1)
                self.assertEqual(library.sane_sp1130n_read(
                    handle, data, 1, ctypes.byref(count)), 4)
                self.assertEqual(count.value, 0)
            finally:
                library.sane_sp1130n_close(handle)
        self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
        self.assertEqual(scanner.commands.count((0x28, 4, 0, 0)), 1)
        self.assertEqual(scanner.commands[-2:],
                         [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])

    def test_sane_cancel_releases_live_batch_and_allows_new_job(self):
        library = self.sane_library()
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
                # An idle GUI makes no further backend call after cancel.
                scanner.wait_closed(10)
                self.assertEqual(scanner.commands[-2:],
                                 [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])
                self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
                self.assertEqual(scanner.commands.count((0x28, 4, 0, 0)), 4)
                byte = ctypes.create_string_buffer(1)
                count = ctypes.c_int(-1)
                self.assertEqual(library.sane_sp1130n_read(
                    handle, byte, 1, ctypes.byref(count)), 2)
                self.assertEqual(count.value, 0)
                self.assertEqual(library.sane_sp1130n_start(handle), 0)
                self.sane_read_page(library, handle, 40)
            finally:
                library.sane_sp1130n_close(handle)
        self.assertEqual(len(scanner.params), 2)
        self.assertEqual(scanner.sessions, 20)

    def test_sane_cancel_during_image_read(self):
        library = self.sane_library()
        for start_image, fault in ((False, "wait_image"),
                                   (True, "wait_start_payload")):
            with Scanner(self.images, fault=fault, start_image=start_image) as scanner:
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
                    scanner.wait_closed(9 if start_image else 10)
                    self.assertEqual(scanner.commands[-2:],
                                     [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])
                finally:
                    library.sane_sp1130n_close(handle)
                    worker.join(3)

    def test_sane_cancel_consumes_only_pending_image_payload(self):
        class PendingPayloadScanner(Scanner):
            def __init__(self, prefix, tail):
                super().__init__((bytes(4096), bytes(4096)),
                                 provisional_height=True)
                self.prefix = prefix
                self.tail = tail
                self.payload_ready = threading.Event()
                self.tail_sent = False

            def send(self, connection, data):
                if len(data) <= 64 or not u32(data, 20) or not u32(data, 36):
                    return super().send(connection, data)
                assert u32(data, 36) == len(data) - 64
                super().send(connection, data[:64 + self.prefix])
                self.payload_ready.set()
                self.stop.wait(0.1)
                if self.tail == "eof":
                    connection.shutdown(socket.SHUT_WR)
                    return
                if self.tail == "timeout":
                    self.stop.wait(1.3)
                    return
                # A cancelled client must keep this framed payload readable,
                # not abandon it and reconnect for STOP while it is pending.
                try:
                    for offset in range(64 + self.prefix, len(data), 17):
                        connection.sendall(data[offset:offset + 17])
                        time.sleep(0.0001)
                    self.tail_sent = True
                except (BrokenPipeError, ConnectionResetError):
                    pass

        library = self.sane_library()
        for prefix, tail in ((0, "complete"), (17, "complete"),
                             (17, "eof"), (17, "timeout")):
            with self.subTest(prefix=prefix, tail=tail), \
                    PendingPayloadScanner(prefix, tail) as scanner:
                handle = ctypes.c_void_p()
                self.assertEqual(library.sane_sp1130n_open(
                    scanner.name[8:].encode(), ctypes.byref(handle)), 0)
                result = []
                worker = threading.Thread(target=lambda: result.append(
                    library.sane_sp1130n_start(handle)))
                try:
                    worker.start()
                    self.assertTrue(scanner.payload_ready.wait(2))
                    time.sleep(0.01)
                    sent = time.monotonic()
                    library.sane_sp1130n_cancel(handle)
                    worker.join(3)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(result, [2])
                    self.assertEqual(scanner.tail_sent, tail == "complete")
                    if tail == "timeout":
                        self.assertGreater(time.monotonic() - sent, 0.8)
                    scanner.wait_closed(10)
                    self.assertEqual(scanner.commands.count((0x14, 4, 0, 0)), 1)
                    self.assertEqual(scanner.commands.count((0x28, 4, 0, 0)), 1)
                    self.assertEqual(scanner.commands[-2:],
                                     [(0x14, 6, 0, 0), (0x2a, 0x78, 2, 40)])
                    buffer = ctypes.create_string_buffer(b"\xa5" * 64)
                    count = ctypes.c_int(-1)
                    self.assertEqual(library.sane_sp1130n_read(
                        handle, buffer, 32, ctypes.byref(count)), 2)
                    self.assertEqual(count.value, 0)
                    self.assertEqual(buffer.raw, b"\xa5" * 64 + b"\0")
                finally:
                    library.sane_sp1130n_close(handle)
                    worker.join(3)

    def test_sane_shared_processing_preserves_no_edge_images(self):
        library = self.sane_library()
        with Scanner(self.images) as scanner:
            handle = ctypes.c_void_p()
            self.assertEqual(library.sane_sp1130n_open(
                scanner.name[8:].encode(), ctypes.byref(handle)), 0)
            try:
                for option in (14, 15, 16):
                    enabled = ctypes.c_int(1)
                    self.assertEqual(library.sane_sp1130n_control_option(
                        handle, option, 1, ctypes.byref(enabled), None), 0)
                for sample in (40, 200):
                    self.assertEqual(library.sane_sp1130n_start(handle), 0)
                    self.sane_read_page(library, handle, sample)
                self.assertEqual(library.sane_sp1130n_start(handle), 7)
            finally:
                library.sane_sp1130n_close(handle)
        self.assertEqual(len(scanner.params), 1)
        self.assertEqual(scanner.params[0][6:8], b"\x18\x18")

    def test_sane_blank_filter_uses_same_feeder_job(self):
        library = self.sane_library()
        with Scanner(self.images, sheets=2) as scanner:
            handle = ctypes.c_void_p()
            self.assertEqual(library.sane_sp1130n_open(scanner.name[8:].encode(),
                                                    ctypes.byref(handle)), 0)
            try:
                for option, value in ((16, 1), (17, 1000)):
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


if __name__ == "__main__":
    unittest.main(verbosity=2)

# Copyright 2026 The Kervan Authors
# SPDX-License-Identifier: Apache-2.0
"""Python client of the ksim remote protocol (docs/remote-protocol.md).

Write a stack by subclassing Stack and hand the class to serve():

    class Beacon(ksim_remote.Stack):
        name, version = "py-beacon", "1"
        def __init__(self, host, config): ...
        def on_rx(self, frame, rssi_dbm, snr_db): ...
        def on_tx_done(self): ...
        def on_timer(self, timer_id): ...

    sys.exit(ksim_remote.serve(Beacon))

For reproducible runs take time from host.now_us and randomness from host.random(); never read
the wall clock or seed your own generator.
"""

import os
import socket
import struct
import sys

PROTOCOL_VERSION = 1
ABI_VERSION = 2
MAX_MESSAGE = 65536

MSG_HELLO, MSG_ERROR = 0x01, 0x02
EV_CREATE, EV_RX, EV_TX_DONE, EV_TIMER, EV_DESTROY = 0x10, 0x11, 0x12, 0x13, 0x14
REQ_RADIO_SEND, REQ_TIMER_SET, REQ_TIMER_CANCEL, REQ_TRACE = 0x20, 0x21, 0x22, 0x23
REQ_RANDOM, REQ_RADIO_GET, REQ_RADIO_SET = 0x24, 0x25, 0x26
MSG_DONE, MSG_RESULT = 0x2F, 0x30

# results (ksim/stack.h)
OK, ERR_BUSY, ERR_INVALID, ERR_UNSUPPORTED = 0, -1, -2, -3

# radio parameters (ksim/stack.h)
RADIO_PHY, RADIO_RX_ENABLED, RADIO_TX_POWER_CDBM = 1, 2, 3
RADIO_FREQ_HZ, RADIO_LORA_SF, RADIO_LORA_BW_HZ = 10, 11, 12
RADIO_NRF24_CHANNEL, RADIO_NRF24_RATE = 20, 21
PHY_LORA, PHY_NRF24 = 1, 2


class ProtocolError(Exception):
    pass


class _Conn:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send(self, msg_type, payload=b""):
        body = bytes([msg_type]) + payload
        self.sock.sendall(struct.pack("<I", len(body)) + body)

    def _recv_exact(self, n):
        data = bytearray()
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ProtocolError("connection to ksim lost")
            data += chunk
        return bytes(data)

    def recv(self):
        (length,) = struct.unpack("<I", self._recv_exact(4))
        if length == 0 or length > MAX_MESSAGE:
            raise ProtocolError("malformed message length %d" % length)
        body = self._recv_exact(length)
        return body[0], _Reader(body[1:])


class _Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def _take(self, fmt):
        size = struct.calcsize(fmt)
        if self.pos + size > len(self.data):
            raise ProtocolError("message too short")
        (value,) = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return value

    def u8(self):
        return self._take("<B")

    def u32(self):
        return self._take("<I")

    def u64(self):
        return self._take("<Q")

    def i32(self):
        return self._take("<i")

    def i64(self):
        return self._take("<q")

    def f64(self):
        return self._take("<d")

    def bytes(self):
        n = self._take("<H")
        if self.pos + n > len(self.data):
            raise ProtocolError("message too short")
        value = self.data[self.pos:self.pos + n]
        self.pos += n
        return value


def _blob(data):
    data = bytes(data)
    return struct.pack("<H", len(data)) + data


def _text(s):
    return _blob(s.encode("utf-8"))


class Host:
    """What ksim offers a stack; mirrors ksim_host_t."""

    def __init__(self, conn, node_id):
        self._conn = conn
        self.node_id = node_id
        self.mtu = 0
        self.now_us = 0  # time of the current event

    def _request(self, msg_type, payload=b""):
        self._conn.send(msg_type, payload)
        msg, r = self._conn.recv()
        if msg != MSG_RESULT:
            raise ProtocolError("expected RESULT, got message type %d" % msg)
        return r.i32(), r.i64()

    def radio_send(self, frame):
        return self._request(REQ_RADIO_SEND, _blob(frame))[0]

    def timer_set(self, timer_id, at_us):
        self._conn.send(REQ_TIMER_SET, struct.pack("<IQ", timer_id, at_us))

    def timer_cancel(self, timer_id):
        self._conn.send(REQ_TIMER_CANCEL, struct.pack("<I", timer_id))

    def random(self):
        return self._request(REQ_RANDOM)[1] & 0xFFFFFFFF

    def trace(self, event, detail=""):
        self._conn.send(REQ_TRACE, _text(event) + _text(detail))

    def radio_get(self, param):
        """Returns (status, value)."""
        return self._request(REQ_RADIO_GET, struct.pack("<I", param))

    def radio_set(self, param, value):
        return self._request(REQ_RADIO_SET, struct.pack("<Iq", param, value))[0]


class Stack:
    """Base class; override what you need. Raise ValueError in __init__ to reject the config."""

    name = "python"
    version = "1"

    def __init__(self, host, config):
        self.host = host

    def on_rx(self, frame, rssi_dbm, snr_db):
        pass

    def on_tx_done(self):
        pass

    def on_timer(self, timer_id):
        pass

    def destroy(self):
        pass


def parse_config(text):
    """'a=1;b=x' -> {'a': '1', 'b': 'x'}"""
    out = {}
    for item in filter(None, text.split(";")):
        key, sep, value = item.partition("=")
        if not sep:
            raise ValueError("config item without '=': %r" % item)
        out[key] = value
    return out


def serve(stack_cls, port=None, node_id=None):
    """Connect to ksim and run one node until ksim ends the run. Returns a process exit code."""
    port = int(port if port is not None else os.environ["KSIM_PORT"])
    node_id = int(node_id if node_id is not None else os.environ["KSIM_NODE_ID"])
    conn = _Conn(port)
    conn.send(MSG_HELLO, struct.pack("<HHIB", PROTOCOL_VERSION, ABI_VERSION, node_id, 0)
              + _text(stack_cls.name) + _text(stack_cls.version))
    host = Host(conn, node_id)
    stack = None
    while True:
        msg, r = conn.recv()
        if msg == MSG_ERROR:
            sys.stderr.write("ksim refused node %d: %s\n" % (node_id, r.bytes().decode()))
            return 1
        host.now_us = r.u64()
        ok = True
        if msg == EV_CREATE:
            host.node_id, host.mtu = r.u32(), r.u32()
            config = r.bytes().decode()
            try:
                stack = stack_cls(host, parse_config(config))
            except ValueError as e:
                host.trace("py_error", str(e))
                ok = False
        elif msg == EV_RX:
            rssi, snr = r.f64(), r.f64()
            stack.on_rx(r.bytes(), rssi, snr)
        elif msg == EV_TX_DONE:
            stack.on_tx_done()
        elif msg == EV_TIMER:
            stack.on_timer(r.u32())
        elif msg == EV_DESTROY:
            if stack is not None:
                stack.destroy()
            conn.send(MSG_DONE, b"\x01")
            conn.sock.close()
            return 0
        else:
            raise ProtocolError("unexpected message type %d" % msg)
        conn.send(MSG_DONE, b"\x01" if ok else b"\x00")

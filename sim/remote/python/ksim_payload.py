# Copyright 2026 The Kervan Authors
# SPDX-License-Identifier: Apache-2.0
"""Runs the Python payload functions of a ksim scenario (docs/simulator.md, "Payloads").

ksim embeds this file and starts it as `python -c <this file> PORT`. It connects to
127.0.0.1:PORT and answers requests, one at a time, in messages of a 4-byte little-endian length
followed by UTF-8 JSON:

    ksim -> {"seed": 42, "code": {"3": "def payload(node, seq, t): ..."}}
    this <- {"ok": true}                        or {"error": "..."}
    ksim -> {"node": 3, "seq": 0, "t_us": 1520384}
    this <- {"data": "01a2ff"}                  or {"error": "..."}

Each node's code runs in its own namespace with `rng`, a random.Random seeded from the scenario
seed and the node id, so payloads are the same in every run. `struct`, `math` and `random` are
imported for convenience.
"""

import json
import math
import random
import socket
import struct
import sys
import traceback


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def receive(sock):
    head = recv_exact(sock, 4)
    if head is None:
        return None
    body = recv_exact(sock, struct.unpack("<I", head)[0])
    return None if body is None else json.loads(body.decode("utf-8"))


def send(sock, obj):
    body = json.dumps(obj).encode("utf-8")
    sock.sendall(struct.pack("<I", len(body)) + body)


def to_bytes(value):
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value)
    if isinstance(value, str):
        return bytes.fromhex(value)
    if isinstance(value, (list, tuple)):
        return bytes(value)
    raise TypeError("payload() must return bytes, a list of 0..255 or a hex string, not %s"
                    % type(value).__name__)


def last_error():
    """The error and the line of the user's code where it happened."""
    etype, value, tb = sys.exc_info()
    where = ""
    for frame in traceback.extract_tb(tb):
        if frame.filename == "<payload>":
            where = " (line %d)" % frame.lineno
    return "%s: %s%s" % (etype.__name__, value, where)


def setup(msg):
    functions = {}
    for node, code in msg["code"].items():
        scope = {"rng": random.Random(msg["seed"] * 1000003 + int(node)),
                 "struct": struct, "math": math, "random": random}
        try:
            exec(compile(code, "<payload>", "exec"), scope)
        except Exception:
            return None, "node %s: %s" % (node, last_error())
        if not callable(scope.get("payload")):
            return None, "node %s: the code defines no function payload(node, seq, t)" % node
        functions[int(node)] = scope["payload"]
    return functions, None


def main():
    sock = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
    functions, error = setup(receive(sock))
    send(sock, {"error": error} if error else {"ok": True})
    while functions is not None:
        req = receive(sock)
        if req is None:
            break  # ksim closed the connection: the run is over
        node = req["node"]
        try:
            data = to_bytes(functions[node](node, req["seq"], req["t_us"] / 1e6))
            send(sock, {"data": data.hex()})
        except Exception:
            send(sock, {"error": "node %d, message %d: %s" % (node, req["seq"], last_error())})


if __name__ == "__main__":
    main()

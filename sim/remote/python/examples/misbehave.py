# Copyright 2026 The Kervan Authors
# SPDX-License-Identifier: Apache-2.0
"""Test helper: breaks the remote protocol in one specific way (argv[1]) so ksim's error handling
can be tested. Pretends to be stack "kervan"."""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ksim_remote as k  # noqa: E402

what = sys.argv[1]
conn = k._Conn(int(os.environ["KSIM_PORT"]))
node = int(os.environ["KSIM_NODE_ID"])
protocol, abi, mode = k.PROTOCOL_VERSION, k.ABI_VERSION, 0
if what == "bad_protocol":
    protocol = 9
elif what == "bad_abi":
    abi = 1
elif what == "bad_mode":
    mode = 1
elif what == "wrong_node":
    node += 100

if what == "no_hello":
    conn.send(k.MSG_DONE, b"\x01")
else:
    conn.send(k.MSG_HELLO, struct.pack("<HHIB", protocol, abi, node, mode)
              + k._text("kervan") + k._text("1"))

if what in ("unknown_request", "short_request", "bad_length", "disconnect"):
    conn.recv()  # EV_CREATE
    if what == "unknown_request":
        conn.send(0x7E)
    elif what == "short_request":
        conn.send(k.REQ_TIMER_SET)  # fields missing
    elif what == "bad_length":
        conn.sock.sendall(b"\x00\x00\x00\x00")
    conn.sock.close()
    sys.exit(0)

try:
    while conn.sock.recv(4096):
        pass
except OSError:
    pass

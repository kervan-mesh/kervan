# Copyright 2026 The Kervan Authors
# SPDX-License-Identifier: Apache-2.0
"""Test helper: says HELLO as stack "kervan" and then never answers (ksim must time out)."""

import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ksim_remote  # noqa: E402

conn = ksim_remote._Conn(int(os.environ["KSIM_PORT"]))
conn.send(ksim_remote.MSG_HELLO,
          struct.pack("<HHIB", ksim_remote.PROTOCOL_VERSION, ksim_remote.ABI_VERSION,
                      int(os.environ["KSIM_NODE_ID"]), 0)
          + ksim_remote._text("kervan") + ksim_remote._text("1"))
try:
    while True:
        time.sleep(1)
except (KeyboardInterrupt, socket.error):
    pass

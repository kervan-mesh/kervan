# Copyright 2026 The Kervan Authors
# SPDX-License-Identifier: Apache-2.0
"""A tiny LoRa beacon written in Python, run by ksim as a separate process.

Every node broadcasts "B<node><counter>" every `period_ms` (first one at a random offset) and
reports the beacons it hears. Config: period_ms (default 5000), len (default 12).

Scenario node:
    {"id": 1, "pos": [0, 0], "stack": "py-beacon",
     "process": {"spawn": ["python3", "sim/remote/python/examples/beacon.py"]},
     "config": {"period_ms": 2000}}
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ksim_remote  # noqa: E402

TIMER_BEACON = 1


class Beacon(ksim_remote.Stack):
    name = "py-beacon"
    version = "1"

    def __init__(self, host, config):
        super().__init__(host, config)
        unknown = set(config) - {"period_ms", "len"}
        if unknown:
            raise ValueError("unknown config keys: %s" % ", ".join(sorted(unknown)))
        status, phy = host.radio_get(ksim_remote.RADIO_PHY)
        if status != ksim_remote.OK or phy != ksim_remote.PHY_LORA:
            raise ValueError("py-beacon needs a lora radio")
        self.period_us = int(config.get("period_ms", 5000)) * 1000
        self.len = int(config.get("len", 12))
        if self.period_us <= 0 or not 4 <= self.len <= host.mtu:
            raise ValueError("period_ms must be > 0 and len 4..mtu")
        self.counter = 0
        host.timer_set(TIMER_BEACON, host.now_us + host.random() % self.period_us)

    def on_timer(self, timer_id):
        frame = b"B" + bytes([self.host.node_id & 0xFF]) + self.counter.to_bytes(2, "little")
        frame += bytes(self.len - len(frame))
        status = self.host.radio_send(frame)
        self.host.trace("py_tx" if status == ksim_remote.OK else "py_tx_busy",
                        "n=%d" % self.counter)
        self.counter += 1
        self.host.timer_set(TIMER_BEACON, self.host.now_us + self.period_us)

    def on_rx(self, frame, rssi_dbm, snr_db):
        if len(frame) >= 4 and frame[:1] == b"B":
            self.host.trace("py_rx", "from=%d n=%d rssi=%.2f"
                            % (frame[1], int.from_bytes(frame[2:4], "little"), rssi_dbm))


if __name__ == "__main__":
    sys.exit(ksim_remote.serve(Beacon))

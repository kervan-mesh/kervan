/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// ksim's side of the board protocol (docs/hardware.md): what a scenario node becomes on a real
// board, and the request/answer exchanges before a live session starts.

#pragma once

#include <string>

#include "ksim/simulator.hpp"
#include "serial.hpp"

namespace ksim::hw {

constexpr int kProtocol = 1;

struct BoardHello {
    std::string fw;    // firmware name, "kervan-nrf24"
    int version = 0;   // firmware version
    int proto = 0;     // protocol version
    std::string board; // catalog MCU name, "rpi-pico"
    std::string id;    // the board's unique id
    std::string radio;  // "nrf24l01p", "none"
    std::string wiring; // the pins the firmware was built for (FirmwareSpec::wiring), "" if unknown
    std::string json;  // the whole answer
};

// The "config" command that makes a board run node n like the simulation does. Throws
// std::runtime_error (naming the node) when the node cannot run on a board: another stack than
// nrf24-esb, a Python payload, a payload longer than 32 bytes.
std::string config_command(const NodeSpec &n);

// The chip's power step (0..3 = -18, -12, -6, 0 dBm at the chip) for a node's tx_power_dbm: its
// index in the module's four power levels (a PA module amplifies them), else the nearest step.
int pa_level(const NodeSpec &n);

// Sends "hello" and waits for the answer, skipping other lines. Throws std::runtime_error.
BoardHello hello(LineIo &io, int timeout_ms);

// Sends a command line and waits for {"ev":"ok","cmd":cmd}. Throws std::runtime_error with the
// board's error message, or on timeout.
void command(LineIo &io, const std::string &line, const std::string &cmd, int timeout_ms);

} // namespace ksim::hw

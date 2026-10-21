/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Lines of text to and from a real board (docs/hardware.md): a serial port, or anything else
// that behaves like one (tests use boards simulated in-process).

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ksim::hw {

class LineIo {
public:
    virtual ~LineIo() = default;
    // Throws std::runtime_error when the board is gone.
    virtual void write_line(const std::string &line) = 0;
    // One line without its end ("\n" or "\r\n"); nullopt after timeout_ms without one. Throws
    // std::runtime_error when the board is gone. May be called from another thread than
    // write_line, never from two threads at once.
    virtual std::optional<std::string> read_line(int timeout_ms) = 0;
};

// Opens a serial port ("COM5", "/dev/ttyACM0"); throws std::runtime_error with the reason.
std::unique_ptr<LineIo> open_serial(const std::string &port);

// Serial ports that exist now, sorted: COM ports on Windows, /dev/ttyACM*, /dev/ttyUSB* on
// Linux, /dev/cu.usbmodem*, /dev/cu.usbserial* on macOS.
std::vector<std::string> list_serial_ports();

// Splits a byte stream into lines; lines longer than max_len are cut there.
class LineSplitter {
public:
    explicit LineSplitter(size_t max_len = 4096) : max_len_(max_len) {}
    void feed(const char *data, size_t n);
    std::optional<std::string> next();

private:
    size_t max_len_;
    std::string partial_;
    std::vector<std::string> ready_;
    size_t first_ = 0;
};

} // namespace ksim::hw

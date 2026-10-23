/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Board firmware builds for a node's wiring (docs/hardware.md, "Building from the UI"): the pins
// become compile options of firmware/<project>, the build runs in the dev container (Docker),
// and the .uf2 can be copied onto a Pico in BOOTSEL mode.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ksim/device.hpp"

namespace ksim::hw {

// What a build makes: one firmware for one MCU and one wiring.
struct FirmwareSpec {
    std::string project; // firmware/<project>, "pico_nrf24"
    std::string board;   // SDK board, "pico"
    std::string mcu;
    std::string module;
    int spi = 0;                                      // controller index
    int sck = 0, mosi = 0, miso = 0, csn = 0, ce = 0; // GPIO numbers

    // File name without extension, e.g. "kervan_nrf24-rpi-pico-spi0-sck18-mosi19-miso16-csn17-ce20"
    std::string name() const;
    // As the firmware reports it in "hello": "SPI0 SCK=GP18 MOSI=GP19 MISO=GP16 CSN=GP17 CE=GP20"
    std::string wiring() const;
    // -DPICO_BOARD=... -DKFW_SPI=... -DKFW_PIN_SCK=...
    std::vector<std::string> cmake_args() const;
};

// Throws std::runtime_error when there is no firmware for the MCU/module, or the pins are wrong.
FirmwareSpec firmware_spec(const DeviceRegistry &devices, const std::string &mcu,
                           const std::string &module, const PinChoice &pins);

// The command that builds `spec` in the dev container (image `image`) and copies the .uf2 to
// <source_dir>/build/uf2/<name>.uf2.
std::vector<std::string> docker_build_command(const FirmwareSpec &spec,
                                              const std::string &source_dir,
                                              const std::string &image = "kervan-dev");

// Runs one build at a time on its own thread and keeps its output.
class FirmwareBuilder {
public:
    struct Options {
        std::string source_dir; // the repository (firmware/, build/uf2/)
        // The command for a spec; empty: docker_build_command. Tests run something small.
        std::function<std::vector<std::string>(const FirmwareSpec &)> command;
        std::string log_file; // where the command's output goes; empty: a temporary file
    };

    struct Status {
        std::string state = "idle";   // "idle", "running", "done", "failed"
        std::string name;             // FirmwareSpec::name()
        std::string wiring;           // FirmwareSpec::wiring()
        std::string file;             // the .uf2 when done
        std::string error;            // when failed
        std::vector<std::string> log; // the last lines of output
        double seconds = 0;
    };

    explicit FirmwareBuilder(Options opt);
    ~FirmwareBuilder(); // stops a running build
    FirmwareBuilder(const FirmwareBuilder &) = delete;
    FirmwareBuilder &operator=(const FirmwareBuilder &) = delete;

    // false if a build is running already.
    bool start(const FirmwareSpec &spec);
    Status status() const;
    std::string out_dir() const; // <source_dir>/build/uf2

private:
    void run(FirmwareSpec spec, std::vector<std::string> argv);

    Options opt_;
    mutable std::mutex m_;
    Status status_;
    bool stop_ = false;
    std::thread thread_;
};

// Mounted Raspberry Pi boot drives (RPI-RP2, a Pico held in BOOTSEL), e.g. "E:\" or
// "/media/me/RPI-RP2".
std::vector<std::string> uf2_drives();
// Copies a .uf2 onto such a drive; the board then restarts with it. Throws std::runtime_error.
void flash_uf2(const std::string &file, const std::string &drive);
// Opens the system's file manager on a file (selected) or a directory.
void reveal(const std::string &path);

} // namespace ksim::hw

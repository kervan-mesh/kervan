/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Board firmware builds for a node's wiring (docs/hardware.md, "Wiring and firmware"): the pins
// become compile options (Pico SDK) or a devicetree overlay (Zephyr), the build runs in the dev
// container (Docker), and the result is copied onto the board's USB drive or written with
// esptool.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ksim/device.hpp"

namespace ksim::hw {

// What a build makes: one firmware for one MCU board and one wiring.
struct FirmwareSpec {
    FirmwareTarget target; // project, SDK board, pinctrl, output, flash method
    std::string mcu;
    std::string module;
    std::string spi;                      // controller, e.g. "SPI0", "SPIM"
    std::string sck, mosi, miso, csn, ce; // MCU pin names: "GP18", "PA5", "P1.15", "GPIO18"

    // File name without extension: "kervan_nrf24-rpi-pico-spi0-sck18-mosi19-miso16-csn17-ce20"
    std::string name() const;
    // As the firmware reports it in "hello": "SPI0 SCK=GP18 MOSI=GP19 MISO=GP16 CSN=GP17 CE=GP20"
    std::string wiring() const;
    // Pico SDK: -DPICO_BOARD=... -DKFW_SPI=... -DKFW_PIN_SCK=...
    std::vector<std::string> cmake_args() const;
    // Zephyr: the devicetree overlay with the radio node (empty for the Pico SDK).
    std::string overlay() const;
    // "uf2", "hex" or "bin"
    std::string extension() const { return target.output; }
};

// Throws std::runtime_error when there is no firmware for the MCU/module, or the pins are wrong.
FirmwareSpec firmware_spec(const DeviceRegistry &devices, const std::string &mcu,
                           const std::string &module, const PinChoice &pins);

// The command that builds `spec` in the dev container (image `image`) and copies the result to
// <source_dir>/build/fw/<name>.<extension>; a Zephyr build reads <name>.overlay from there.
std::vector<std::string> docker_build_command(const FirmwareSpec &spec,
                                              const std::string &source_dir,
                                              const std::string &image = "kervan-dev");

// The command that writes an ESP32 firmware over a serial port (esptool on this computer).
std::vector<std::string> esptool_command(const FirmwareTarget &target, const std::string &port,
                                         const std::string &file);

// Runs one job (a build, or flashing with esptool) at a time on its own thread and keeps its
// output.
class FirmwareBuilder {
public:
    struct Options {
        std::string source_dir; // the repository (firmware/, build/fw/)
        // The build command for a spec; empty: docker_build_command. Tests run something small.
        std::function<std::vector<std::string>(const FirmwareSpec &)> command;
        // The esptool command; empty: esptool_command.
        std::function<std::vector<std::string>(const FirmwareTarget &, const std::string &port,
                                               const std::string &file)>
            esptool;
        std::string log_file; // where a job's output goes; empty: a temporary file
    };

    struct Status {
        std::string state = "idle";   // "idle", "running", "done", "failed" (of the last job)
        std::string job;              // "build" or "flash"
        std::string name;             // FirmwareSpec::name()
        std::string wiring;           // FirmwareSpec::wiring()
        std::string file;             // the firmware file once built
        std::string flash;            // how it goes onto the board: "drive" or "esptool"
        std::string drive_kind;       // for "drive": "rp2", "stlink" or "jlink"
        std::string error;            // when failed
        std::vector<std::string> log; // the last lines of output
        double seconds = 0;
    };

    explicit FirmwareBuilder(Options opt);
    ~FirmwareBuilder(); // stops a running job
    FirmwareBuilder(const FirmwareBuilder &) = delete;
    FirmwareBuilder &operator=(const FirmwareBuilder &) = delete;

    // false if a job is running already.
    bool start(const FirmwareSpec &spec);
    // Writes the last build with esptool over `port`; false if a job runs or nothing was built,
    // or the build is not for esptool.
    bool flash_esptool(const std::string &port);
    Status status() const;
    std::string out_dir() const; // <source_dir>/build/fw

private:
    bool begin(const std::string &job, std::vector<std::string> argv,
               std::function<std::string(int code)> finish);
    void run(std::vector<std::string> argv, std::function<std::string(int code)> finish);

    Options opt_;
    mutable std::mutex m_;
    Status status_;
    FirmwareTarget built_; // target of the last finished build
    bool stop_ = false;
    std::thread thread_;
};

// A board's USB drive for copying firmware: a Pico in BOOTSEL mode (RPI-RP2), an ST-LINK
// (NODE_...), a J-Link on a Nordic DK (JLINK).
struct BootDrive {
    std::string path;  // "E:\" or "/media/me/RPI-RP2"
    std::string kind;  // "rp2" (takes .uf2), "stlink" (.bin), "jlink" (.hex)
    std::string label; // the volume name
};

std::vector<BootDrive> boot_drives();
// Which drive kind takes a firmware file of this extension ("" if none).
std::string drive_kind_for(const std::string &extension);
// Copies a firmware file onto a boot drive; the board then starts it. Throws std::runtime_error.
void flash_to_drive(const std::string &file, const BootDrive &drive);
// Opens the system's file manager on a file (selected) or a directory.
void reveal(const std::string &path);

} // namespace ksim::hw

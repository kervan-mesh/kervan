/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ksim/phy.hpp"

namespace ksim {

// Hardware is data (sim/hardware/{mcus,modules,boards}/*.json, format in docs/simulator.md):
// a node runs on a radio module, optionally wired to an MCU; a board is a named MCU + module.

struct Range {
    double min = 0.0;
    double max = 0.0;
};

struct McuProfile {
    std::string name;
    int version = 1;
    std::string title;
    std::string description;
    std::string chip;
    double logic_v = 3.3;
    int ram_kb = 0;
    int flash_kb = 0;
    int clock_mhz = 0;
    int supply_out_ma = 0;                  // what its 3.3 V pin can supply; 0 = unknown
    std::map<std::string, std::string> pins; // 3v3, gnd, spi_sck, spi_mosi, spi_miso, spi_cs
    std::vector<std::string> gpio;           // free GPIOs, in the order wiring uses them
    std::vector<std::string> notes;
};

struct ModulePin {
    std::string name; // as printed on the module, e.g. "CE"
    std::string role; // power, gnd, spi_sck, spi_mosi, spi_miso, spi_cs, gpio_out, gpio_in,
                      // gpio_in_optional
};

struct ModuleProfile {
    std::string name;
    int version = 1;
    std::string title;
    std::string description;
    std::string chip;
    RadioConfig radio; // defaults, including antenna_dbi and lna_db
    std::vector<double> tx_power_levels;
    std::optional<Range> tx_power_range;
    std::optional<Range> freq_hz_range;
    std::optional<Range> supply_v;
    bool io_5v_tolerant = false;
    double tx_ma = 0.0;
    double rx_ma = 0.0;
    std::vector<ModulePin> pins;
    std::vector<std::string> notes;
};

struct BoardProfile {
    std::string name;
    int version = 1;
    std::string title;
    std::string description;
    std::string mcu;
    std::string module;
    bool integrated = false; // the radio is on the board: nothing to wire
};

// What a node runs on, resolved from a board or from an MCU + module.
struct DeviceProfile {
    using Range = ksim::Range;

    std::string name; // board name, or "mcu+module"
    int version = 1;  // board version (1 for a combination; parts carry their own)
    std::string description;
    bool board = false;
    std::string mcu; // MCU name, "" if none was given
    int mcu_version = 0;
    std::string module;
    int module_version = 0;
    RadioConfig radio; // defaults for nodes using this device
    std::vector<double> tx_power_levels;  // discrete steps (e.g. nRF24); empty = use the range
    std::optional<Range> tx_power_range;  // dBm
    std::optional<Range> freq_hz_range;   // LoRa: supported band

    // Empty if this device can run with `r`, otherwise why not.
    std::string check(const RadioConfig &r) const;
};

struct WiringRow {
    std::string module_pin;
    std::string mcu_pin;
    std::string role;
};

// How to connect a module to an MCU, with what to watch out for.
struct Wiring {
    bool integrated = false; // a board with the radio on it
    std::vector<WiringRow> rows;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
};

// Each throws std::runtime_error pointing at the problem.
McuProfile parse_mcu(const std::string &json_text, const std::string &where = "mcu");
ModuleProfile parse_module(const std::string &json_text, const std::string &where = "module");
BoardProfile parse_board(const std::string &json_text, const std::string &where = "board");

class DeviceRegistry {
public:
    // The catalog shipped in sim/hardware (compiled into the binary).
    static DeviceRegistry builtin();

    // Add or replace a part with the same name. Boards must name known parts.
    void add(McuProfile mcu);
    void add(ModuleProfile module);
    void add(BoardProfile board);
    // Load DIR/mcus, DIR/modules and DIR/boards (*.json; each directory optional). Throws on the
    // first invalid file.
    void load_dir(const std::string &dir);

    // A board, or nullptr.
    const DeviceProfile *find(const std::string &board) const;
    // A module, optionally wired to an MCU ("" for none). Throws for unknown parts.
    DeviceProfile combine(const std::string &mcu, const std::string &module) const;
    Wiring wiring(const std::string &mcu, const std::string &module) const;

    std::vector<const DeviceProfile *> all() const; // boards
    const BoardProfile *board_profile(const std::string &name) const;
    const McuProfile *mcu(const std::string &name) const;
    const ModuleProfile *module(const std::string &name) const;
    std::vector<const McuProfile *> mcus() const;
    std::vector<const ModuleProfile *> modules() const;

private:
    void resolve(); // rebuild boards_ from board_profiles_

    std::map<std::string, McuProfile> mcus_;
    std::map<std::string, ModuleProfile> modules_;
    std::map<std::string, BoardProfile> board_profiles_;
    std::map<std::string, DeviceProfile> boards_;
};

} // namespace ksim

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/device.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "json_util.hpp"

namespace ksim {

// Generated at build time from sim/hardware/*/*.json (see sim/CMakeLists.txt).
struct BuiltinPart {
    const char *kind; // "mcu", "module", "board"
    const char *json;
};
extern const BuiltinPart kBuiltinHardware[];
extern const size_t kBuiltinHardwareCount;

using namespace json_util;

namespace {

std::string fmt(double v)
{
    std::ostringstream s;
    s << v;
    return s.str();
}

json parse_json(const std::string &text, const std::string &where)
{
    try {
        return json::parse(text);
    } catch (const json::parse_error &e) {
        fail(where, std::string("invalid JSON: ") + e.what());
    }
}

Range parse_range(const json &j, const std::string &where)
{
    check_object(j, {"min", "max"}, where);
    Range r;
    if (!j.contains("min") || !j.contains("max")) {
        fail(where, "needs \"min\" and \"max\"");
    }
    r.min = get_double(j, "min", 0.0, where);
    r.max = get_double(j, "max", 0.0, where);
    if (r.min > r.max) {
        fail(where, "min must not exceed max");
    }
    return r;
}

std::vector<std::string> strings_of(const json &a, const std::string &where)
{
    std::vector<std::string> out;
    if (!a.is_array()) {
        fail(where, "must be an array of strings");
    }
    for (const json &v : a) {
        if (!v.is_string()) {
            fail(where, "must be an array of strings");
        }
        out.push_back(v.get<std::string>());
    }
    return out;
}

std::vector<std::string> string_list(const json &obj, const char *key, const std::string &where)
{
    return obj.contains(key) ? strings_of(obj.at(key), where + "." + key)
                             : std::vector<std::string>{};
}

// name, version, title and description shared by every kind of part
template <typename T>
void parse_common(const json &j, T &out, const std::string &where, std::string &at)
{
    out.name = get_string(j, "name", "", where);
    if (out.name.empty()) {
        fail(where, "needs a \"name\"");
    }
    at = where + " '" + out.name + "'";
    out.version = get_int(j, "version", 1, at);
    out.title = get_string(j, "title", out.name, at);
    out.description = get_string(j, "description", "", at);
}

bool in_range(const std::optional<Range> &r, double v)
{
    return !r || (v >= r->min && v <= r->max);
}

const std::vector<std::string> kPinRoles = {"power",    "gnd",      "spi_sck", "spi_mosi",
                                            "spi_miso", "spi_cs",   "gpio_out", "gpio_in",
                                            "gpio_in_optional"};

} // namespace

std::string DeviceProfile::check(const RadioConfig &r) const
{
    if (r.type != radio.type) {
        return "device '" + name + "' has a " + phy_for(radio.type).name() + " radio";
    }
    if (!tx_power_levels.empty()) {
        const bool ok = std::any_of(tx_power_levels.begin(), tx_power_levels.end(),
                                    [&](double l) { return std::fabs(l - r.tx_power_dbm) < 1e-9; });
        if (!ok) {
            std::string levels;
            for (double l : tx_power_levels) {
                levels += (levels.empty() ? "" : ", ") + fmt(l);
            }
            return "device '" + name + "' supports tx_power_dbm " + levels;
        }
    }
    if (tx_power_range &&
        (r.tx_power_dbm < tx_power_range->min || r.tx_power_dbm > tx_power_range->max)) {
        return "device '" + name + "' supports tx_power_dbm " + fmt(tx_power_range->min) + ".." +
               fmt(tx_power_range->max);
    }
    if (freq_hz_range && r.type == PhyType::LoRa) {
        const double f = static_cast<double>(r.lora.freq_hz);
        if (f < freq_hz_range->min || f > freq_hz_range->max) {
            return "device '" + name + "' supports freq_hz " + fmt(freq_hz_range->min) + ".." +
                   fmt(freq_hz_range->max);
        }
    }
    return {};
}

McuProfile parse_mcu(const std::string &json_text, const std::string &where)
{
    const json j = parse_json(json_text, where);
    check_object(j,
                 {"name", "version", "title", "description", "chip", "logic_v", "ram_kb",
                  "flash_kb", "clock_mhz", "supply_out_ma", "pins", "gpio", "header", "spi",
                  "firmware", "notes"},
                 where);
    McuProfile m;
    std::string at;
    parse_common(j, m, where, at);
    m.chip = get_string(j, "chip", "", at);
    m.logic_v = get_double(j, "logic_v", m.logic_v, at);
    m.ram_kb = get_int(j, "ram_kb", 0, at);
    m.flash_kb = get_int(j, "flash_kb", 0, at);
    m.clock_mhz = get_int(j, "clock_mhz", 0, at);
    m.supply_out_ma = get_int(j, "supply_out_ma", 0, at);
    if (j.contains("pins")) {
        const json &p = j.at("pins");
        check_object(p, {"3v3", "gnd", "spi_sck", "spi_mosi", "spi_miso", "spi_cs"}, at + ".pins");
        for (const auto &item : p.items()) {
            m.pins[item.key()] = get_string(p, item.key().c_str(), "", at + ".pins");
        }
    }
    m.gpio = string_list(j, "gpio", at);
    if (j.contains("header")) {
        const json &h = j.at("header");
        check_object(h, {"left", "right", "top"}, at + ".header");
        McuHeader header;
        header.left = string_list(h, "left", at + ".header");
        header.right = string_list(h, "right", at + ".header");
        header.top = get_string(h, "top", "", at + ".header");
        if (header.left.empty() || header.left.size() != header.right.size()) {
            fail(at + ".header", "needs \"left\" and \"right\" rows of the same length");
        }
        m.header = header;
    }
    if (j.contains("spi")) {
        if (!j.at("spi").is_array()) {
            fail(at + ".spi", "must be an array of {\"name\", \"sck\", \"mosi\", \"miso\"}");
        }
        for (const json &b : j.at("spi")) {
            check_object(b, {"name", "sck", "mosi", "miso"}, at + ".spi");
            SpiBus bus;
            bus.name = get_string(b, "name", "", at + ".spi");
            bus.sck = string_list(b, "sck", at + ".spi");
            bus.mosi = string_list(b, "mosi", at + ".spi");
            bus.miso = string_list(b, "miso", at + ".spi");
            for (const auto *list : {&bus.sck, &bus.mosi, &bus.miso}) {
                for (const std::string &pin : *list) {
                    if (m.header && m.pin_number(pin) == 0) {
                        fail(at + ".spi", pin + " is not on the header");
                    }
                }
            }
            m.spi.push_back(bus);
        }
    }
    if (j.contains("firmware")) {
        const json &f = j.at("firmware");
        check_object(f, {"project", "board"}, at + ".firmware");
        m.firmware = FirmwareTarget{get_string(f, "project", "", at + ".firmware"),
                                    get_string(f, "board", "", at + ".firmware")};
        if (m.firmware->project.empty() || m.firmware->board.empty()) {
            fail(at + ".firmware", "needs \"project\" and \"board\"");
        }
    }
    m.notes = string_list(j, "notes", at);
    return m;
}

int McuProfile::pin_number(const std::string &pin) const
{
    if (!header) {
        return 0;
    }
    const int n = static_cast<int>(header->left.size());
    for (int i = 0; i < n; i++) {
        if (header->left[static_cast<size_t>(i)] == pin) {
            return i + 1;
        }
    }
    for (int i = 0; i < n; i++) {
        if (header->right[static_cast<size_t>(i)] == pin) {
            return 2 * n - i;
        }
    }
    return 0;
}

ModuleProfile parse_module(const std::string &json_text, const std::string &where)
{
    const json j = parse_json(json_text, where);
    check_object(j,
                 {"name", "version", "title", "description", "chip", "radio", "limits",
                  "antenna_dbi", "lna_db", "supply_v", "io_5v_tolerant", "current_ma", "pins",
                  "header", "notes"},
                 where);
    ModuleProfile m;
    std::string at;
    parse_common(j, m, where, at);
    m.chip = get_string(j, "chip", "", at);
    if (!j.contains("radio") || !j.at("radio").contains("type")) {
        fail(at + ".radio", "needs a \"type\"");
    }
    m.radio = parse_radio(j.at("radio"), RadioConfig{}, at + ".radio");
    m.radio.antenna_dbi = get_double(j, "antenna_dbi", 0.0, at);
    m.radio.lna_db = get_double(j, "lna_db", 0.0, at);

    if (j.contains("limits")) {
        const json &lim = j.at("limits");
        check_object(lim, {"tx_power_dbm", "freq_hz"}, at + ".limits");
        if (lim.contains("tx_power_dbm")) {
            const json &p = lim.at("tx_power_dbm");
            if (p.is_array()) {
                for (const json &v : p) {
                    if (!v.is_number()) {
                        fail(at + ".limits.tx_power_dbm", "levels must be numbers");
                    }
                    m.tx_power_levels.push_back(v.get<double>());
                }
            } else {
                m.tx_power_range = parse_range(p, at + ".limits.tx_power_dbm");
            }
        }
        if (lim.contains("freq_hz")) {
            m.freq_hz_range = parse_range(lim.at("freq_hz"), at + ".limits.freq_hz");
        }
    }
    if (j.contains("supply_v")) {
        m.supply_v = parse_range(j.at("supply_v"), at + ".supply_v");
    }
    m.io_5v_tolerant = get_bool(j, "io_5v_tolerant", false, at);
    if (j.contains("current_ma")) {
        const json &c = j.at("current_ma");
        check_object(c, {"tx", "rx"}, at + ".current_ma");
        m.tx_ma = get_double(c, "tx", 0.0, at + ".current_ma");
        m.rx_ma = get_double(c, "rx", 0.0, at + ".current_ma");
    }
    if (j.contains("pins")) {
        if (!j.at("pins").is_array()) {
            fail(at + ".pins", "must be an array of {\"name\", \"role\"}");
        }
        for (const json &p : j.at("pins")) {
            check_object(p, {"name", "role"}, at + ".pins");
            ModulePin pin{get_string(p, "name", "", at + ".pins"),
                          get_string(p, "role", "", at + ".pins")};
            if (std::find(kPinRoles.begin(), kPinRoles.end(), pin.role) == kPinRoles.end()) {
                fail(at + ".pins", "unknown role '" + pin.role + "'");
            }
            m.pins.push_back(pin);
        }
    }
    if (j.contains("header")) {
        const json &h = j.at("header");
        if (!h.is_array()) {
            fail(at + ".header", "must be rows of pin names");
        }
        for (size_t r = 0; r < h.size(); r++) {
            m.header.push_back(strings_of(h.at(r), at + ".header"));
            for (const std::string &name : m.header.back()) {
                const bool known = std::any_of(m.pins.begin(), m.pins.end(),
                                               [&](const ModulePin &p) { return p.name == name; });
                if (!known) {
                    fail(at + ".header", name + " is not one of the module's pins");
                }
            }
        }
    }
    m.notes = string_list(j, "notes", at);

    DeviceProfile probe; // reuse the limit check on the defaults
    probe.name = m.name;
    probe.radio = m.radio;
    probe.tx_power_levels = m.tx_power_levels;
    probe.tx_power_range = m.tx_power_range;
    probe.freq_hz_range = m.freq_hz_range;
    const std::string err = probe.check(m.radio);
    if (!err.empty()) {
        fail(at + ".radio", "defaults violate the limits: " + err);
    }
    return m;
}

BoardProfile parse_board(const std::string &json_text, const std::string &where)
{
    const json j = parse_json(json_text, where);
    check_object(j, {"name", "version", "title", "description", "mcu", "module", "integrated"},
                 where);
    BoardProfile b;
    std::string at;
    parse_common(j, b, where, at);
    b.mcu = get_string(j, "mcu", "", at);
    b.module = get_string(j, "module", "", at);
    if (b.module.empty()) {
        fail(at, "needs a \"module\"");
    }
    b.integrated = get_bool(j, "integrated", false, at);
    return b;
}

DeviceRegistry DeviceRegistry::builtin()
{
    DeviceRegistry reg;
    // parts before boards, so every board can be resolved when it is added
    for (const char *kind : {"mcu", "module", "board"}) {
        for (size_t i = 0; i < kBuiltinHardwareCount; i++) {
            const BuiltinPart &p = kBuiltinHardware[i];
            if (std::string(p.kind) != kind) {
                continue;
            }
            const std::string where = std::string("built-in ") + kind;
            if (p.kind == std::string("mcu")) {
                reg.add(parse_mcu(p.json, where));
            } else if (p.kind == std::string("module")) {
                reg.add(parse_module(p.json, where));
            } else {
                reg.add(parse_board(p.json, where));
            }
        }
    }
    return reg;
}

void DeviceRegistry::add(McuProfile mcu)
{
    const std::string key = mcu.name;
    mcus_[key] = std::move(mcu);
    resolve();
}

void DeviceRegistry::add(ModuleProfile module)
{
    const std::string key = module.name;
    modules_[key] = std::move(module);
    resolve();
}

void DeviceRegistry::add(BoardProfile board)
{
    const std::string key = board.name;
    auto previous = board_profiles_.find(key);
    std::optional<BoardProfile> old;
    if (previous != board_profiles_.end()) {
        old = previous->second;
    }
    board_profiles_[key] = std::move(board);
    try {
        resolve();
    } catch (...) {
        if (old) {
            board_profiles_[key] = *old;
        } else {
            board_profiles_.erase(key);
        }
        resolve();
        throw;
    }
}

void DeviceRegistry::resolve()
{
    std::map<std::string, DeviceProfile> boards;
    for (const auto &[name, b] : board_profiles_) {
        if (!b.mcu.empty() && mcus_.count(b.mcu) == 0) {
            throw std::runtime_error("board '" + name + "': unknown mcu '" + b.mcu + "'");
        }
        if (modules_.count(b.module) == 0) {
            throw std::runtime_error("board '" + name + "': unknown module '" + b.module + "'");
        }
        DeviceProfile d = combine(b.mcu, b.module);
        d.name = b.name;
        d.version = b.version;
        d.description = b.description;
        d.board = true;
        boards.emplace(name, std::move(d));
    }
    boards_ = std::move(boards);
}

void DeviceRegistry::load_dir(const std::string &dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        throw std::runtime_error("hardware directory not found: " + dir);
    }
    for (const char *kind : {"mcus", "modules", "boards"}) {
        const fs::path sub = fs::path(dir) / kind;
        if (!fs::is_directory(sub, ec)) {
            continue;
        }
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(sub)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end()); // deterministic order
        for (const fs::path &file : files) {
            std::ifstream in(file, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            const std::string where = file.string();
            if (std::string(kind) == "mcus") {
                add(parse_mcu(text.str(), where));
            } else if (std::string(kind) == "modules") {
                add(parse_module(text.str(), where));
            } else {
                try {
                    add(parse_board(text.str(), where));
                } catch (const std::runtime_error &e) {
                    throw std::runtime_error(where + ": " + e.what());
                }
            }
        }
    }
}

const DeviceProfile *DeviceRegistry::find(const std::string &board) const
{
    auto it = boards_.find(board);
    return it == boards_.end() ? nullptr : &it->second;
}

DeviceProfile DeviceRegistry::combine(const std::string &mcu_name,
                                      const std::string &module_name) const
{
    const ModuleProfile *m = module(module_name);
    if (m == nullptr) {
        throw std::runtime_error("unknown module '" + module_name + "' (see `ksim devices`)");
    }
    const McuProfile *u = nullptr;
    if (!mcu_name.empty()) {
        u = mcu(mcu_name);
        if (u == nullptr) {
            throw std::runtime_error("unknown mcu '" + mcu_name + "' (see `ksim devices`)");
        }
    }
    DeviceProfile d;
    d.name = u != nullptr ? u->name + "+" + m->name : m->name;
    d.description = u != nullptr ? u->title + " with " + m->title : m->title;
    d.mcu = mcu_name;
    d.mcu_version = u != nullptr ? u->version : 0;
    d.module = m->name;
    d.module_version = m->version;
    d.radio = m->radio;
    d.tx_power_levels = m->tx_power_levels;
    d.tx_power_range = m->tx_power_range;
    d.freq_hz_range = m->freq_hz_range;
    return d;
}

namespace {

std::string first_word(const std::string &s)
{
    return s.substr(0, s.find(' '));
}

// "GP18 / SPI0 SCK (pin 24)" -> 24
int pin_in_text(const std::string &s)
{
    const size_t at = s.find("(pin ");
    return at == std::string::npos ? 0 : std::atoi(s.c_str() + at + 5);
}

bool is_gpio(const std::string &name)
{
    return name.size() > 2 && name.compare(0, 2, "GP") == 0 &&
           name.find_first_not_of("0123456789", 2) == std::string::npos;
}

[[noreturn]] void bad_pins(const std::string &msg)
{
    throw std::runtime_error("pins: " + msg);
}

std::string join(const std::vector<std::string> &items, const char *sep)
{
    std::string out;
    for (const std::string &i : items) {
        out += (out.empty() ? "" : sep) + i;
    }
    return out;
}

} // namespace

Wiring DeviceRegistry::wiring(const std::string &mcu_name, const std::string &module_name,
                              const PinChoice &choice) const
{
    combine(mcu_name, module_name); // throws for unknown parts
    const ModuleProfile &m = *module(module_name);
    const McuProfile *u = mcu_name.empty() ? nullptr : mcu(mcu_name);

    Wiring w;
    w.notes = m.notes;
    if (u != nullptr) {
        w.notes.insert(w.notes.end(), u->notes.begin(), u->notes.end());
    }

    // chosen pins: signal pins of the module, onto GPIOs of an MCU whose header is known
    for (const auto &[pin, to] : choice) {
        const auto it = std::find_if(m.pins.begin(), m.pins.end(),
                                     [&](const ModulePin &p) { return p.name == pin; });
        if (it == m.pins.end()) {
            bad_pins(m.title + " has no pin '" + pin + "'");
        }
        if (u == nullptr || !u->header) {
            bad_pins("choosing pins needs an MCU whose pin header is in the catalog");
        }
        if (it->role == "power" || it->role == "gnd") {
            bad_pins(pin + " always goes to " + (it->role == "power" ? "3.3 V" : "GND"));
        }
        if (!is_gpio(to) || u->pin_number(to) == 0) {
            bad_pins(to + " is not a GPIO on the header of " + u->title);
        }
    }
    const auto chosen_elsewhere = [&](const std::string &gpio) {
        return std::any_of(choice.begin(), choice.end(),
                           [&](const auto &c) { return c.second == gpio; });
    };

    size_t next_gpio = 0;
    for (const ModulePin &pin : m.pins) {
        WiringRow row{pin.name, "", pin.role, "", 0};
        if (u != nullptr) {
            std::string text; // the catalog's description of a default pin
            if (const auto c = choice.find(pin.name); c != choice.end()) {
                row.mcu_name = c->second;
                if (pin.role != "power" && pin.role != "gnd" && pin.role.rfind("spi_", 0) != 0) {
                    next_gpio++; // its default slot stays taken, so the others keep theirs
                }
            } else if (pin.role == "power" || pin.role == "gnd") {
                const char *key = pin.role == "power" ? "3v3" : "gnd";
                text = u->pins.count(key) != 0 ? u->pins.at(key) : "";
            } else if (pin.role.rfind("spi_", 0) == 0) {
                text = u->pins.count(pin.role) != 0 ? u->pins.at(pin.role) : "";
            } else if (next_gpio < u->gpio.size()) {
                // the default stays where it is unless a chosen pin took it
                text = u->gpio[next_gpio];
                for (size_t k = 0; chosen_elsewhere(first_word(text)) && k < u->gpio.size(); k++) {
                    const std::string spare = first_word(u->gpio[k]);
                    const bool taken = chosen_elsewhere(spare) ||
                                       std::any_of(w.rows.begin(), w.rows.end(),
                                                   [&](const WiringRow &r) {
                                                       return r.mcu_name == spare;
                                                   });
                    if (!taken) {
                        text = u->gpio[k];
                    }
                }
                if (chosen_elsewhere(first_word(text))) {
                    text.clear(); // no free GPIO left in the list
                }
                next_gpio++;
            }
            if (!text.empty()) {
                row.mcu_name = first_word(text);
                row.mcu_number = pin_in_text(text);
                row.mcu_pin = text;
            }
            if (row.mcu_number == 0 && !row.mcu_name.empty()) {
                row.mcu_number = u->pin_number(row.mcu_name);
            }
            if (row.mcu_name.empty() && pin.role != "gpio_in_optional") {
                w.warnings.push_back(u->title + " has no pin listed for " + pin.name + " (" +
                                     pin.role + ")");
            }
        }
        w.rows.push_back(row);
    }

    if (u != nullptr && u->header) {
        // one GPIO per signal, and SCK/MOSI/MISO on one SPI controller
        for (size_t i = 0; i < w.rows.size(); i++) {
            for (size_t k = i + 1; k < w.rows.size(); k++) {
                const WiringRow &a = w.rows[i];
                const WiringRow &b = w.rows[k];
                if (!a.mcu_name.empty() && a.mcu_name == b.mcu_name && is_gpio(a.mcu_name)) {
                    bad_pins(a.mcu_name + " is wired to both " + a.module_pin + " and " +
                             b.module_pin);
                }
            }
        }
        const auto signal = [&](const char *role) -> WiringRow * {
            for (WiringRow &r : w.rows) {
                if (r.role == role) {
                    return &r;
                }
            }
            return nullptr;
        };
        WiringRow *sck = signal("spi_sck");
        WiringRow *mosi = signal("spi_mosi");
        WiringRow *miso = signal("spi_miso");
        if (sck != nullptr && mosi != nullptr && miso != nullptr && !u->spi.empty()) {
            const auto has = [](const std::vector<std::string> &l, const std::string &v) {
                return std::find(l.begin(), l.end(), v) != l.end();
            };
            for (const SpiBus &bus : u->spi) {
                if (has(bus.sck, sck->mcu_name) && has(bus.mosi, mosi->mcu_name) &&
                    has(bus.miso, miso->mcu_name)) {
                    w.spi = bus.name;
                }
            }
            if (w.spi.empty()) {
                std::vector<std::string> options;
                for (const SpiBus &bus : u->spi) {
                    options.push_back(bus.name + ": SCK " + join(bus.sck, "/") + ", MOSI " +
                                      join(bus.mosi, "/") + ", MISO " + join(bus.miso, "/"));
                }
                bad_pins("SCK, MOSI and MISO must be on one SPI controller (" +
                         join(options, "; ") + ")");
            }
        }
        // describe every pin the same way: "GP10 / SPI1 SCK (pin 14)"
        for (WiringRow &r : w.rows) {
            if (r.mcu_name.empty() || r.role == "power" || r.role == "gnd") {
                continue;
            }
            std::string what;
            if (r.role == "spi_sck" || r.role == "spi_mosi" || r.role == "spi_miso") {
                what = " / " + w.spi + " " + r.module_pin;
            }
            r.mcu_pin = r.mcu_name + what + " (pin " + std::to_string(r.mcu_number) + ")";
        }
    }
    if (u == nullptr) {
        return w;
    }

    if (!in_range(m.supply_v, 3.3)) {
        w.warnings.push_back(m.title + " needs " + fmt(m.supply_v->min) + "-" +
                             fmt(m.supply_v->max) + " V, not 3.3 V");
    }
    if (u->logic_v > 3.6 && !m.io_5v_tolerant) {
        w.warnings.push_back(u->title + " uses " + fmt(u->logic_v) + " V logic but " + m.title +
                             " takes 3.3 V: put a level shifter on the signal lines");
    }
    const double peak = std::max(m.tx_ma, m.rx_ma);
    if (u->supply_out_ma > 0 && peak > u->supply_out_ma) {
        w.warnings.push_back(m.title + " draws up to " + fmt(peak) + " mA but the 3.3 V pin of " +
                             u->title + " supplies about " + std::to_string(u->supply_out_ma) +
                             " mA: power the module from its own 3.3 V regulator");
    }
    return w;
}

std::vector<const DeviceProfile *> DeviceRegistry::all() const
{
    std::vector<const DeviceProfile *> out;
    for (const auto &entry : boards_) {
        out.push_back(&entry.second);
    }
    return out;
}

const BoardProfile *DeviceRegistry::board_profile(const std::string &name) const
{
    auto it = board_profiles_.find(name);
    return it == board_profiles_.end() ? nullptr : &it->second;
}

const McuProfile *DeviceRegistry::mcu(const std::string &name) const
{
    auto it = mcus_.find(name);
    return it == mcus_.end() ? nullptr : &it->second;
}

const ModuleProfile *DeviceRegistry::module(const std::string &name) const
{
    auto it = modules_.find(name);
    return it == modules_.end() ? nullptr : &it->second;
}

std::vector<const McuProfile *> DeviceRegistry::mcus() const
{
    std::vector<const McuProfile *> out;
    for (const auto &entry : mcus_) {
        out.push_back(&entry.second);
    }
    return out;
}

std::vector<const ModuleProfile *> DeviceRegistry::modules() const
{
    std::vector<const ModuleProfile *> out;
    for (const auto &entry : modules_) {
        out.push_back(&entry.second);
    }
    return out;
}

} // namespace ksim

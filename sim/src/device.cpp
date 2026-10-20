/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/device.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "json_util.hpp"

namespace ksim {

// Generated at build time from sim/devices/*.json (see sim/CMakeLists.txt).
extern const char *const kBuiltinDevices[];
extern const size_t kBuiltinDeviceCount;

using namespace json_util;

namespace {

std::string fmt(double v)
{
    std::ostringstream s;
    s << v;
    return s.str();
}

DeviceProfile::Range parse_range(const json &j, const std::string &where)
{
    check_object(j, {"min", "max"}, where);
    DeviceProfile::Range r;
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

DeviceProfile parse_device(const std::string &json_text, const std::string &where)
{
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::parse_error &e) {
        fail(where, std::string("invalid JSON: ") + e.what());
    }
    check_object(j, {"name", "version", "description", "mcu", "radio", "limits"}, where);

    DeviceProfile d;
    d.name = get_string(j, "name", "", where);
    if (d.name.empty()) {
        fail(where, "needs a \"name\"");
    }
    const std::string at = where + " '" + d.name + "'";
    d.version = get_int(j, "version", 1, at);
    d.description = get_string(j, "description", "", at);
    d.mcu = get_string(j, "mcu", "", at);
    if (!j.contains("radio") || !j.at("radio").contains("type")) {
        fail(at + ".radio", "needs a \"type\"");
    }
    d.radio = parse_radio(j.at("radio"), RadioConfig{}, at + ".radio");

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
                    d.tx_power_levels.push_back(v.get<double>());
                }
            } else {
                d.tx_power_range = parse_range(p, at + ".limits.tx_power_dbm");
            }
        }
        if (lim.contains("freq_hz")) {
            d.freq_hz_range = parse_range(lim.at("freq_hz"), at + ".limits.freq_hz");
        }
    }

    const std::string err = d.check(d.radio);
    if (!err.empty()) {
        fail(at + ".radio", "defaults violate the limits: " + err);
    }
    return d;
}

DeviceRegistry DeviceRegistry::builtin()
{
    DeviceRegistry reg;
    for (size_t i = 0; i < kBuiltinDeviceCount; i++) {
        reg.add(parse_device(kBuiltinDevices[i], "built-in device"));
    }
    return reg;
}

void DeviceRegistry::add(DeviceProfile profile)
{
    const std::string key = profile.name;
    profiles_[key] = std::move(profile);
}

void DeviceRegistry::load_dir(const std::string &dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        throw std::runtime_error("device directory not found: " + dir);
    }
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end()); // deterministic order
    for (const fs::path &file : files) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        add(parse_device(text.str(), file.string()));
    }
}

const DeviceProfile *DeviceRegistry::find(const std::string &name) const
{
    auto it = profiles_.find(name);
    return it == profiles_.end() ? nullptr : &it->second;
}

std::vector<const DeviceProfile *> DeviceRegistry::all() const
{
    std::vector<const DeviceProfile *> out;
    for (const auto &entry : profiles_) {
        out.push_back(&entry.second);
    }
    return out;
}

} // namespace ksim

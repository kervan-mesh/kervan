/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "json_util.hpp"

#include <limits>
#include <stdexcept>

namespace ksim::json_util {

void fail(const std::string &where, const std::string &msg)
{
    throw std::runtime_error(where + ": " + msg);
}

void check_object(const json &obj, std::initializer_list<const char *> allowed,
                  const std::string &where)
{
    if (!obj.is_object()) {
        fail(where, "must be an object");
    }
    for (const auto &item : obj.items()) {
        bool known = false;
        for (const char *key : allowed) {
            known = known || item.key() == key;
        }
        if (!known) {
            fail(where, "unknown key '" + item.key() + "'");
        }
    }
}

double get_double(const json &obj, const char *key, double def, const std::string &where)
{
    if (!obj.contains(key)) {
        return def;
    }
    const json &v = obj.at(key);
    if (!v.is_number()) {
        fail(where + "." + key, "must be a number");
    }
    return v.get<double>();
}

uint64_t get_u64(const json &obj, const char *key, uint64_t def, uint64_t max,
                 const std::string &where)
{
    if (!obj.contains(key)) {
        return def;
    }
    const json &v = obj.at(key);
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<int64_t>() < 0)) {
        fail(where + "." + key, "must be a non-negative integer");
    }
    const uint64_t value = v.get<uint64_t>();
    if (value > max) {
        fail(where + "." + key, "must be at most " + std::to_string(max));
    }
    return value;
}

int get_int(const json &obj, const char *key, int def, const std::string &where)
{
    return static_cast<int>(get_u64(obj, key, static_cast<uint64_t>(def),
                                    static_cast<uint64_t>(std::numeric_limits<int>::max()), where));
}

bool get_bool(const json &obj, const char *key, bool def, const std::string &where)
{
    if (!obj.contains(key)) {
        return def;
    }
    if (!obj.at(key).is_boolean()) {
        fail(where + "." + key, "must be true or false");
    }
    return obj.at(key).get<bool>();
}

std::string get_string(const json &obj, const char *key, const std::string &def,
                       const std::string &where)
{
    if (!obj.contains(key)) {
        return def;
    }
    if (!obj.at(key).is_string()) {
        fail(where + "." + key, "must be a string");
    }
    return obj.at(key).get<std::string>();
}

RadioConfig parse_radio(const json &j, RadioConfig base, const std::string &where)
{
    static const std::initializer_list<const char *> lora_keys = {
        "freq_hz", "bw_hz", "sf", "cr", "preamble", "crc", "implicit_header"};
    static const std::initializer_list<const char *> nrf24_keys = {"channel", "data_rate",
                                                                   "crc_bytes"};

    check_object(j,
                 {"type", "tx_power_dbm", "freq_hz", "bw_hz", "sf", "cr", "preamble", "crc",
                  "implicit_header", "channel", "data_rate", "crc_bytes"},
                 where);

    RadioConfig r = base;
    if (j.contains("type")) {
        const Phy *phy = j.at("type").is_string() ? phy_by_name(j.at("type").get<std::string>())
                                                  : nullptr;
        if (phy == nullptr) {
            fail(where + ".type", "must be \"lora\" or \"nrf24\"");
        }
        if (phy->type() != base.type) {
            r = RadioConfig{};
            r.type = phy->type();
            r.tx_power_dbm = r.type == PhyType::Nrf24 ? 0.0 : 14.0;
        }
    }

    const auto &foreign = r.type == PhyType::LoRa ? nrf24_keys : lora_keys;
    for (const char *key : foreign) {
        if (j.contains(key)) {
            fail(where + "." + key,
                 std::string("is not a ") + phy_for(r.type).name() + " setting");
        }
    }

    r.tx_power_dbm = get_double(j, "tx_power_dbm", r.tx_power_dbm, where);
    if (r.type == PhyType::LoRa) {
        LoraParams &p = r.lora;
        p.freq_hz = get_u64(j, "freq_hz", p.freq_hz, std::numeric_limits<uint64_t>::max(), where);
        p.bw_hz = static_cast<uint32_t>(get_u64(j, "bw_hz", p.bw_hz, 500000, where));
        p.sf = get_int(j, "sf", p.sf, where);
        p.cr = get_int(j, "cr", p.cr, where);
        p.preamble = get_int(j, "preamble", p.preamble, where);
        p.crc = get_bool(j, "crc", p.crc, where);
        p.implicit_header = get_bool(j, "implicit_header", p.implicit_header, where);
    } else {
        Nrf24Params &p = r.nrf24;
        p.channel = get_int(j, "channel", p.channel, where);
        p.data_rate = static_cast<uint32_t>(get_u64(j, "data_rate", p.data_rate, 2000000, where));
        p.crc_bytes = get_int(j, "crc_bytes", p.crc_bytes, where);
    }

    const std::string err = phy_for(r.type).validate(r);
    if (!err.empty()) {
        fail(where, err);
    }
    return r;
}

} // namespace ksim::json_util

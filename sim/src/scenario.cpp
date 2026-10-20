/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/scenario.hpp"

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ksim {

namespace {

using nlohmann::json;

[[noreturn]] void fail(const std::string &where, const std::string &msg)
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

LoraParams parse_radio(const json &j, LoraParams p, const std::string &where)
{
    check_object(j,
                 {"type", "freq_hz", "bw_hz", "sf", "cr", "preamble", "crc", "implicit_header",
                  "tx_power_dbm"},
                 where);
    if (j.contains("type") && j.at("type") != "lora") {
        fail(where + ".type", "only \"lora\" is supported");
    }
    p.freq_hz = get_u64(j, "freq_hz", p.freq_hz, std::numeric_limits<uint64_t>::max(), where);
    p.bw_hz = static_cast<uint32_t>(get_u64(j, "bw_hz", p.bw_hz, 500000, where));
    p.sf = get_int(j, "sf", p.sf, where);
    p.cr = get_int(j, "cr", p.cr, where);
    p.preamble = get_int(j, "preamble", p.preamble, where);
    p.crc = get_bool(j, "crc", p.crc, where);
    p.implicit_header = get_bool(j, "implicit_header", p.implicit_header, where);
    p.tx_power_dbm = get_double(j, "tx_power_dbm", p.tx_power_dbm, where);
    const std::string err = lora_validate(p);
    if (!err.empty()) {
        fail(where, err);
    }
    return p;
}

std::string config_value(const json &v, const std::string &where)
{
    std::string s;
    if (v.is_boolean()) {
        s = v.get<bool>() ? "1" : "0";
    } else if (v.is_number_integer()) {
        s = v.dump();
    } else if (v.is_number()) {
        s = v.dump();
    } else if (v.is_string()) {
        s = v.get<std::string>();
    } else {
        fail(where, "must be a number, string or boolean");
    }
    if (s.find_first_of(";=") != std::string::npos) {
        fail(where, "must not contain ';' or '='");
    }
    return s;
}

// {"a": 1, "b": true} -> "a=1;b=1" (keys sorted, so the string is deterministic)
std::string config_string(const json &cfg, const std::string &where)
{
    if (!cfg.is_object()) {
        fail(where, "must be an object");
    }
    std::string out;
    for (const auto &item : cfg.items()) {
        if (!out.empty()) {
            out += ';';
        }
        out += item.key() + "=" + config_value(item.value(), where + "." + item.key());
    }
    return out;
}

Vec3 parse_pos(const json &j, const std::string &where)
{
    if (!j.is_array() || j.size() < 2 || j.size() > 3) {
        fail(where, "must be [x, y] or [x, y, z] in meters");
    }
    for (const json &c : j) {
        if (!c.is_number()) {
            fail(where, "coordinates must be numbers");
        }
    }
    Vec3 p;
    p.x = j[0].get<double>();
    p.y = j[1].get<double>();
    p.z = j.size() == 3 ? j[2].get<double>() : 0.0;
    return p;
}

} // namespace

Scenario parse_scenario(const std::string &json_text)
{
    json root;
    try {
        root = json::parse(json_text);
    } catch (const json::parse_error &e) {
        throw std::runtime_error(std::string("scenario: invalid JSON: ") + e.what());
    }

    check_object(root,
                 {"seed", "duration_s", "mtu", "radio", "path_loss", "capture_threshold_db",
                  "noise_figure_db", "duty_cycle_limit", "nodes", "description"},
                 "scenario");

    Scenario s;
    s.seed = get_u64(root, "seed", s.seed, std::numeric_limits<uint64_t>::max(), "scenario");
    const double duration_s = get_double(root, "duration_s", 60.0, "scenario");
    if (!(duration_s > 0.0) || duration_s > 1e9) {
        fail("scenario.duration_s", "must be > 0");
    }
    s.duration_us = static_cast<Time>(std::llround(duration_s * 1e6));
    s.mtu = static_cast<size_t>(get_u64(root, "mtu", s.mtu, 255, "scenario"));
    s.capture_threshold_db =
        get_double(root, "capture_threshold_db", s.capture_threshold_db, "scenario");
    s.noise_figure_db = get_double(root, "noise_figure_db", s.noise_figure_db, "scenario");
    s.duty_cycle_limit = get_double(root, "duty_cycle_limit", s.duty_cycle_limit, "scenario");
    if (root.contains("radio")) {
        s.radio = parse_radio(root.at("radio"), s.radio, "scenario.radio");
    }
    if (root.contains("path_loss")) {
        const json &pl = root.at("path_loss");
        const std::string where = "scenario.path_loss";
        check_object(pl,
                     {"model", "exponent", "ref_distance_m", "ref_loss_db", "shadowing_sigma_db"},
                     where);
        if (pl.contains("model") && pl.at("model") != "log_distance") {
            fail(where + ".model", "only \"log_distance\" is supported");
        }
        s.path_loss.exponent = get_double(pl, "exponent", s.path_loss.exponent, where);
        s.path_loss.ref_distance_m =
            get_double(pl, "ref_distance_m", s.path_loss.ref_distance_m, where);
        if (pl.contains("ref_loss_db")) {
            s.path_loss.ref_loss_db = get_double(pl, "ref_loss_db", 0.0, where);
        }
        s.path_loss.shadowing_sigma_db =
            get_double(pl, "shadowing_sigma_db", s.path_loss.shadowing_sigma_db, where);
        if (s.path_loss.ref_distance_m <= 0.0 || s.path_loss.shadowing_sigma_db < 0.0) {
            fail(where, "ref_distance_m must be > 0 and shadowing_sigma_db >= 0");
        }
    }

    if (!root.contains("nodes") || !root.at("nodes").is_array() || root.at("nodes").empty()) {
        fail("scenario.nodes", "must be a non-empty array");
    }
    for (size_t i = 0; i < root.at("nodes").size(); i++) {
        const json &jn = root.at("nodes")[i];
        const std::string where = "scenario.nodes[" + std::to_string(i) + "]";
        check_object(jn, {"id", "pos", "stack", "config", "radio"}, where);
        NodeSpec n;
        if (!jn.contains("id") || !jn.contains("pos") || !jn.contains("stack")) {
            fail(where, "needs \"id\", \"pos\" and \"stack\"");
        }
        n.id = static_cast<uint32_t>(get_u64(jn, "id", 0, 0xFFFFFFFFu, where));
        n.pos = parse_pos(jn.at("pos"), where + ".pos");
        if (!jn.at("stack").is_string()) {
            fail(where + ".stack", "must be a string");
        }
        n.stack = jn.at("stack").get<std::string>();
        n.radio = jn.contains("radio") ? parse_radio(jn.at("radio"), s.radio, where + ".radio")
                                       : s.radio;
        if (jn.contains("config")) {
            n.config = config_string(jn.at("config"), where + ".config");
        }
        for (const NodeSpec &other : s.nodes) {
            if (other.id == n.id) {
                fail(where + ".id", "duplicate id " + std::to_string(n.id));
            }
        }
        s.nodes.push_back(std::move(n));
    }
    return s;
}

Scenario load_scenario_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream text;
    text << in.rdbuf();
    try {
        return parse_scenario(text.str());
    } catch (const std::runtime_error &e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

} // namespace ksim

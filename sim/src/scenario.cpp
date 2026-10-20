/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/scenario.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "json_util.hpp"

namespace ksim {

using namespace json_util;

namespace {

std::string config_value(const json &v, const std::string &where)
{
    std::string s;
    if (v.is_boolean()) {
        s = v.get<bool>() ? "1" : "0";
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

void parse_path_loss(const json &pl, PathLoss &out)
{
    const std::string where = "scenario.path_loss";
    check_object(pl, {"model", "exponent", "ref_distance_m", "ref_loss_db", "shadowing_sigma_db"},
                 where);
    if (pl.contains("model") && pl.at("model") != "log_distance") {
        fail(where + ".model", "only \"log_distance\" is supported");
    }
    out.exponent = get_double(pl, "exponent", out.exponent, where);
    out.ref_distance_m = get_double(pl, "ref_distance_m", out.ref_distance_m, where);
    if (pl.contains("ref_loss_db")) {
        out.ref_loss_db = get_double(pl, "ref_loss_db", 0.0, where);
    }
    out.shadowing_sigma_db = get_double(pl, "shadowing_sigma_db", out.shadowing_sigma_db, where);
    if (out.ref_distance_m <= 0.0 || out.shadowing_sigma_db < 0.0) {
        fail(where, "ref_distance_m must be > 0 and shadowing_sigma_db >= 0");
    }
}

NodeSpec parse_node(const json &jn, const Scenario &s, const DeviceRegistry &devices,
                    const std::string &where)
{
    check_object(jn, {"id", "pos", "device", "stack", "config", "radio"}, where);
    if (!jn.contains("id") || !jn.contains("pos") || !jn.contains("stack")) {
        fail(where, "needs \"id\", \"pos\" and \"stack\"");
    }
    NodeSpec n;
    n.id = static_cast<uint32_t>(get_u64(jn, "id", 0, 0xFFFFFFFFu, where));
    n.pos = parse_pos(jn.at("pos"), where + ".pos");
    n.stack = get_string(jn, "stack", "", where);

    // radio: device defaults (or the scenario default), then the node's own overrides
    n.radio = s.radio;
    if (jn.contains("device")) {
        const std::string name = get_string(jn, "device", "", where);
        const DeviceProfile *d = devices.find(name);
        if (d == nullptr) {
            fail(where + ".device", "unknown device '" + name + "' (see `ksim devices`)");
        }
        n.device = *d;
        n.radio = d->radio;
    }
    if (jn.contains("radio")) {
        const json &jr = jn.at("radio");
        n.radio = parse_radio(jr, n.radio, where + ".radio");
        if (n.device && n.radio.type != n.device->radio.type) {
            fail(where + ".radio.type", n.device->check(n.radio));
        }
    }
    if (n.device) {
        const std::string err = n.device->check(n.radio);
        if (!err.empty()) {
            fail(where + ".radio", err);
        }
    }
    if (jn.contains("config")) {
        n.config = config_string(jn.at("config"), where + ".config");
    }
    return n;
}

} // namespace

Scenario parse_scenario(const std::string &json_text, const DeviceRegistry &devices)
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
        parse_path_loss(root.at("path_loss"), s.path_loss);
    }

    if (!root.contains("nodes") || !root.at("nodes").is_array() || root.at("nodes").empty()) {
        fail("scenario.nodes", "must be a non-empty array");
    }
    for (size_t i = 0; i < root.at("nodes").size(); i++) {
        const std::string where = "scenario.nodes[" + std::to_string(i) + "]";
        NodeSpec n = parse_node(root.at("nodes")[i], s, devices, where);
        for (const NodeSpec &other : s.nodes) {
            if (other.id == n.id) {
                fail(where + ".id", "duplicate id " + std::to_string(n.id));
            }
        }
        s.nodes.push_back(std::move(n));
    }
    return s;
}

Scenario parse_scenario(const std::string &json_text)
{
    return parse_scenario(json_text, DeviceRegistry::builtin());
}

Scenario load_scenario_file(const std::string &path, const DeviceRegistry &devices)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream text;
    text << in.rdbuf();
    try {
        return parse_scenario(text.str(), devices);
    } catch (const std::runtime_error &e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

Scenario load_scenario_file(const std::string &path)
{
    return load_scenario_file(path, DeviceRegistry::builtin());
}

} // namespace ksim

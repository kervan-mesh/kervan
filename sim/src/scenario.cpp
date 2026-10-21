/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/scenario.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "json_util.hpp"

namespace ksim {

using namespace json_util;

namespace {

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "01 A2 ff", "0x01a2ff" -> bytes; whitespace between bytes is ignored.
std::vector<uint8_t> parse_hex(const std::string &text, const std::string &where)
{
    std::string digits;
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        i++;
    }
    if (text.compare(i, 2, "0x") == 0 || text.compare(i, 2, "0X") == 0) {
        i += 2;
    }
    for (; i < text.size(); i++) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (hex_digit(c) < 0) {
            fail(where, std::string("'") + c + "' is not a hex digit");
        }
        digits += c;
    }
    if (digits.size() % 2 != 0) {
        fail(where, "needs an even number of hex digits (two per byte)");
    }
    std::vector<uint8_t> out;
    for (size_t k = 0; k < digits.size(); k += 2) {
        out.push_back(static_cast<uint8_t>(hex_digit(digits[k]) * 16 + hex_digit(digits[k + 1])));
    }
    return out;
}

PayloadSpec parse_payload(const json &jp, const std::string &where)
{
    check_object(jp, {"hex", "python"}, where);
    if (jp.contains("hex") == jp.contains("python")) {
        fail(where, "needs either \"hex\": \"01 A2 ...\" or \"python\": \"def payload(...): ...\"");
    }
    PayloadSpec p;
    if (jp.contains("hex")) {
        p.bytes = parse_hex(get_string(jp, "hex", "", where), where + ".hex");
        if (p.bytes.size() > 1024) {
            fail(where + ".hex", "longer than 1024 bytes");
        }
    } else {
        p.python = get_string(jp, "python", "", where);
        if (p.python.find("def payload") == std::string::npos) {
            fail(where + ".python", "must define payload(node, seq, t)");
        }
    }
    return p;
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

std::string env_var(const char *name)
{
#if defined(_MSC_VER)
    char *buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
        return {};
    }
    std::string value(buf);
    std::free(buf);
    return value;
#else
    const char *value = std::getenv(name);
    return value != nullptr ? value : "";
#endif
}

std::string default_python()
{
    const std::string env = env_var("KSIM_PYTHON");
    if (!env.empty()) {
        return env;
    }
#if defined(_WIN32)
    return "python"; // "python3" is often only the Microsoft Store stub on Windows
#else
    return "python3";
#endif
}

// Replace ${scenario_dir} and ${python} so scenario files work on every OS and from any directory.
std::string expand(std::string arg, const std::string &scenario_dir)
{
    const std::pair<std::string, std::string> vars[] = {{"${scenario_dir}", scenario_dir},
                                                        {"${python}", default_python()}};
    for (const auto &[name, value] : vars) {
        for (size_t pos = arg.find(name); pos != std::string::npos;
             pos = arg.find(name, pos + value.size())) {
            arg.replace(pos, name.size(), value);
        }
    }
    return arg;
}

ProcessSpec parse_process(const json &jp, const std::string &where, const std::string &dir)
{
    check_object(jp, {"spawn", "wait", "timeout_s"}, where);
    ProcessSpec ps;
    if (jp.contains("spawn")) {
        const json &argv = jp.at("spawn");
        if (!argv.is_array() || argv.empty()) {
            fail(where + ".spawn", "must be a non-empty array of strings");
        }
        for (const json &arg : argv) {
            if (!arg.is_string()) {
                fail(where + ".spawn", "must be a non-empty array of strings");
            }
            ps.spawn.push_back(expand(arg.get<std::string>(), dir));
        }
    }
    ps.wait = get_bool(jp, "wait", false, where);
    ps.timeout_s = get_double(jp, "timeout_s", 0.0, where);
    if (ps.timeout_s < 0.0) {
        fail(where + ".timeout_s", "must be >= 0");
    }
    if (ps.spawn.empty() == !ps.wait) {
        fail(where, "needs either \"spawn\": [...] or \"wait\": true");
    }
    return ps;
}

NodeSpec parse_node(const json &jn, const Scenario &s, const DeviceRegistry &devices,
                    const std::string &where, const std::string &dir)
{
    check_object(jn,
                 {"id", "pos", "device", "mcu", "module", "stack", "config", "radio", "process",
                  "payload"},
                 where);
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
        if (jn.contains("mcu") || jn.contains("module")) {
            fail(where, "takes either a \"device\" (board) or \"mcu\" + \"module\", not both");
        }
    } else if (jn.contains("module")) {
        // a module, optionally wired to an MCU
        try {
            n.device = devices.combine(get_string(jn, "mcu", "", where),
                                       get_string(jn, "module", "", where));
        } catch (const std::runtime_error &e) {
            fail(where, e.what());
        }
        n.radio = n.device->radio;
    } else if (jn.contains("mcu")) {
        fail(where, "an \"mcu\" needs a radio \"module\"");
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
    if (jn.contains("process")) {
        n.process = parse_process(jn.at("process"), where + ".process", dir);
    }
    if (jn.contains("payload")) {
        n.payload = parse_payload(jn.at("payload"), where + ".payload");
    }
    return n;
}

Scenario parse(const std::string &json_text, const DeviceRegistry &devices, const std::string &dir)
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
        NodeSpec n = parse_node(root.at("nodes")[i], s, devices, where, dir);
        for (const NodeSpec &other : s.nodes) {
            if (other.id == n.id) {
                fail(where + ".id", "duplicate id " + std::to_string(n.id));
            }
        }
        s.nodes.push_back(std::move(n));
    }
    return s;
}

} // namespace

Scenario parse_scenario(const std::string &json_text, const DeviceRegistry &devices)
{
    return parse(json_text, devices, ".");
}

Scenario parse_scenario(const std::string &json_text)
{
    return parse_scenario(json_text, DeviceRegistry::builtin());
}

Scenario parse_scenario(const std::string &json_text, const DeviceRegistry &devices,
                        const std::string &base_dir)
{
    return parse(json_text, devices, base_dir.empty() ? "." : base_dir);
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
        const std::string dir = std::filesystem::path(path).parent_path().string();
        return parse(text.str(), devices, dir.empty() ? "." : dir);
    } catch (const std::runtime_error &e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

Scenario load_scenario_file(const std::string &path)
{
    return load_scenario_file(path, DeviceRegistry::builtin());
}

std::string python_interpreter()
{
    return default_python();
}

} // namespace ksim

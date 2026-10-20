/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "api.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <utility>

#include <nlohmann/json.hpp>

#include "../json_util.hpp"
#include "ksim/phy.hpp"
#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"

namespace ksim::serve {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

Response json_response(const json &j, int status = 200)
{
    Response r;
    r.status = status;
    r.body = j.dump();
    return r;
}

Response error(int status, const std::string &msg)
{
    return json_response(json{{"error", msg}}, status);
}

// Scenario names map to files in the scenario directory: no paths, no surprises.
bool valid_name(const std::string &name)
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool read_file(const fs::path &path, std::string &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}

std::string hex64(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

double round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

// Collects trace lines up to a size limit; cuts at a line boundary.
class TraceBuffer : public std::streambuf {
public:
    explicit TraceBuffer(size_t limit) : limit_(limit) {}

    // Lines joined by ',' (the inside of a JSON array) and whether lines were dropped.
    std::string take_array_items(bool &truncated)
    {
        if (truncated_) {
            const size_t last = text_.rfind('\n');
            text_.resize(last == std::string::npos ? 0 : last + 1);
        }
        truncated = truncated_;
        if (!text_.empty()) {
            text_.pop_back(); // final '\n'
        }
        std::replace(text_.begin(), text_.end(), '\n', ',');
        return std::move(text_);
    }

protected:
    std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        const auto len = static_cast<size_t>(n);
        if (truncated_ || text_.size() + len > limit_) {
            truncated_ = true;
        } else {
            text_.append(s, len);
        }
        return n;
    }

    int_type overflow(int_type ch) override
    {
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            const char c = traits_type::to_char_type(ch);
            xsputn(&c, 1);
        }
        return traits_type::not_eof(ch);
    }

private:
    size_t limit_;
    bool truncated_ = false;
    std::string text_;
};

json device_json(const DeviceProfile &d)
{
    json j;
    j["name"] = d.name;
    j["version"] = d.version;
    j["description"] = d.description;
    j["mcu"] = d.mcu;
    j["radio"] = json_util::radio_json(d.radio);
    json limits = json::object();
    if (d.tx_power_range) {
        limits["tx_power_dbm"] = {{"min", d.tx_power_range->min}, {"max", d.tx_power_range->max}};
    }
    if (!d.tx_power_levels.empty()) {
        limits["tx_power_levels"] = d.tx_power_levels;
    }
    if (d.freq_hz_range) {
        limits["freq_hz"] = {{"min", d.freq_hz_range->min}, {"max", d.freq_hz_range->max}};
    }
    j["limits"] = limits;
    return j;
}

json option(const char *key, const char *type, const char *help)
{
    return json{{"key", key}, {"type", type}, {"help", help}};
}

json enum_option(const char *key, json values, const char *help)
{
    json j = option(key, "enum", help);
    j["values"] = std::move(values);
    return j;
}

json int_option(const char *key, int64_t min, int64_t max, const char *help)
{
    json j = option(key, "int", help);
    j["min"] = min;
    j["max"] = max;
    return j;
}

// Radio settings per PHY in the stack config-schema format, defaults from RadioConfig.
json phy_options(PhyType type)
{
    json opts = json::array();
    opts.push_back(option("tx_power_dbm", "number", "Transmit power (limited by the device)"));
    if (type == PhyType::LoRa) {
        opts.push_back(int_option("freq_hz", 137000000, 1020000000, "Carrier frequency"));
        opts.push_back(enum_option("sf", {7, 8, 9, 10, 11, 12},
                                   "Spreading factor: higher = longer range, longer airtime"));
        opts.push_back(enum_option("bw_hz", {125000, 250000, 500000}, "Bandwidth"));
        opts.push_back(enum_option("cr", {1, 2, 3, 4}, "Coding rate 4/(4+cr)"));
        opts.push_back(int_option("preamble", 6, 65535, "Preamble symbols"));
        opts.push_back(option("crc", "bool", "Payload CRC"));
        opts.push_back(option("implicit_header", "bool", "Implicit header mode"));
    } else {
        opts.push_back(int_option("channel", 0, 125, "RF channel: 2400 + channel MHz"));
        opts.push_back(enum_option("data_rate", {250000, 1000000, 2000000}, "Air data rate"));
        opts.push_back(enum_option("crc_bytes", {1, 2}, "CRC length"));
    }
    RadioConfig def;
    def.type = type;
    def.tx_power_dbm = type == PhyType::Nrf24 ? 0.0 : 14.0; // as parse_radio does
    const json values = json_util::radio_json(def);
    for (json &o : opts) {
        o["default"] = values.at(o.at("key").get<std::string>());
    }
    return opts;
}

json summary_json(const Scenario &scn, const Summary &sum)
{
    json j;
    j["seed"] = scn.seed;
    j["duration_us"] = sum.duration_us;
    j["duty_cycle_limit"] = scn.duty_cycle_limit;
    j["tx"] = sum.tx;
    j["rx_ok"] = sum.rx_ok;
    j["lost_collision"] = sum.lost_collision;
    j["lost_half_duplex"] = sum.lost_half_duplex;
    j["lost_rx_off"] = sum.lost_rx_off;
    j["app_events"] = sum.app_events;
    j["versions"] = sum.versions;
    j["trace_hash"] = hex64(sum.trace_hash);
    json nodes = json::array();
    for (const NodeStats &n : sum.nodes) {
        nodes.push_back({{"id", n.id},
                         {"airtime_us", n.airtime_us},
                         {"tx", n.tx},
                         {"rx_ok", n.rx_ok},
                         {"lost_collision", n.lost_collision},
                         {"lost_half_duplex", n.lost_half_duplex},
                         {"lost_rx_off", n.lost_rx_off}});
    }
    j["nodes"] = nodes;
    return j;
}

// Who can hear whom without interference: every directed pair whose signal is decodable.
json links_json(const Scenario &scn, const Simulator &sim)
{
    json links = json::array();
    for (const NodeSpec &tx : scn.nodes) {
        const Phy &phy = phy_for(tx.radio.type);
        const double noise = phy.noise_floor_dbm(tx.radio, scn.noise_figure_db);
        for (const NodeSpec &rx : scn.nodes) {
            if (rx.id == tx.id || rx.radio.type != tx.radio.type ||
                !phy.can_receive(rx.radio, tx.radio)) {
                continue;
            }
            const double rssi = sim.rssi_dbm(tx.id, rx.id);
            const double snr = round2(rssi - noise);
            if (phy.decodable(tx.radio, rssi, snr)) {
                links.push_back({{"from", tx.id}, {"to", rx.id}, {"rssi", rssi}, {"snr", snr}});
            }
        }
    }
    return links;
}

} // namespace

Api::Api(Options options) : opt_(std::move(options)) {}

Response Api::handle(const Request &req) const
{
    const std::string prefix = "/api/";
    if (req.path.compare(0, prefix.size(), prefix) != 0) {
        return error(404, "not found");
    }
    const std::string route = req.path.substr(prefix.size());
    const bool get = req.method == "GET";

    try {
        static const std::map<std::string, Response (Api::*)() const> listings = {
            {"info", &Api::info},   {"devices", &Api::devices},         {"stacks", &Api::stacks},
            {"phys", &Api::phys},   {"scenarios", &Api::scenario_list},
        };
        if (auto it = listings.find(route); it != listings.end()) {
            return get ? (this->*it->second)() : error(405, "use GET");
        }
        if (route == "check" || route == "run") {
            if (req.method != "POST") {
                return error(405, "use POST");
            }
            return route == "check" ? check(req.body) : run(req.body);
        }
        const std::string scenarios = "scenarios/";
        if (route.compare(0, scenarios.size(), scenarios) == 0) {
            const std::string name = route.substr(scenarios.size());
            if (!valid_name(name)) {
                return error(400, "scenario names use letters, digits, '_' and '-' (max 64)");
            }
            if (get) {
                return scenario_get(name);
            }
            return req.method == "PUT" ? scenario_put(name, req.body)
                                       : error(405, "use GET or PUT");
        }
        return error(404, "not found");
    } catch (const std::exception &e) {
        return error(500, e.what());
    }
}

Response Api::info() const
{
    return json_response({{"ksim", version()},
                          {"abi", KSIM_ABI_VERSION},
                          {"scenarios", !opt_.scenario_dir.empty()},
                          {"allow_processes", opt_.allow_processes}});
}

Response Api::devices() const
{
    json list = json::array();
    for (const DeviceProfile *d : opt_.devices.all()) {
        list.push_back(device_json(*d));
    }
    return json_response(list);
}

Response Api::stacks() const
{
    json list = json::array();
    for (const ksim_stack_t *s : opt_.stacks) {
        json j{{"name", s->name}, {"version", s->version != nullptr ? s->version : ""}};
        j["schema"] = nullptr;
        if (s->config_schema != nullptr) {
            try {
                j["schema"] = json::parse(s->config_schema);
            } catch (const json::parse_error &e) {
                j["schema_error"] = e.what(); // the UI falls back to free key=value editing
            }
        }
        list.push_back(j);
    }
    return json_response(list);
}

Response Api::phys() const
{
    json out = json::object();
    for (PhyType t : {PhyType::LoRa, PhyType::Nrf24}) {
        const Phy &phy = phy_for(t);
        out[phy.name()] = {{"version", phy.version()}, {"options", phy_options(t)}};
    }
    return json_response(out);
}

Response Api::scenario_list() const
{
    json list = json::array();
    if (opt_.scenario_dir.empty()) {
        return json_response(list);
    }
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(opt_.scenario_dir, ec)) {
        const fs::path &p = entry.path();
        if (entry.is_regular_file() && p.extension() == ".json" && valid_name(p.stem().string())) {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path &p : files) {
        json item{{"name", p.stem().string()}};
        std::string text;
        try {
            if (!read_file(p, text)) {
                throw std::runtime_error("cannot read the file");
            }
            const json s = json::parse(text);
            item["description"] = s.value("description", "");
            item["nodes"] = s.contains("nodes") && s.at("nodes").is_array() ? s.at("nodes").size()
                                                                             : 0u;
        } catch (const std::exception &e) {
            item["error"] = e.what();
        }
        list.push_back(item);
    }
    return json_response(list);
}

Response Api::scenario_get(const std::string &name) const
{
    std::string text;
    if (opt_.scenario_dir.empty() ||
        !read_file(fs::path(opt_.scenario_dir) / (name + ".json"), text)) {
        return error(404, "no scenario '" + name + "'");
    }
    Response r;
    r.body = std::move(text);
    return r;
}

Response Api::scenario_put(const std::string &name, const std::string &body) const
{
    if (opt_.scenario_dir.empty()) {
        return error(404, "ksim serve was started without a scenario directory");
    }
    try {
        parse_scenario(body, opt_.devices, opt_.scenario_dir);
    } catch (const std::exception &e) {
        return error(400, e.what());
    }
    const fs::path path = fs::path(opt_.scenario_dir) / (name + ".json");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    if (body.empty() || body.back() != '\n') {
        out << '\n';
    }
    out.close();
    if (!out) {
        return error(500, "cannot write " + path.string());
    }
    return json_response({{"ok", true}, {"name", name}});
}

Response Api::check(const std::string &body) const
{
    try {
        const Scenario scn = parse_scenario(body, opt_.devices, opt_.scenario_dir);
        for (const NodeSpec &n : scn.nodes) {
            const bool known = std::any_of(opt_.stacks.begin(), opt_.stacks.end(),
                                           [&](const ksim_stack_t *s) { return n.stack == s->name; });
            if (!known && !n.process) {
                return error(400, "node " + std::to_string(n.id) + ": unknown stack '" + n.stack +
                                      "'");
            }
        }
        const Simulator sim(scn);
        return json_response({{"ok", true}, {"links", links_json(scn, sim)}});
    } catch (const std::exception &e) {
        return error(400, e.what());
    }
}

Response Api::run(const std::string &body) const
{
    Scenario scn;
    try {
        scn = parse_scenario(body, opt_.devices, opt_.scenario_dir);
    } catch (const std::exception &e) {
        return error(400, e.what());
    }
    for (const NodeSpec &n : scn.nodes) {
        if (!n.process) {
            continue;
        }
        const std::string where = "node " + std::to_string(n.id) + ": ";
        if (n.process->wait) {
            return error(400, where + "\"wait\" nodes need a terminal; use `ksim run`");
        }
        if (!opt_.allow_processes) {
            return error(403, where + "out-of-process nodes are disabled; start "
                                      "`ksim serve --allow-processes` to run them");
        }
    }

    TraceBuffer buffer(opt_.max_trace_bytes);
    std::ostream trace(&buffer);
    json head;
    json links;
    try {
        Simulator sim(scn);
        for (const ksim_stack_t *s : opt_.stacks) {
            sim.register_stack(s);
        }
        sim.set_remote_options(0, opt_.program_dir);
        sim.set_trace(&trace);
        const Summary sum = sim.run();
        head["summary"] = summary_json(scn, sum);
        links = links_json(scn, sim);
    } catch (const std::exception &e) {
        return error(400, e.what());
    }
    bool truncated = false;
    const std::string items = buffer.take_array_items(truncated);
    head["truncated"] = truncated;
    head["links"] = std::move(links);

    // {"summary":...,"truncated":...,"links":[...],"trace":[...]} without re-parsing the trace
    std::string out = head.dump();
    out.pop_back(); // '}'
    out.reserve(out.size() + items.size() + 16);
    out += ",\"trace\":[";
    out += items;
    out += "]}";
    Response r;
    r.body = std::move(out);
    return r;
}

} // namespace ksim::serve

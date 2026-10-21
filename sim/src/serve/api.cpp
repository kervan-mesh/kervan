/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "api.hpp"
#include "live.hpp"
#include "report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
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

json range_json(const Range &r)
{
    return {{"min", r.min}, {"max", r.max}};
}

json limits_json(const std::vector<double> &levels, const std::optional<Range> &power,
                 const std::optional<Range> &freq)
{
    json limits = json::object();
    if (power) {
        limits["tx_power_dbm"] = range_json(*power);
    }
    if (!levels.empty()) {
        limits["tx_power_levels"] = levels;
    }
    if (freq) {
        limits["freq_hz"] = range_json(*freq);
    }
    return limits;
}

json mcu_json(const McuProfile &m)
{
    return {{"name", m.name},         {"version", m.version},   {"title", m.title},
            {"description", m.description}, {"chip", m.chip},   {"logic_v", m.logic_v},
            {"ram_kb", m.ram_kb},     {"flash_kb", m.flash_kb}, {"clock_mhz", m.clock_mhz},
            {"supply_out_ma", m.supply_out_ma}, {"notes", m.notes}};
}

json module_json(const ModuleProfile &m)
{
    json pins = json::array();
    for (const ModulePin &p : m.pins) {
        pins.push_back({{"name", p.name}, {"role", p.role}});
    }
    json j{{"name", m.name},
           {"version", m.version},
           {"title", m.title},
           {"description", m.description},
           {"chip", m.chip},
           {"radio", json_util::radio_json(m.radio)},
           {"limits", limits_json(m.tx_power_levels, m.tx_power_range, m.freq_hz_range)},
           {"antenna_dbi", m.radio.antenna_dbi},
           {"lna_db", m.radio.lna_db},
           {"current_ma", {{"tx", m.tx_ma}, {"rx", m.rx_ma}}},
           {"pins", pins},
           {"notes", m.notes}};
    j["supply_v"] = m.supply_v ? range_json(*m.supply_v) : json(nullptr);
    return j;
}

json board_json(const BoardProfile &b, const DeviceProfile &d)
{
    return {{"name", b.name},
            {"version", b.version},
            {"title", b.title},
            {"description", b.description},
            {"mcu", b.mcu},
            {"module", b.module},
            {"integrated", b.integrated},
            {"radio", json_util::radio_json(d.radio)},
            {"limits", limits_json(d.tx_power_levels, d.tx_power_range, d.freq_hz_range)}};
}

json wiring_json(const Wiring &w)
{
    json rows = json::array();
    for (const WiringRow &r : w.rows) {
        rows.push_back({{"module_pin", r.module_pin}, {"mcu_pin", r.mcu_pin}, {"role", r.role}});
    }
    return {{"integrated", w.integrated},
            {"rows", rows},
            {"warnings", w.warnings},
            {"notes", w.notes}};
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

} // namespace

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
            if (phy.decodable(tx.radio, rssi + rx.radio.lna_db, snr)) {
                links.push_back({{"from", tx.id}, {"to", rx.id}, {"rssi", rssi}, {"snr", snr}});
            }
        }
    }
    return links;
}

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
        if (route == "check" || route == "run" || route == "decode") {
            if (req.method != "POST") {
                return error(405, "use POST");
            }
            return route == "check" ? check(req.body)
                   : route == "run" ? run(req.body)
                                    : decode(req.body);
        }
        if (route.compare(0, 5, "live/") == 0) {
            return live(req.method, route.substr(5), req.body);
        }
        const std::string wiring_prefix = "wiring/";
        if (route.compare(0, wiring_prefix.size(), wiring_prefix) == 0) {
            return get ? wiring(route.substr(wiring_prefix.size())) : error(405, "use GET");
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
    const DeviceRegistry &reg = opt_.devices;
    json boards = json::array();
    for (const DeviceProfile *d : reg.all()) {
        boards.push_back(board_json(*reg.board_profile(d->name), *d));
    }
    json mcus = json::array();
    for (const McuProfile *m : reg.mcus()) {
        mcus.push_back(mcu_json(*m));
    }
    json modules = json::array();
    for (const ModuleProfile *m : reg.modules()) {
        modules.push_back(module_json(*m));
    }
    return json_response({{"boards", boards}, {"mcus", mcus}, {"modules", modules}});
}

// /api/wiring/BOARD or /api/wiring/MCU/MODULE
Response Api::wiring(const std::string &path) const
{
    const DeviceRegistry &reg = opt_.devices;
    const size_t slash = path.find('/');
    std::string mcu;
    std::string module;
    if (slash == std::string::npos) {
        const BoardProfile *b = reg.board_profile(path);
        if (b == nullptr) {
            return error(404, "no board '" + path + "'");
        }
        if (b->integrated) {
            Wiring w;
            w.integrated = true;
            w.notes = reg.module(b->module)->notes;
            return json_response(wiring_json(w));
        }
        mcu = b->mcu;
        module = b->module;
    } else {
        mcu = path.substr(0, slash);
        module = path.substr(slash + 1);
    }
    if (!valid_name(mcu) || !valid_name(module)) {
        return error(400, "use /api/wiring/BOARD or /api/wiring/MCU/MODULE");
    }
    try {
        return json_response(wiring_json(reg.wiring(mcu, module)));
    } catch (const std::runtime_error &e) {
        return error(404, e.what());
    }
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

// Parse a scenario for running and apply the server's rules; a response on failure.
std::optional<Response> Api::prepare(const std::string &body, Scenario &scn) const
{
    try {
        scn = parse_scenario(body, opt_.devices, opt_.scenario_dir);
    } catch (const std::exception &e) {
        return error(400, e.what());
    }
    for (const NodeSpec &n : scn.nodes) {
        if (n.payload && !n.payload->python.empty() && !opt_.allow_processes) {
            return error(403, "node " + std::to_string(n.id) +
                                  ": Python payloads run code on this computer; start "
                                  "`ksim serve --allow-processes` to run them");
        }
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
    return std::nullopt;
}

Response Api::run(const std::string &body) const
{
    Scenario scn;
    if (auto failed = prepare(body, scn)) {
        return *failed;
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

Response Api::decode(const std::string &body) const
{
    json req;
    try {
        req = json::parse(body);
    } catch (const json::parse_error &) {
        return error(400, "invalid JSON");
    }
    if (!req.is_object() || !req.contains("stack") || !req.at("stack").is_string() ||
        !req.contains("data") || !req.at("data").is_string()) {
        return error(400, "needs \"stack\" and \"data\" (hex)");
    }
    const std::string name = req.at("stack").get<std::string>();
    const auto it = std::find_if(opt_.stacks.begin(), opt_.stacks.end(),
                                 [&](const ksim_stack_t *s) { return name == s->name; });
    if (it == opt_.stacks.end()) {
        return error(404, "unknown stack '" + name + "'");
    }

    const std::string hex = req.at("data").get<std::string>();
    std::vector<uint8_t> frame;
    if (hex.size() % 2 != 0 || hex.size() > 1024 ||
        hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return error(400, "\"data\" must be hex bytes");
    }
    for (size_t i = 0; i < hex.size(); i += 2) {
        frame.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    std::string config;
    try {
        if (req.contains("config")) {
            config = json_util::config_string(req.at("config"), "config");
        }
    } catch (const std::exception &e) {
        return error(400, e.what());
    }

    // null fields: the stack has no decoder or does not understand the frame (show hex only)
    json fields = nullptr;
    if ((*it)->describe_frame != nullptr) {
        std::vector<char> out(16384);
        const size_t n = (*it)->describe_frame(config.c_str(), frame.data(), frame.size(),
                                               out.data(), out.size());
        if (n > 0 && n < out.size()) {
            try {
                fields = json::parse(std::string(out.data(), n));
            } catch (const json::parse_error &) {
                fields = nullptr; // a broken decoder must not break the inspector
            }
        }
    }
    return json_response({{"fields", fields}});
}

// ---- live sessions ----

json snapshot_json(const LiveSession::Snapshot &s)
{
    json j{{"state", s.state},       {"t_us", s.t_us},        {"duration_us", s.duration_us},
           {"speed", s.speed},       {"total_lines", s.total_lines}, {"truncated", s.truncated}};
    if (!s.error.empty()) {
        j["error"] = s.error;
    }
    return j;
}

std::string Api::live_message(const LiveSession::Snapshot &s)
{
    // the lines are JSON already: splice them in instead of parsing them again
    std::string out = snapshot_json(s).dump();
    out.pop_back(); // '}'
    out += ",\"from\":" + std::to_string(s.from) + ",\"lines\":[";
    for (size_t i = 0; i < s.lines.size(); i++) {
        if (i > 0) {
            out += ',';
        }
        out += s.lines[i];
    }
    out += "]}";
    return out;
}

std::shared_ptr<LiveSession> Api::live_session() const
{
    const std::lock_guard<std::mutex> lock(live_m_);
    return live_;
}

Response Api::live(const std::string &method, const std::string &action,
                   const std::string &body) const
{
    if (action == "start") {
        if (method != "POST") {
            return error(405, "use POST");
        }
        json req;
        try {
            req = json::parse(body);
        } catch (const json::parse_error &) {
            return error(400, "invalid JSON");
        }
        if (!req.is_object() || !req.contains("scenario")) {
            return error(400, "needs {\"scenario\": {...}, \"speed\": 1}");
        }
        Scenario scn;
        if (auto failed = prepare(req.at("scenario").dump(), scn)) {
            return *failed;
        }
        LiveSession::Options o;
        o.speed = req.value("speed", 1.0);
        o.max_trace_bytes = opt_.max_trace_bytes;
        o.program_dir = opt_.program_dir;
        std::shared_ptr<LiveSession> previous;
        {
            const std::lock_guard<std::mutex> lock(live_m_);
            previous = std::move(live_); // one live session at a time
        }
        if (previous) {
            previous->stop(); // also ends streams that still hold it
        }
        previous.reset(); // outside the lock: joining takes a moment
        std::shared_ptr<LiveSession> session;
        try {
            session = std::make_shared<LiveSession>(std::move(scn), opt_.stacks, o);
        } catch (const std::exception &e) {
            return error(400, e.what());
        }
        {
            const std::lock_guard<std::mutex> lock(live_m_);
            live_ = session;
        }
        return json_response(snapshot_json(session->poll(0, 0)));
    }

    const std::shared_ptr<LiveSession> session = live_session();
    if (!session) {
        return error(404, "no live session");
    }
    if (action == "status") {
        return method == "GET" ? json_response(snapshot_json(session->poll(~size_t{0}, 0)))
                               : error(405, "use GET");
    }
    if (action == "result") {
        if (method != "GET") {
            return error(405, "use GET");
        }
        std::string result = session->result();
        if (result.empty()) {
            return error(409, "the live session is still running");
        }
        Response r;
        r.body = std::move(result);
        return r;
    }
    if (action == "control") {
        if (method != "POST") {
            return error(405, "use POST");
        }
        json req;
        try {
            req = json::parse(body);
        } catch (const json::parse_error &) {
            return error(400, "invalid JSON");
        }
        const std::string what = req.is_object() ? req.value("action", "") : "";
        if (what == "pause") {
            session->pause();
        } else if (what == "resume") {
            session->resume();
        } else if (what == "stop") {
            session->stop();
        } else if (what == "speed" && req.contains("speed") && req.at("speed").is_number()) {
            session->set_speed(req.at("speed").get<double>());
        } else {
            return error(400, "action: pause, resume, stop, or speed with \"speed\": number");
        }
        return json_response(snapshot_json(session->poll(~size_t{0}, 0)));
    }
    return error(404, "not found");
}

} // namespace ksim::serve

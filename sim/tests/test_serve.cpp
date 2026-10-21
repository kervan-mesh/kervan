/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// `ksim serve`: the web UI's API (without sockets) and the HTTP layer's guards.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api.hpp"
#include "http.hpp"
#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"

using namespace ksim;
using namespace ksim::serve;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const std::string kScenarios = KSIM_SCENARIO_DIR;

std::string read_text(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

Options options(const std::string &scenario_dir = kScenarios)
{
    Options o;
    o.stacks = {ksim_stack_kervan(), ksim_stack_nrf24_esb()};
    o.scenario_dir = scenario_dir;
    return o;
}

Response call(const Api &api, const std::string &method, const std::string &path,
              const std::string &body = "")
{
    return api.handle(Request{method, path, body});
}

json body_json(const Response &r)
{
    return json::parse(r.body);
}

// A fresh, empty directory for one test.
fs::path temp_dir(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / ("ksim_test_serve_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

uint64_t direct_hash(const std::string &scenario_json)
{
    Simulator sim(parse_scenario(scenario_json));
    sim.register_stack(ksim_stack_kervan());
    sim.register_stack(ksim_stack_nrf24_esb());
    return sim.run().trace_hash;
}

// The config string a schema's defaults produce, as scenario JSON config values.
json schema_defaults(const json &schema)
{
    json cfg = json::object();
    for (const json &o : schema.at("options")) {
        if (o.contains("default")) {
            cfg[o.at("key").get<std::string>()] = o.at("default");
        }
    }
    return cfg;
}

// Runs a server on a free port until the end of the scope, also when an assertion fails.
class RunningServer {
public:
    RunningServer(const Api &api, const std::string &web_dir)
        : server_(api, web_dir), port_(server_.bind(0)), thread_([this] { server_.listen(); })
    {
    }
    ~RunningServer()
    {
        server_.stop();
        thread_.join();
    }
    RunningServer(const RunningServer &) = delete;
    RunningServer &operator=(const RunningServer &) = delete;
    uint16_t port() const { return port_; }

private:
    HttpServer server_;
    uint16_t port_;
    std::thread thread_;
};

} // namespace

TEST(ServeApi, Catalogs)
{
    const Api api(options());

    const json info = body_json(call(api, "GET", "/api/info"));
    EXPECT_EQ(info.at("abi"), KSIM_ABI_VERSION);
    EXPECT_EQ(info.at("allow_processes"), false);

    const json hw = body_json(call(api, "GET", "/api/devices"));
    ASSERT_GE(hw.at("boards").size(), 3u);
    bool found = false;
    for (const json &d : hw.at("boards")) {
        if (d.at("name") == "esp32-sx1276") {
            found = true;
            EXPECT_EQ(d.at("radio").at("type"), "lora");
            EXPECT_EQ(d.at("limits").at("tx_power_dbm").at("max"), 20);
            EXPECT_EQ(d.at("integrated"), true);
            EXPECT_EQ(d.at("module"), "sx1276");
        }
        if (d.at("name") == "arduino-nrf24l01p") {
            EXPECT_EQ(d.at("radio").at("type"), "nrf24");
            EXPECT_FALSE(d.at("limits").at("tx_power_levels").empty());
            EXPECT_EQ(d.at("mcu"), "arduino-nano");
        }
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(hw.at("mcus").size(), 5u);
    ASSERT_EQ(hw.at("modules").size(), 4u);
    for (const json &m : hw.at("modules")) {
        if (m.at("name") == "nrf24l01p-pa-lna") {
            EXPECT_EQ(m.at("antenna_dbi"), 2);
            EXPECT_EQ(m.at("lna_db"), 3);
            EXPECT_EQ(m.at("current_ma").at("tx"), 115);
            EXPECT_EQ(m.at("pins").size(), 8u);
            EXPECT_EQ(m.at("supply_v").at("max"), 3.6);
        }
    }

    const json phys = body_json(call(api, "GET", "/api/phys"));
    EXPECT_EQ(phys.at("lora").at("version"), 1);
    EXPECT_EQ(phys.at("nrf24").at("options").size(), 4u);
    for (const json &o : phys.at("lora").at("options")) {
        EXPECT_TRUE(o.contains("default")) << o.dump();
    }
}

TEST(ServeApi, Wiring)
{
    const Api api(options());
    json w = body_json(call(api, "GET", "/api/wiring/rpi-pico/nrf24l01p"));
    EXPECT_EQ(w.at("integrated"), false);
    ASSERT_EQ(w.at("rows").size(), 8u);
    EXPECT_EQ(w.at("rows").at(0).at("module_pin"), "VCC");
    EXPECT_NE(w.at("rows").at(0).at("mcu_pin").get<std::string>().find("3V3"), std::string::npos);
    EXPECT_TRUE(w.at("warnings").empty());

    w = body_json(call(api, "GET", "/api/wiring/arduino-nano/nrf24l01p-pa-lna"));
    EXPECT_FALSE(w.at("warnings").empty());

    w = body_json(call(api, "GET", "/api/wiring/esp32-sx1276")); // radio on the board
    EXPECT_EQ(w.at("integrated"), true);
    EXPECT_TRUE(w.at("rows").empty());
    w = body_json(call(api, "GET", "/api/wiring/arduino-nrf24l01p")); // board of parts
    EXPECT_EQ(w.at("rows").size(), 8u);

    EXPECT_EQ(call(api, "GET", "/api/wiring/nope").status, 404);
    EXPECT_EQ(call(api, "GET", "/api/wiring/rpi-pico/nope").status, 404);
    EXPECT_EQ(call(api, "GET", "/api/wiring/rpi-pico/a/b").status, 400);
    EXPECT_EQ(call(api, "PUT", "/api/wiring/esp32-sx1276").status, 405);
}

TEST(ServeApi, EveryBuiltinStackHasAValidSchema)
{
    const Api api(options());
    const json stacks = body_json(call(api, "GET", "/api/stacks"));
    ASSERT_EQ(stacks.size(), 2u);
    for (const json &s : stacks) {
        SCOPED_TRACE(s.at("name").get<std::string>());
        ASSERT_TRUE(s.at("schema").is_object()) << s.dump();
        EXPECT_EQ(s.at("schema").at("phy").size(), 1u);
        for (const json &o : s.at("schema").at("options")) {
            const std::string type = o.at("type");
            EXPECT_TRUE(type == "int" || type == "bool" || type == "string" || type == "enum");
            EXPECT_FALSE(o.at("help").get<std::string>().empty());
            if (type == "enum") {
                EXPECT_FALSE(o.at("values").empty());
            }
            if (o.contains("format")) {
                EXPECT_EQ(o.at("format"), "hex");
                EXPECT_EQ(type, "int");
            }
        }
    }
}

// Spelling out every default of a schema must not change a run: the schema tells the truth.
TEST(ServeApi, SchemaDefaultsAreTheStackDefaults)
{
    const Api api(options());
    const json stacks = body_json(call(api, "GET", "/api/stacks"));
    for (const json &s : stacks) {
        const std::string name = s.at("name");
        SCOPED_TRACE(name);
        const bool lora = s.at("schema").at("phy").at(0) == "lora";
        json scn = json::parse(read_text(kScenarios + (lora ? "/two_nodes.json" : "/nrf24_star.json")));
        scn["duration_s"] = 120;
        const std::string plain = scn.dump();

        for (json &n : scn.at("nodes")) {
            json cfg = schema_defaults(s.at("schema"));
            if (n.contains("config")) {
                for (const auto &item : n.at("config").items()) {
                    cfg[item.key()] = item.value(); // the scenario's own settings win
                }
            }
            n["config"] = cfg;
        }
        EXPECT_EQ(direct_hash(plain), direct_hash(scn.dump()));
    }
}

TEST(ServeApi, RunReturnsSummaryLinksAndTrace)
{
    const Api api(options());
    std::string text = read_text(kScenarios + "/line_5.json");
    const Response r = call(api, "POST", "/api/run", text);
    ASSERT_EQ(r.status, 200) << r.body;
    const json out = body_json(r);

    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx",
                  static_cast<unsigned long long>(direct_hash(text)));
    EXPECT_EQ(out.at("summary").at("trace_hash"), hash); // same run as `ksim run`
    EXPECT_EQ(out.at("summary").at("nodes").size(), 5u);
    EXPECT_EQ(out.at("truncated"), false);

    const json &trace = out.at("trace");
    ASSERT_GT(trace.size(), 100u);
    EXPECT_EQ(trace.at(0).at("ev"), "meta");
    EXPECT_EQ(trace.at(0).at("abi"), KSIM_ABI_VERSION);

    // neighbours 1 km apart hear each other, nodes 4 km apart do not
    bool one_two = false;
    for (const json &l : out.at("links")) {
        EXPECT_NE(std::abs(l.at("from").get<int>() - l.at("to").get<int>()), 0);
        one_two = one_two || (l.at("from") == 1 && l.at("to") == 2);
        EXPECT_FALSE(l.at("from") == 1 && l.at("to") == 5) << l.dump();
    }
    EXPECT_TRUE(one_two);
}

TEST(ServeApi, LongTracesAreCutAtALineBoundary)
{
    Options o = options();
    o.max_trace_bytes = 2000;
    const Api api(o);
    const Response r = call(api, "POST", "/api/run", read_text(kScenarios + "/line_5.json"));
    ASSERT_EQ(r.status, 200) << r.body;
    const json out = body_json(r); // still valid JSON
    EXPECT_EQ(out.at("truncated"), true);
    EXPECT_GT(out.at("trace").size(), 3u);
    EXPECT_LT(out.at("trace").size(), 30u);
    EXPECT_GT(out.at("summary").at("tx"), 100u); // the summary covers the whole run

    o.max_trace_bytes = 10;
    const json tiny = body_json(call(Api(o), "POST", "/api/run",
                                     read_text(kScenarios + "/two_nodes.json")));
    EXPECT_TRUE(tiny.at("trace").empty());
}

TEST(ServeApi, CheckValidatesAndReportsLinks)
{
    const Api api(options());
    const Response ok = call(api, "POST", "/api/check", read_text(kScenarios + "/two_nodes.json"));
    ASSERT_EQ(ok.status, 200) << ok.body;
    EXPECT_EQ(body_json(ok).at("links").size(), 2u); // both directions

    const std::string unknown_stack =
        R"({"nodes":[{"id":1,"pos":[0,0],"stack":"nope"}]})";
    Response r = call(api, "POST", "/api/check", unknown_stack);
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(body_json(r).at("error").get<std::string>().find("unknown stack"), std::string::npos);

    r = call(api, "POST", "/api/check", "{not json");
    EXPECT_EQ(r.status, 400);
    r = call(api, "POST", "/api/check", R"({"nodes":[{"id":1,"pos":[0,0],"stack":"kervan","bogus":1}]})");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("bogus"), std::string::npos);
    r = call(api, "POST", "/api/check",
             R"({"mtu":0,"nodes":[{"id":1,"pos":[0,0],"stack":"kervan"}]})");
    EXPECT_EQ(r.status, 400); // rejected by the simulator, not the parser
}

TEST(ServeApi, RunErrors)
{
    const Api api(options());
    // stack rejects its config at create time
    Response r = call(api, "POST", "/api/run",
                      R"({"nodes":[{"id":1,"pos":[0,0],"stack":"kervan","config":{"len":999}}]})");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("rejected config"), std::string::npos);

    r = call(api, "POST", "/api/run", "[]");
    EXPECT_EQ(r.status, 400);

    const std::string spawn = R"({"nodes":[{"id":1,"pos":[0,0],"stack":"kervan",
        "process":{"spawn":["ksim-node"]}}]})";
    r = call(api, "POST", "/api/run", spawn);
    EXPECT_EQ(r.status, 403);
    EXPECT_NE(r.body.find("--allow-processes"), std::string::npos);

    const std::string wait = R"({"nodes":[{"id":1,"pos":[0,0],"stack":"kervan",
        "process":{"wait":true}}]})";
    Options o = options();
    o.allow_processes = true;
    r = call(Api(o), "POST", "/api/run", wait);
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("ksim run"), std::string::npos);
}

TEST(ServeApi, ScenarioFiles)
{
    const fs::path dir = temp_dir("files");
    const Api api(options(dir.string()));

    EXPECT_TRUE(body_json(call(api, "GET", "/api/scenarios")).empty());
    EXPECT_EQ(call(api, "GET", "/api/scenarios/missing").status, 404);

    const std::string text = read_text(kScenarios + "/two_nodes.json");
    Response r = call(api, "PUT", "/api/scenarios/my_test-1", text);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(call(api, "GET", "/api/scenarios/my_test-1").body, text);

    // invalid scenarios are not saved
    r = call(api, "PUT", "/api/scenarios/broken", R"({"nodes": []})");
    EXPECT_EQ(r.status, 400);
    EXPECT_FALSE(fs::exists(dir / "broken.json"));

    // names cannot leave the directory
    for (const char *bad : {"../evil", "a/b", "a.b", "", "x\\y"}) {
        EXPECT_EQ(call(api, "PUT", std::string("/api/scenarios/") + bad, text).status, 400) << bad;
    }

    // a file that is not a valid scenario still shows up, with its problem
    std::ofstream(dir / "junk.json") << "{oops";
    std::ofstream(dir / "notes.txt") << "ignored";
    const json list = body_json(call(api, "GET", "/api/scenarios"));
    ASSERT_EQ(list.size(), 2u);
    EXPECT_TRUE(list[0].contains("error")); // junk
    EXPECT_EQ(list[1].at("name"), "my_test-1");
    EXPECT_EQ(list[1].at("nodes"), 2);
    EXPECT_FALSE(list[1].at("description").get<std::string>().empty());

    // without a scenario directory there is nothing to list or save
    const Api none(options(""));
    EXPECT_TRUE(body_json(call(none, "GET", "/api/scenarios")).empty());
    EXPECT_EQ(call(none, "PUT", "/api/scenarios/x", text).status, 404);
    EXPECT_EQ(call(none, "GET", "/api/scenarios/x").status, 404);
    fs::remove_all(dir);
}

TEST(ServeApi, Routing)
{
    const Api api(options());
    EXPECT_EQ(call(api, "GET", "/nope").status, 404);
    EXPECT_EQ(call(api, "GET", "/api/nope").status, 404);
    EXPECT_EQ(call(api, "POST", "/api/devices").status, 405);
    EXPECT_EQ(call(api, "GET", "/api/run").status, 405);
    EXPECT_EQ(call(api, "POST", "/api/scenarios/x").status, 405);
}

TEST(ServeHttp, ServesTheApiAndGuardsIt)
{
    const fs::path web = temp_dir("web");
    std::ofstream(web / "index.html") << "<html>kervan ui</html>";

    const Api api(options());
    const RunningServer server(api, web.string());
    const uint16_t port = server.port();

    httplib::Client client("127.0.0.1", port);

    auto res = client.Get("/api/info");
    ASSERT_TRUE(res) << httplib::to_string(res.error());
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(json::parse(res->body).at("abi"), KSIM_ABI_VERSION);

    res = client.Get("/");
    ASSERT_TRUE(res);
    EXPECT_NE(res->body.find("kervan ui"), std::string::npos);

    const std::string scenario = read_text(kScenarios + "/two_nodes.json");
    res = client.Post("/api/check", scenario, "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    // a form post from another site cannot reach the API
    res = client.Post("/api/run", scenario, "text/plain");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 415);

    res = client.Post("/api/run", httplib::Headers{{"Origin", "http://evil.example"}}, scenario,
                      "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);

    res = client.Get("/api/info", httplib::Headers{{"Origin", "http://localhost:" +
                                                                  std::to_string(port)}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    // DNS rebinding: a foreign host name pointing at 127.0.0.1
    res = client.Get("/api/info", httplib::Headers{{"Host", "evil.example:" + std::to_string(port)}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    fs::remove_all(web);
}

TEST(ServeHttp, ExplainsAMissingWebBuild)
{
    const Api api(options());
    const RunningServer server(api, "");
    const uint16_t port = server.port();

    httplib::Client client("127.0.0.1", port);
    auto res = client.Get("/");
    ASSERT_TRUE(res);
    EXPECT_NE(res->body.find("npm run build"), std::string::npos);

    // the port is taken now
    HttpServer second(api, "");
    EXPECT_THROW(second.bind(port), std::runtime_error);
}

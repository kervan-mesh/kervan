/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Scenario payloads (fixed bytes, Python functions), frame bytes in the trace, and the stacks'
// frame decoders.

#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api.hpp"
#include "ksim/frame_fields.h"
#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"

using namespace ksim;
using nlohmann::json;

namespace {

struct Result {
    Summary sum;
    std::vector<json> trace;
};

Result run_json(const std::string &scenario)
{
    Simulator sim(parse_scenario(scenario));
    sim.register_stack(ksim_stack_kervan());
    sim.register_stack(ksim_stack_nrf24_esb());
    std::ostringstream out;
    sim.set_trace(&out);
    Result r;
    r.sum = sim.run();
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);) {
        r.trace.push_back(json::parse(line));
    }
    return r;
}

// Frames node `id` sent, as hex.
std::vector<std::string> sent(const Result &r, int id)
{
    std::vector<std::string> out;
    for (const json &j : r.trace) {
        if (j.at("ev") == "tx" && j.at("node") == id) {
            out.push_back(j.at("data").get<std::string>());
        }
    }
    return out;
}

std::vector<std::string> app_details(const Result &r, const std::string &name)
{
    std::vector<std::string> out;
    for (const json &j : r.trace) {
        if (j.at("ev") == "app" && j.at("name") == name) {
            out.push_back(j.at("detail").get<std::string>());
        }
    }
    return out;
}

bool ends_with(const std::string &s, const std::string &tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Two Kervan nodes 300 m apart; node 1 sends every 10 s for 35 s. node1_extra goes into node 1's
// object, config_extra into its config.
std::string kervan_pair(const std::string &node1_extra, const std::string &config_extra = "")
{
    return R"({"duration_s": 35, "mtu": 51, "nodes": [
        {"id": 1, "pos": [0, 0], "stack": "kervan",
         "config": {"send_to": 2, "send_every_ms": 10000, "duty_permille": 0)" +
           config_extra + "}" + node1_extra + R"(},
        {"id": 2, "pos": [300, 0], "stack": "kervan"}]})";
}

std::string esb_pair(const std::string &node2_extra, const std::string &rx_config = "")
{
    return R"({"duration_s": 10, "nodes": [
        {"id": 1, "pos": [0, 0], "device": "pico-nrf24l01p", "stack": "nrf24-esb",
         "config": {)" + rx_config + R"(}},
        {"id": 2, "pos": [5, 0], "device": "pico-nrf24l01p", "stack": "nrf24-esb",
         "config": {"send_to": "0xE7E7E7E701", "send_every_ms": 2000, "listen": 0})" +
           node2_extra + R"(}]})";
}

void expect_parse_error(const std::string &payload, const std::string &part)
{
    SCOPED_TRACE(payload);
    try {
        parse_scenario(kervan_pair(", \"payload\": " + payload));
        FAIL() << "accepted";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find(part), std::string::npos) << e.what();
    }
}

#if defined(KSIM_PYTHON)
void use_test_python()
{
#if defined(_WIN32)
    _putenv_s("KSIM_PYTHON", KSIM_PYTHON);
#else
    setenv("KSIM_PYTHON", KSIM_PYTHON, 1);
#endif
}
#endif

} // namespace

TEST(Payload, ScenarioHexForms)
{
    for (const char *form : {R"("01 A2 ff")", R"("0x01a2FF")", R"("  01a2 ff ")"}) {
        const Scenario s = parse_scenario(kervan_pair(R"(, "payload": {"hex": )" +
                                                      std::string(form) + "}"));
        ASSERT_TRUE(s.nodes[0].payload.has_value()) << form;
        EXPECT_EQ(s.nodes[0].payload->bytes, (std::vector<uint8_t>{0x01, 0xA2, 0xFF})) << form;
        EXPECT_FALSE(s.nodes[1].payload.has_value());
    }
    EXPECT_TRUE(parse_scenario(kervan_pair(R"(, "payload": {"hex": ""})")).nodes[0].payload->bytes
                    .empty());

    expect_parse_error(R"({"hex": "012"})", "even number");
    expect_parse_error(R"({"hex": "01 zz"})", "not a hex digit");
    expect_parse_error(R"({"hex": "01", "python": "def payload(n, s, t): return b''"})",
                       "either");
    expect_parse_error(R"({})", "either");
    expect_parse_error(R"({"python": "x = 1"})", "must define payload");
    expect_parse_error(R"({"hex": "01", "bogus": 1})", "bogus");
    expect_parse_error(R"({"hex": ")" + std::string(2050, 'a') + R"("})", "1024");
}

TEST(Payload, TraceCarriesFrameBytes)
{
    const Result r = run_json(kervan_pair(""));
    const auto frames = sent(r, 1);
    ASSERT_GE(frames.size(), 3u);
    // 8-byte header + topic + the stack's own 10 counter bytes 00 01 02 ...
    EXPECT_EQ(frames[0].size(), 2u * (8 + 2 + 10));
    EXPECT_TRUE(ends_with(frames[0], "00010203040506070809")) << frames[0];
    EXPECT_EQ(r.trace.at(0).at("format"), 2);
}

TEST(Payload, FixedBytesReplaceTheStackData)
{
    const Result r = run_json(kervan_pair(R"(, "payload": {"hex": "CA FE 00 42"})"));
    for (const std::string &f : sent(r, 1)) {
        EXPECT_TRUE(ends_with(f, "cafe0042")) << f;
        EXPECT_EQ(f.size(), 2u * (8 + 2 + 4));
    }
    const auto tx = app_details(r, "app_tx");
    ASSERT_FALSE(tx.empty());
    EXPECT_NE(tx[0].find("len=4"), std::string::npos) << tx[0];
    const auto rx = app_details(r, "app_rx");
    ASSERT_FALSE(rx.empty());
    EXPECT_NE(rx[0].find("len=4"), std::string::npos) << rx[0];

    // nRF24: the payload follows address + PCF; the default test data (node id first) is gone
    const Result e = run_json(esb_pair(R"(, "payload": {"hex": "11 22 33"})"));
    const auto esb = sent(e, 2);
    ASSERT_FALSE(esb.empty());
    EXPECT_TRUE(ends_with(esb[0], "112233")) << esb[0];
    EXPECT_EQ(esb[0].size(), 2u * (5 + 2 + 3));
    EXPECT_FALSE(app_details(e, "esb_rx").empty());
}

TEST(Payload, TooLongForTheStackStopsTheRun)
{
    try {
        run_json(esb_pair(R"(, "payload": {"hex": ")" + std::string(66, '1') + R"("})"));
        FAIL() << "ran";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find("payload of 33 bytes"), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("at most 32"), std::string::npos) << e.what();
    }
}

TEST(Payload, NotForOutOfProcessNodes)
{
    Scenario s = parse_scenario(kervan_pair(R"(, "payload": {"hex": "01"})"));
    s.nodes[0].process = ProcessSpec{{"ksim-node"}, false, 0.0};
    EXPECT_THROW(Simulator{s}, std::invalid_argument);
}

#if defined(KSIM_PYTHON)
TEST(Payload, PythonFunction)
{
    use_test_python();
    const std::string code =
        R"(def payload(node, seq, t):\n    return bytes([node, seq, int(t)]) + struct.pack('<H', rng.randrange(65536)))";
    const std::string scn = kervan_pair(R"(, "payload": {"python": ")" + code + R"("})");
    const Result a = run_json(scn);
    const auto frames = sent(a, 1);
    ASSERT_GE(frames.size(), 3u);
    // node 1, message 0 at the first send (a random start within the first 10 s)
    EXPECT_EQ(frames[0].substr(2 * 10, 4), "0100") << frames[0];
    EXPECT_EQ(frames[1].substr(2 * 10, 4), "0101") << frames[1];
    EXPECT_EQ(frames[0].size(), 2u * (8 + 2 + 5));

    const Result b = run_json(scn); // seeded rng: the same bytes every time
    EXPECT_EQ(a.sum.trace_hash, b.sum.trace_hash);
}

TEST(Payload, PythonErrorsStopTheRun)
{
    use_test_python();
    const auto expect_error = [](const std::string &code, const std::string &part) {
        SCOPED_TRACE(code);
        try {
            run_json(kervan_pair(R"(, "payload": {"python": ")" + code + R"("})"));
            FAIL() << "ran";
        } catch (const std::runtime_error &e) {
            EXPECT_NE(std::string(e.what()).find(part), std::string::npos) << e.what();
        }
    };
    expect_error(R"(def payload(node, seq, t):\n    return 1 / 0)", "ZeroDivisionError");
    expect_error(R"(def payload(node, seq, t):\n    return 1 / 0)", "(line 2)");
    expect_error(R"(def payload(node, seq, t):\n    return 3.5)", "must return bytes");
    expect_error(R"(def payload(node, seq, t) oops)", "SyntaxError");
    expect_error(R"(payload = 5  # def payload)", "defines no function");
}
#endif

TEST(Payload, FrameFieldsHelper)
{
    char out[128];
    ksim_fields_t f;
    ksim_fields_begin(&f, out, sizeof(out));
    ksim_field(&f, "a\"b", "x\\y", 1, 2);
    ksim_field(&f, "c", "d", -1, 0);
    const size_t n = ksim_fields_end(&f);
    ASSERT_GT(n, 0u);
    const json j = json::parse(std::string(out, n));
    EXPECT_EQ(j.at(0).at("name"), "a\"b");
    EXPECT_EQ(j.at(0).at("value"), "x\\y");
    EXPECT_EQ(j.at(0).at("offset"), 1);
    EXPECT_FALSE(j.at(1).contains("offset"));

    char tiny[8];
    ksim_fields_begin(&f, tiny, sizeof(tiny));
    ksim_field(&f, "long name", "value", 0, 1);
    EXPECT_EQ(ksim_fields_end(&f), 0u);
    EXPECT_EQ(tiny[0], '\0');
}

namespace {

json describe(const ksim_stack_t *stack, const std::string &config, const std::string &hex)
{
    std::vector<uint8_t> frame;
    for (size_t i = 0; i < hex.size(); i += 2) {
        frame.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    char out[4096];
    const size_t n = stack->describe_frame(config.c_str(), frame.data(), frame.size(), out,
                                           sizeof(out));
    return n == 0 ? json() : json::parse(std::string(out, n));
}

std::string field(const json &fields, const std::string &name)
{
    for (const json &f : fields) {
        if (f.at("name") == name) {
            return f.at("value").get<std::string>();
        }
    }
    return "<missing>";
}

} // namespace

TEST(Payload, KervanDecoder)
{
    const Result r = run_json(kervan_pair("", R"(, "qos": 1)"));
    const auto data = sent(r, 1);
    ASSERT_FALSE(data.empty());
    const json d = describe(ksim_stack_kervan(), "send_to=2", data[0]);
    ASSERT_TRUE(d.is_array());
    EXPECT_EQ(field(d, "type"), "DATA (0)");
    EXPECT_EQ(field(d, "src"), "1");
    EXPECT_EQ(field(d, "dst"), "2");
    EXPECT_EQ(field(d, "ack_req"), "yes");
    EXPECT_NE(field(d, "topic").find("(kervan/data)"), std::string::npos);
    EXPECT_EQ(field(d, "data"), "10 bytes");
    for (const json &f : d) {
        if (f.contains("offset")) {
            EXPECT_LE(f.at("offset").get<size_t>() + f.at("len").get<size_t>(), data[0].size() / 2);
        }
    }
    // a topic the sender's config names
    EXPECT_EQ(field(describe(ksim_stack_kervan(), "topic=other", data[0]), "topic").find("other"),
              std::string::npos);

    // node 2's ACK
    const auto acks = sent(r, 2);
    ASSERT_FALSE(acks.empty());
    const json a = describe(ksim_stack_kervan(), "", acks[0]);
    EXPECT_EQ(field(a, "type"), "ACK (1)");
    EXPECT_EQ(field(a, "acked_seq"), field(d, "seq"));

    EXPECT_TRUE(describe(ksim_stack_kervan(), "", "ff").is_null()); // not a Kervan frame
    char small[16];
    std::vector<uint8_t> raw(data[0].size() / 2);
    for (size_t i = 0; i < raw.size(); i++) {
        raw[i] = static_cast<uint8_t>(std::stoul(data[0].substr(2 * i, 2), nullptr, 16));
    }
    EXPECT_EQ(ksim_stack_kervan()->describe_frame("", raw.data(), raw.size(), small, sizeof(small)),
              0u);
}

TEST(Payload, EsbDecoder)
{
    const Result r = run_json(esb_pair(""));
    const auto data = sent(r, 2);
    ASSERT_FALSE(data.empty());
    const json d = describe(ksim_stack_nrf24_esb(), "", data[0]);
    EXPECT_EQ(field(d, "address"), "0xE7E7E7E701");
    EXPECT_EQ(field(d, "payload_length"), "8");
    EXPECT_EQ(field(d, "no_ack"), "no");
    EXPECT_EQ(field(d, "payload"), "8 bytes");

    // 3-byte addresses: the sender's config says how to split the frame
    const json d3 = describe(ksim_stack_nrf24_esb(), "aw=3;pipes=2", "c2c2c2" "0c" "01" "aabbcc");
    EXPECT_EQ(field(d3, "address"), "0xC2C2C2");
    EXPECT_EQ(field(d3, "payload_length"), "3");
    EXPECT_EQ(field(d3, "pid"), "0");
    EXPECT_EQ(field(d3, "no_ack"), "yes");
    EXPECT_EQ(field(describe(ksim_stack_nrf24_esb(), "aw=3", "c2c2c2" "10" "00" "aa"),
                    "payload_length"),
              "4 (frame has 1)");
    EXPECT_TRUE(describe(ksim_stack_nrf24_esb(), "", "0102").is_null()); // too short
}

TEST(Payload, ApiDecodesAndGatesPython)
{
    static const ksim_stack_t no_decoder = {KSIM_ABI_VERSION, "plain", "1", nullptr, nullptr,
                                            nullptr, nullptr, nullptr, nullptr, nullptr};
    serve::Options o;
    o.stacks = {ksim_stack_kervan(), ksim_stack_nrf24_esb(), &no_decoder};
    const serve::Api api(o);
    const auto post = [&](const std::string &path, const std::string &body) {
        return api.handle(serve::Request{"POST", path, body});
    };

    serve::Response r = post("/api/decode", R"({"stack": "nrf24-esb", "config": {"aw": 3},
                                               "data": "C2C2C20C01AABBCC"})");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).at("fields").at(0).at("value"), "0xC2C2C2");

    EXPECT_TRUE(json::parse(post("/api/decode", R"({"stack": "plain", "data": "01"})").body)
                    .at("fields")
                    .is_null());
    EXPECT_TRUE(json::parse(post("/api/decode", R"({"stack": "kervan", "data": "ff"})").body)
                    .at("fields")
                    .is_null());
    EXPECT_EQ(post("/api/decode", R"({"stack": "nope", "data": "01"})").status, 404);
    EXPECT_EQ(post("/api/decode", R"({"stack": "kervan", "data": "0g"})").status, 400);
    EXPECT_EQ(post("/api/decode", R"({"stack": "kervan", "data": "012"})").status, 400);
    EXPECT_EQ(post("/api/decode", R"({"stack": "kervan"})").status, 400);
    EXPECT_EQ(post("/api/decode", "nope").status, 400);
    EXPECT_EQ(post("/api/decode", R"({"stack": "kervan", "data": "01", "config": 5})").status,
              400);
    EXPECT_EQ(api.handle(serve::Request{"GET", "/api/decode", ""}).status, 405);

    const std::string python = kervan_pair(
        R"(, "payload": {"python": "def payload(node, seq, t):\n    return b'x'"})");
    r = post("/api/run", python);
    EXPECT_EQ(r.status, 403);
    EXPECT_NE(r.body.find("--allow-processes"), std::string::npos);
}

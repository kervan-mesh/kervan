/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Board firmware builds for a wiring (docs/hardware.md): what is built, the Docker command, the
// builder (with small commands instead of Docker), and the API around it.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "api.hpp"
#include "firmware.hpp"
#include "ksim/scenario.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"

using namespace ksim;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const DeviceRegistry &catalog()
{
    static const DeviceRegistry reg = DeviceRegistry::builtin();
    return reg;
}

// A scratch directory that is removed again.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string &name)
        : path(fs::temp_directory_path() / ("ksim-test-" + name))
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

hw::FirmwareBuilder::Status wait(const hw::FirmwareBuilder &b)
{
    for (int i = 0; i < 300; i++) {
        const auto s = b.status();
        if (s.state != "running") {
            return s;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return b.status();
}

} // namespace

TEST(Firmware, SpecFromTheWiring)
{
    hw::FirmwareSpec s = hw::firmware_spec(catalog(), "rpi-pico", "nrf24l01p", {});
    EXPECT_EQ(s.project, "pico_nrf24");
    EXPECT_EQ(s.board, "pico");
    EXPECT_EQ(s.name(), "kervan_nrf24-rpi-pico-spi0-sck18-mosi19-miso16-csn17-ce20");
    EXPECT_EQ(s.wiring(), "SPI0 SCK=GP18 MOSI=GP19 MISO=GP16 CSN=GP17 CE=GP20");
    EXPECT_EQ(s.cmake_args(),
              (std::vector<std::string>{"-DPICO_BOARD=pico", "-DKFW_SPI=0", "-DKFW_PIN_SCK=18",
                                        "-DKFW_PIN_MOSI=19", "-DKFW_PIN_MISO=16",
                                        "-DKFW_PIN_CSN=17", "-DKFW_PIN_CE=20"}));

    s = hw::firmware_spec(catalog(), "rpi-pico-w", "nrf24l01p-pa-lna",
                          {{"SCK", "GP10"}, {"MOSI", "GP11"}, {"MISO", "GP12"}, {"CE", "GP14"}});
    EXPECT_EQ(s.board, "pico_w");
    EXPECT_EQ(s.wiring(), "SPI1 SCK=GP10 MOSI=GP11 MISO=GP12 CSN=GP17 CE=GP14");

    const auto error = [](const std::string &mcu, const std::string &module) {
        try {
            hw::firmware_spec(catalog(), mcu, module, {});
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_EQ(error("esp32", "nrf24l01p"),
              "there is no Kervan board firmware for ESP32 yet (only Raspberry Pi Pico and "
              "Pico W)");
    EXPECT_NE(error("rpi-pico", "sx1276").find("drives nRF24 modules, not"), std::string::npos);
    EXPECT_EQ(error("nope", "nrf24l01p"), "unknown MCU 'nope'");
    EXPECT_EQ(error("rpi-pico", "nope"), "unknown module 'nope'");
}

TEST(Firmware, DockerCommand)
{
    const hw::FirmwareSpec s = hw::firmware_spec(catalog(), "rpi-pico", "nrf24l01p", {});
    const auto argv = hw::docker_build_command(s, "C:/src/kervan");
    ASSERT_GE(argv.size(), 10u);
    EXPECT_EQ(argv[0], "docker");
    EXPECT_EQ(argv[4], "type=bind,source=C:/src/kervan,target=/workspaces/kervan");
    EXPECT_EQ(argv[7], "kervan-dev");
    const std::string &script = argv.back();
    EXPECT_NE(script.find("cmake -S firmware/pico_nrf24 -B /build/fw/" + s.name()),
              std::string::npos);
    EXPECT_NE(script.find("-DKFW_PIN_CE=20"), std::string::npos);
    EXPECT_NE(script.find("cp /build/fw/" + s.name() + "/kervan_nrf24.uf2 build/uf2/" + s.name() +
                          ".uf2"),
              std::string::npos);
    EXPECT_EQ(script.find('"'), std::string::npos); // nothing to quote inside
}

TEST(Firmware, Builder)
{
    TempDir dir("builder");
    const hw::FirmwareSpec spec = hw::firmware_spec(catalog(), "rpi-pico", "nrf24l01p", {});
    const std::string uf2 = (dir.path / "build" / "uf2" / (spec.name() + ".uf2")).string();

    // a "build" that makes the file and says something
    hw::FirmwareBuilder::Options opt;
    opt.source_dir = dir.path.string();
    opt.log_file = (dir.path / "build.log").string();
    std::string mode = "ok";
    opt.command = [&](const hw::FirmwareSpec &) -> std::vector<std::string> {
        if (mode == "ok") {
            return {KSIM_CMAKE, "-E", "touch", uf2};
        }
        if (mode == "slow") {
            return {KSIM_CMAKE, "-E", "sleep", "1"};
        }
        if (mode == "missing") {
            return {"ksim-no-such-program"};
        }
        return {KSIM_CMAKE, "-E", "echo", "Cannot connect to the Docker daemon at unix:///x"};
    };
    hw::FirmwareBuilder b(opt);
    EXPECT_EQ(b.status().state, "idle");
    ASSERT_TRUE(b.start(spec));
    auto s = wait(b);
    EXPECT_EQ(s.state, "done") << s.error;
    EXPECT_EQ(fs::path(s.file), fs::path(uf2).lexically_normal().make_preferred());
    EXPECT_EQ(s.name, spec.name());
    EXPECT_EQ(s.wiring, spec.wiring());
    ASSERT_FALSE(s.log.empty());
    EXPECT_EQ(s.log[0].rfind("$ ", 0), 0u); // the command comes first
    EXPECT_EQ(b.out_dir(), (dir.path / "build" / "uf2").string());

    // the output is kept, and a failure is explained
    fs::remove(uf2);
    mode = "docker down";
    ASSERT_TRUE(b.start(spec));
    s = wait(b);
    EXPECT_EQ(s.state, "failed");
    EXPECT_EQ(s.error, "Docker is not running: start Docker Desktop and build again");
    EXPECT_NE(s.log.back().find("Cannot connect to the Docker daemon"), std::string::npos);

    // exit code 0 but no file
    mode = "slow";
    ASSERT_TRUE(b.start(spec));
    EXPECT_FALSE(b.start(spec)); // one at a time
    s = wait(b);
    EXPECT_EQ(s.state, "failed");
    EXPECT_EQ(s.error, "the build failed (exit code 0): see the log");

    mode = "missing";
    ASSERT_TRUE(b.start(spec));
    s = wait(b);
    EXPECT_EQ(s.state, "failed");
}

TEST(Firmware, StopsWithTheBuilder)
{
    TempDir dir("stop");
    hw::FirmwareBuilder::Options opt;
    opt.source_dir = dir.path.string();
    opt.log_file = (dir.path / "build.log").string();
    opt.command = [](const hw::FirmwareSpec &) -> std::vector<std::string> {
        return {KSIM_CMAKE, "-E", "sleep", "30"};
    };
    const auto t0 = std::chrono::steady_clock::now();
    {
        hw::FirmwareBuilder b(opt);
        ASSERT_TRUE(b.start(hw::firmware_spec(catalog(), "rpi-pico", "nrf24l01p", {})));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } // the destructor kills the command
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
}

TEST(Firmware, ScenarioPins)
{
    const auto error = [](const std::string &node) {
        try {
            parse_scenario(R"({"nodes": [)" + node + "]}");
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    const std::string pico =
        R"("id": 1, "pos": [0, 0], "mcu": "rpi-pico", "module": "nrf24l01p", "stack": "nrf24-esb")";
    EXPECT_EQ(error("{" + pico + R"(, "pins": {"CE": "GP15"}})"), "no error");
    EXPECT_EQ(parse_scenario(R"({"nodes": [{)" + pico + R"(, "pins": {"CE": "GP15"}}]})")
                  .nodes[0]
                  .pins.at("CE"),
              "GP15");
    EXPECT_NE(error("{" + pico + R"(, "pins": {"CE": "GP17"}})")
                  .find("scenario.nodes[0].pins: GP17 is wired to both CE and CSN"),
              std::string::npos);
    EXPECT_NE(error("{" + pico + R"(, "pins": ["CE"]})").find("must be an object"),
              std::string::npos);
    EXPECT_NE(error(R"({"id": 1, "pos": [0, 0], "module": "nrf24l01p", "stack": "nrf24-esb",
                       "pins": {"CE": "GP15"}})")
                  .find("needs an MCU and a radio module"),
              std::string::npos);
    // a board that is an MCU + module
    EXPECT_NE(error(R"({"id": 1, "pos": [0, 0], "device": "arduino-nrf24l01p",
                       "stack": "nrf24-esb", "pins": {"CE": "D9"}})")
                  .find("needs an MCU whose pin header is in the catalog"),
              std::string::npos);
}

TEST(Firmware, Api)
{
    TempDir dir("api");
    TempDir drive("drive");
    const std::vector<const ksim_stack_t *> stacks = {ksim_stack_kervan(), ksim_stack_nrf24_esb()};
    serve::Options opt;
    opt.stacks = stacks;
    hw::FirmwareBuilder::Options build;
    build.source_dir = dir.path.string();
    build.log_file = (dir.path / "build.log").string();
    build.command = [&](const hw::FirmwareSpec &spec) -> std::vector<std::string> {
        return {KSIM_CMAKE, "-E", "touch",
                (dir.path / "build" / "uf2" / (spec.name() + ".uf2")).string()};
    };
    opt.builder = std::make_shared<hw::FirmwareBuilder>(build);
    std::vector<std::string> drives;
    opt.list_drives = [&] { return drives; };

    const auto call = [&](const serve::Api &api, const std::string &method, const std::string &path,
                          const std::string &body = "") {
        return api.handle({method, path, body});
    };

    {
        const serve::Api locked(opt); // without --allow-processes
        const auto r = call(locked, "POST", "/api/firmware/build", "{}");
        EXPECT_EQ(r.status, 403);
        EXPECT_NE(r.body.find("--allow-processes"), std::string::npos);
        EXPECT_EQ(call(locked, "GET", "/api/firmware/build").status, 200);
    }
    opt.allow_processes = true;
    const serve::Api api(opt);

    // the catalog carries the headers
    json devices = json::parse(call(api, "GET", "/api/devices").body);
    for (const json &m : devices.at("mcus")) {
        if (m.at("name") == "rpi-pico") {
            EXPECT_EQ(m.at("header").at("right")[4], "3V3(OUT)");
            EXPECT_EQ(m.at("spi").size(), 2u);
            EXPECT_EQ(m.at("firmware").at("board"), "pico");
        }
    }

    // wiring with chosen pins
    serve::Response r = call(api, "POST", "/api/wiring",
                             R"({"mcu":"rpi-pico","module":"nrf24l01p","pins":{"CE":"GP15"}})");
    ASSERT_EQ(r.status, 200) << r.body;
    json w = json::parse(r.body);
    EXPECT_EQ(w.at("spi"), "SPI0");
    EXPECT_EQ(w.at("firmware").at("wiring"), "SPI0 SCK=GP18 MOSI=GP19 MISO=GP16 CSN=GP17 CE=GP15");
    EXPECT_EQ(w.at("rows")[2].at("mcu_number"), 20);
    r = call(api, "POST", "/api/wiring", R"({"mcu":"esp32","module":"nrf24l01p"})");
    EXPECT_NE(json::parse(r.body).at("firmware").at("error").get<std::string>().find("ESP32"),
              std::string::npos);
    EXPECT_EQ(call(api, "POST", "/api/wiring",
                   R"({"mcu":"rpi-pico","module":"nrf24l01p","pins":{"CE":"GP99"}})")
                  .status,
              400);
    EXPECT_EQ(call(api, "POST", "/api/wiring", R"({"mcu":"rpi-pico"})").status, 400);
    EXPECT_EQ(call(api, "POST", "/api/wiring", R"({"mcu":"a","module":"b","pins":[]})").status,
              400);
    EXPECT_EQ(call(api, "POST", "/api/wiring", "{").status, 400);
    EXPECT_EQ(call(api, "GET", "/api/wiring").status, 405);

    // nothing built yet: no flashing
    EXPECT_EQ(call(api, "POST", "/api/firmware/flash", "{}").status, 409);

    // build, then flash onto the (fake) drive
    r = call(api, "POST", "/api/firmware/build", R"({"mcu":"rpi-pico","module":"nrf24l01p"})");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).at("state"), "running");
    json st;
    for (int i = 0; i < 200; i++) {
        st = json::parse(call(api, "GET", "/api/firmware/build").body);
        if (st.at("state") != "running") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_EQ(st.at("state"), "done") << st.dump();
    EXPECT_EQ(
        call(api, "POST", "/api/firmware/build", R"({"mcu":"esp32","module":"nrf24l01p"})").status,
        400);

    r = call(api, "POST", "/api/firmware/flash", "{}");
    EXPECT_EQ(r.status, 409);
    EXPECT_NE(json::parse(r.body).at("error").get<std::string>().find("BOOTSEL"),
              std::string::npos);
    drives = {drive.path.string()};
    EXPECT_EQ(json::parse(call(api, "GET", "/api/firmware/drives").body).at("drives")[0],
              drive.path.string());
    r = call(api, "POST", "/api/firmware/flash", "{}");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_TRUE(
        fs::exists(drive.path / "kervan_nrf24-rpi-pico-spi0-sck18-mosi19-miso16-csn17-ce20.uf2"));
    EXPECT_EQ(call(api, "POST", "/api/firmware/flash", R"({"drive":"Z:\\"})").status, 400);
    drives.push_back("other");
    EXPECT_EQ(call(api, "POST", "/api/firmware/flash", "{}").status, 409); // which one?

    EXPECT_EQ(call(api, "GET", "/api/firmware/flash").status, 405);
    EXPECT_EQ(call(api, "POST", "/api/firmware/what", "{}").status, 404);
    EXPECT_EQ(call(api, "POST", "/api/firmware/drives").status, 405);

    serve::Options none;
    none.stacks = stacks;
    EXPECT_EQ(call(serve::Api(none), "GET", "/api/firmware/build").status, 404);
}

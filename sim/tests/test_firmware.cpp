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
    EXPECT_EQ(s.target.project, "pico_nrf24");
    EXPECT_EQ(s.target.board, "pico");
    EXPECT_EQ(s.extension(), "uf2");
    EXPECT_EQ(s.overlay(), ""); // Pico SDK: pins are compile options
    EXPECT_EQ(s.name(), "kervan_nrf24-rpi-pico-spi0-sck18-mosi19-miso16-csn17-ce20");
    EXPECT_EQ(s.wiring(), "SPI0 SCK=GP18 MOSI=GP19 MISO=GP16 CSN=GP17 CE=GP20");
    EXPECT_EQ(s.cmake_args(),
              (std::vector<std::string>{"-DPICO_BOARD=pico", "-DKFW_SPI=0", "-DKFW_PIN_SCK=18",
                                        "-DKFW_PIN_MOSI=19", "-DKFW_PIN_MISO=16",
                                        "-DKFW_PIN_CSN=17", "-DKFW_PIN_CE=20"}));

    s = hw::firmware_spec(catalog(), "rpi-pico-w", "nrf24l01p-pa-lna",
                          {{"SCK", "GP10"}, {"MOSI", "GP11"}, {"MISO", "GP12"}, {"CE", "GP14"}});
    EXPECT_EQ(s.target.board, "pico_w");
    EXPECT_EQ(s.wiring(), "SPI1 SCK=GP10 MOSI=GP11 MISO=GP12 CSN=GP17 CE=GP14");

    const auto error = [](const std::string &mcu, const std::string &module) {
        try {
            hw::firmware_spec(catalog(), mcu, module, {});
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_EQ(error("esp32", "nrf24l01p"), "there is no Kervan board firmware for ESP32 yet");
    EXPECT_EQ(error("stm32-bluepill", "nrf24l01p"),
              "there is no Kervan board firmware for STM32 Blue Pill (STM32F103C8) yet");
    EXPECT_NE(error("rpi-pico", "sx1276").find("drives nRF24 modules, not"), std::string::npos);
    EXPECT_EQ(error("nope", "nrf24l01p"), "unknown MCU 'nope'");
    EXPECT_EQ(error("rpi-pico", "nope"), "unknown module 'nope'");
}

TEST(Firmware, ZephyrOverlays)
{
    // STM32: pinctrl nodes of the SPI controller, GPIO phandles by port
    hw::FirmwareSpec s =
        hw::firmware_spec(catalog(), "nucleo-f401re", "nrf24l01p", {{"CE", "PB10"}});
    EXPECT_EQ(s.name(), "kervan_nrf24-nucleo-f401re-spi1-sckpa5-mosipa7-misopa6-csnpb6-cepb10");
    EXPECT_EQ(s.wiring(), "SPI1 SCK=PA5 MOSI=PA7 MISO=PA6 CSN=PB6 CE=PB10");
    EXPECT_EQ(s.extension(), "bin");
    EXPECT_EQ(s.cmake_args(), std::vector<std::string>{"-DBOARD=nucleo_f401re"});
    std::string o = s.overlay();
    EXPECT_NE(o.find("&spi1 {"), std::string::npos) << o;
    EXPECT_NE(o.find("pinctrl-0 = <&spi1_sck_pa5 &spi1_miso_pa6 &spi1_mosi_pa7>;"),
              std::string::npos)
        << o;
    EXPECT_NE(o.find("cs-gpios = <&gpiob 6 GPIO_ACTIVE_LOW>;"), std::string::npos);
    EXPECT_NE(o.find("ce-gpios = <&gpiob 10 GPIO_ACTIVE_HIGH>;"), std::string::npos);
    EXPECT_NE(o.find("kervan,board = \"nucleo-f401re\";"), std::string::npos);
    EXPECT_NE(o.find("kervan,wiring = \"SPI1 SCK=PA5 MOSI=PA7 MISO=PA6 CSN=PB6 CE=PB10\";"),
              std::string::npos);

    // nRF: psels on the board's Arduino SPI bus, ports from the pin name
    s = hw::firmware_spec(catalog(), "nrf52840-dk", "nrf24l01p", {});
    EXPECT_EQ(s.extension(), "hex");
    o = s.overlay();
    EXPECT_NE(o.find("&arduino_spi {"), std::string::npos) << o;
    EXPECT_NE(o.find("psels = <NRF_PSEL(SPIM_SCK, 1, 15)>, <NRF_PSEL(SPIM_MOSI, 1, 13)>, "
                     "<NRF_PSEL(SPIM_MISO, 1, 14)>;"),
              std::string::npos)
        << o;
    EXPECT_NE(o.find("low-power-enable;"), std::string::npos);
    EXPECT_NE(o.find("cs-gpios = <&gpio1 12 GPIO_ACTIVE_LOW>;"), std::string::npos);
    EXPECT_NE(o.find("ce-gpios = <&gpio1 11 GPIO_ACTIVE_HIGH>;"), std::string::npos);

    // ESP32: pinmux through the GPIO matrix, GPIO32+ on gpio1
    s = hw::firmware_spec(catalog(), "esp32-devkit-v1", "nrf24l01p", {{"CE", "GPIO33"}});
    EXPECT_EQ(s.target.flash, "esptool");
    o = s.overlay();
    EXPECT_NE(o.find("#include <zephyr/dt-bindings/pinctrl/esp32-pinctrl.h>"), std::string::npos);
    EXPECT_NE(o.find("pinmux = <SPIM3_MISO_GPIO19>, <SPIM3_SCLK_GPIO18>;"), std::string::npos) << o;
    EXPECT_NE(o.find("pinmux = <SPIM3_MOSI_GPIO23>;"), std::string::npos);
    EXPECT_NE(o.find("&spi3 {"), std::string::npos);
    EXPECT_NE(o.find("ce-gpios = <&gpio1 1 GPIO_ACTIVE_HIGH>;"), std::string::npos);
    EXPECT_EQ(s.name(), "kervan_nrf24-esp32-devkit-v1-vspi-sck18-mosi23-miso19-csn5-ce33");

    // the esptool command for it
    const auto e = hw::esptool_command(s.target, "COM7", "x.bin");
    EXPECT_EQ(
        e, (std::vector<std::string>{"python", "-m", "esptool", "--chip", "esp32", "--port", "COM7",
                                     "--baud", "460800", "write_flash", "0x1000", "x.bin"}));
    EXPECT_EQ(hw::drive_kind_for("uf2"), "rp2");
    EXPECT_EQ(hw::drive_kind_for("bin"), "stlink");
    EXPECT_EQ(hw::drive_kind_for("hex"), "jlink");
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
    EXPECT_NE(
        script.find("cp /build/fw/" + s.name() + "/kervan_nrf24.uf2 build/fw/" + s.name() + ".uf2"),
        std::string::npos);
    EXPECT_EQ(script.find('"'), std::string::npos); // nothing to quote inside

    const hw::FirmwareSpec z = hw::firmware_spec(catalog(), "nrf52840-dk", "nrf24l01p", {});
    const std::string zs = hw::docker_build_command(z, "/src").back();
    EXPECT_NE(zs.find("cmake -S firmware/zephyr_nrf24 -B /build/fw/" + z.name() +
                      " -G Ninja -DBOARD=nrf52840dk/nrf52840 -DEXTRA_DTC_OVERLAY_FILE="
                      "/workspaces/kervan/build/fw/" +
                      z.name() + ".overlay"),
              std::string::npos)
        << zs;
    EXPECT_NE(
        zs.find("cp /build/fw/" + z.name() + "/zephyr/zephyr.hex build/fw/" + z.name() + ".hex"),
        std::string::npos);
}

TEST(Firmware, Builder)
{
    TempDir dir("builder");
    const hw::FirmwareSpec spec = hw::firmware_spec(catalog(), "rpi-pico", "nrf24l01p", {});
    const std::string uf2 = (dir.path / "build" / "fw" / (spec.name() + ".uf2")).string();

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
    EXPECT_EQ(b.out_dir(), (dir.path / "build" / "fw").string());
    EXPECT_EQ(s.job, "build");
    EXPECT_EQ(s.drive_kind, "rp2");
    EXPECT_FALSE(b.flash_esptool("COM1")); // a Pico is flashed by copying, not with esptool

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
    EXPECT_EQ(error(R"({"id": 1, "pos": [0, 0], "device": "pico-nrf24l01p",
                       "stack": "nrf24-esb", "pins": {"CE": "GP15"}})"),
              "no error");
    EXPECT_NE(error(R"({"id": 1, "pos": [0, 0], "mcu": "esp32", "module": "sx1276",
                       "stack": "kervan", "pins": {"NSS": "GPIO15"}})")
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
                (dir.path / "build" / "fw" / (spec.name() + "." + spec.extension())).string()};
    };
    std::string esptool_port;
    build.esptool = [&](const FirmwareTarget &t, const std::string &port,
                        const std::string &file) -> std::vector<std::string> {
        esptool_port = port;
        return {KSIM_CMAKE, "-E", "echo", "esptool", t.esp_chip, file};
    };
    opt.builder = std::make_shared<hw::FirmwareBuilder>(build);
    std::vector<hw::BootDrive> drives;
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
            EXPECT_EQ(m.at("header").at("strips")[1].at("pins")[4].at("name"), "3V3(OUT)");
            EXPECT_EQ(m.at("header").at("strips")[1].at("pins")[4].at("number"), 36);
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

    const auto wait_done = [&] {
        json j;
        for (int i = 0; i < 200; i++) {
            j = json::parse(call(api, "GET", "/api/firmware/build").body);
            if (j.at("state") != "running") {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return j;
    };

    // a Pico: copied onto its RPI-RP2 drive
    r = call(api, "POST", "/api/firmware/flash", "{}");
    EXPECT_EQ(r.status, 409);
    EXPECT_NE(json::parse(r.body).at("error").get<std::string>().find("BOOTSEL"),
              std::string::npos);
    drives = {{drive.path.string(), "rp2", "RPI-RP2"}, {"Q:\\", "stlink", "NODE_F401RE"}};
    json listed = json::parse(call(api, "GET", "/api/firmware/drives").body).at("drives");
    EXPECT_EQ(listed[0].at("path"), drive.path.string());
    EXPECT_EQ(listed[1].at("kind"), "stlink");
    r = call(api, "POST", "/api/firmware/flash", "{}");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_TRUE(
        fs::exists(drive.path / "kervan_nrf24-rpi-pico-spi0-sck18-mosi19-miso16-csn17-ce20.uf2"));
    EXPECT_EQ(call(api, "POST", "/api/firmware/flash", R"({"drive":"Q:\\"})").status,
              400); // an ST-LINK drive does not take .uf2
    drives.push_back({"other", "rp2", "RPI-RP2"});
    EXPECT_EQ(call(api, "POST", "/api/firmware/flash", "{}").status, 409); // which one?

    // an ESP32: written with esptool over the board's serial port
    r = call(api, "POST", "/api/firmware/build",
             R"({"mcu":"esp32-devkit-v1","module":"nrf24l01p"})");
    ASSERT_EQ(r.status, 200) << r.body;
    st = wait_done();
    ASSERT_EQ(st.at("state"), "done") << st.dump();
    EXPECT_EQ(st.at("flash"), "esptool");
    EXPECT_EQ(call(api, "POST", "/api/firmware/flash", "{}").status, 400); // needs a port
    r = call(api, "POST", "/api/firmware/flash", R"({"port":"COM9"})");
    ASSERT_EQ(r.status, 200) << r.body;
    st = wait_done();
    EXPECT_EQ(st.at("state"), "done") << st.dump();
    EXPECT_EQ(st.at("job"), "flash");
    EXPECT_EQ(esptool_port, "COM9");
    EXPECT_NE(st.at("file").get<std::string>().find(".bin"), std::string::npos);
    EXPECT_NE(st.at("log").dump().find("esptool esp32"), std::string::npos);
    r = call(api, "POST", "/api/firmware/flash", R"({"port":"COM9"})"); // again: still possible
    EXPECT_EQ(r.status, 200) << r.body;
    wait_done();

    EXPECT_EQ(call(api, "GET", "/api/firmware/flash").status, 405);
    EXPECT_EQ(call(api, "POST", "/api/firmware/what", "{}").status, 404);
    EXPECT_EQ(call(api, "POST", "/api/firmware/drives").status, 405);

    serve::Options none;
    none.stacks = stacks;
    EXPECT_EQ(call(serve::Api(none), "GET", "/api/firmware/build").status, 404);
}

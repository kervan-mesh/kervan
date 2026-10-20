/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "ksim/device.hpp"

using namespace ksim;

namespace {

void expect_error(const std::string &json, const std::string &needle)
{
    try {
        parse_device(json);
        ADD_FAILURE() << "no error for: " << json;
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
            << "message: " << e.what();
    }
}

} // namespace

TEST(Device, BuiltinProfiles)
{
    const DeviceRegistry reg = DeviceRegistry::builtin();
    EXPECT_GE(reg.all().size(), 3u);
    for (const char *name : {"esp32-sx1276", "esp32s3-sx1262", "arduino-nrf24l01p"}) {
        SCOPED_TRACE(name);
        const DeviceProfile *d = reg.find(name);
        ASSERT_NE(d, nullptr);
        EXPECT_GE(d->version, 1);
        EXPECT_FALSE(d->description.empty());
        EXPECT_EQ(d->check(d->radio), "");
    }
    const DeviceProfile *nrf = reg.find("arduino-nrf24l01p");
    EXPECT_EQ(nrf->radio.type, PhyType::Nrf24);
    EXPECT_EQ(nrf->radio.nrf24.channel, 76);
    EXPECT_EQ(reg.find("nope"), nullptr);
}

TEST(Device, Limits)
{
    const DeviceRegistry reg = DeviceRegistry::builtin();
    const DeviceProfile &lora = *reg.find("esp32-sx1276");
    RadioConfig r = lora.radio;
    r.tx_power_dbm = 20.0;
    EXPECT_EQ(lora.check(r), "");
    r.tx_power_dbm = 21.0;
    EXPECT_NE(lora.check(r).find("2..20"), std::string::npos);
    r = lora.radio;
    r.lora.freq_hz = 2400000000;
    EXPECT_NE(lora.check(r).find("freq_hz"), std::string::npos);
    r = RadioConfig{};
    r.type = PhyType::Nrf24;
    EXPECT_NE(lora.check(r).find("has a lora radio"), std::string::npos);

    const DeviceProfile &nrf = *reg.find("arduino-nrf24l01p");
    r = nrf.radio;
    r.tx_power_dbm = -6.0;
    EXPECT_EQ(nrf.check(r), "");
    r.tx_power_dbm = -5.0;
    EXPECT_NE(nrf.check(r).find("-18, -12, -6, 0"), std::string::npos);
}

TEST(Device, ParseErrors)
{
    expect_error("{", "invalid JSON");
    expect_error(R"({"version": 1})", "needs a \"name\"");
    expect_error(R"({"name": "x", "radio": {"sf": 7}})", "needs a \"type\"");
    expect_error(R"({"name": "x", "radio": {"type": "wifi"}})", "\"lora\" or \"nrf24\"");
    expect_error(R"({"name": "x", "radio": {"type": "nrf24", "sf": 7}})", "not a nrf24 setting");
    expect_error(R"({"name": "x", "colour": "red", "radio": {"type": "lora"}})",
                 "unknown key 'colour'");
    expect_error(R"({"name": "x", "radio": {"type": "lora", "tx_power_dbm": 30},
                     "limits": {"tx_power_dbm": {"min": 0, "max": 20}}})",
                 "defaults violate the limits");
    expect_error(R"({"name": "x", "radio": {"type": "lora"},
                     "limits": {"tx_power_dbm": {"min": 20, "max": 0}}})",
                 "min must not exceed max");
}

TEST(Device, LoadDirectoryAddsAndOverrides)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ksim_test_devices";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "my-board.json") << R"({"name": "my-board", "version": 3,
        "description": "custom", "radio": {"type": "nrf24", "channel": 2, "tx_power_dbm": -6}})";
    std::ofstream(dir / "esp32-sx1276.json") << R"({"name": "esp32-sx1276", "version": 9,
        "radio": {"type": "lora", "sf": 12}})";
    std::ofstream(dir / "notes.txt") << "ignored";

    DeviceRegistry reg = DeviceRegistry::builtin();
    reg.load_dir(dir.string());
    ASSERT_NE(reg.find("my-board"), nullptr);
    EXPECT_EQ(reg.find("my-board")->version, 3);
    EXPECT_EQ(reg.find("my-board")->radio.nrf24.channel, 2);
    EXPECT_EQ(reg.find("esp32-sx1276")->version, 9); // user profile replaces the built-in one
    EXPECT_EQ(reg.find("esp32-sx1276")->radio.lora.sf, 12);

    std::ofstream(dir / "broken.json") << "{";
    EXPECT_THROW(reg.load_dir(dir.string()), std::runtime_error);
    EXPECT_THROW(reg.load_dir((dir / "missing").string()), std::runtime_error);
    fs::remove_all(dir);
}

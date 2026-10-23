/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The hardware catalog: MCUs, radio modules, boards, combinations and wiring.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "ksim/device.hpp"

using namespace ksim;

namespace {

template <typename Parse>
void expect_error(Parse parse, const std::string &json, const std::string &needle)
{
    try {
        parse(json, "test");
        ADD_FAILURE() << "no error for: " << json;
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << "message: " << e.what();
    }
}

bool has(const std::vector<std::string> &list, const std::string &part)
{
    return std::any_of(list.begin(), list.end(),
                       [&](const std::string &s) { return s.find(part) != std::string::npos; });
}

} // namespace

TEST(Device, BuiltinCatalog)
{
    const DeviceRegistry reg = DeviceRegistry::builtin();
    for (const char *name : {"esp32-sx1276", "esp32s3-sx1262", "pico-nrf24l01p"}) {
        SCOPED_TRACE(name);
        const DeviceProfile *d = reg.find(name);
        ASSERT_NE(d, nullptr);
        EXPECT_TRUE(d->board);
        EXPECT_GE(d->version, 1);
        EXPECT_FALSE(d->description.empty());
        EXPECT_FALSE(d->mcu.empty());
        EXPECT_GE(d->module_version, 1);
        EXPECT_EQ(d->check(d->radio), "");
    }
    const DeviceProfile *nrf = reg.find("pico-nrf24l01p");
    EXPECT_EQ(nrf->radio.type, PhyType::Nrf24);
    EXPECT_EQ(nrf->radio.nrf24.channel, 76);
    EXPECT_EQ(nrf->mcu, "rpi-pico");
    EXPECT_EQ(nrf->module, "nrf24l01p");
    EXPECT_EQ(reg.find("nope"), nullptr);

    for (const char *m : {"rpi-pico", "rpi-pico-w", "esp32-devkit-v1", "esp32", "esp32-s3"}) {
        EXPECT_NE(reg.mcu(m), nullptr) << m;
    }
    for (const char *m : {"nrf24l01p", "nrf24l01p-pa-lna", "sx1276", "sx1262"}) {
        ASSERT_NE(reg.module(m), nullptr) << m;
        EXPECT_FALSE(reg.module(m)->pins.empty()) << m;
    }
    EXPECT_EQ(reg.mcus().size(), 15u);
    EXPECT_EQ(reg.modules().size(), 5u);
    EXPECT_EQ(reg.board_profile("esp32-sx1276")->integrated, true);
}

TEST(Device, CombineAnMcuWithAModule)
{
    const DeviceRegistry reg = DeviceRegistry::builtin();
    const DeviceProfile d = reg.combine("rpi-pico", "nrf24l01p-pa-lna");
    EXPECT_EQ(d.name, "rpi-pico+nrf24l01p-pa-lna");
    EXPECT_FALSE(d.board);
    EXPECT_EQ(d.mcu, "rpi-pico");
    EXPECT_EQ(d.radio.type, PhyType::Nrf24);
    EXPECT_DOUBLE_EQ(d.radio.tx_power_dbm, 20.0);
    EXPECT_DOUBLE_EQ(d.radio.antenna_dbi, 2.0);
    EXPECT_DOUBLE_EQ(d.radio.lna_db, 3.0);
    EXPECT_EQ(d.check(d.radio), "");

    const DeviceProfile bare = reg.combine("", "sx1262"); // a module on its own
    EXPECT_EQ(bare.name, "sx1262");
    EXPECT_TRUE(bare.mcu.empty());

    EXPECT_THROW(reg.combine("rpi-pico", "toaster"), std::runtime_error);
    EXPECT_THROW(reg.combine("abacus", "nrf24l01p"), std::runtime_error);
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

    const DeviceProfile &nrf = *reg.find("pico-nrf24l01p");
    r = nrf.radio;
    r.tx_power_dbm = -6.0;
    EXPECT_EQ(nrf.check(r), "");
    r.tx_power_dbm = -5.0;
    EXPECT_NE(nrf.check(r).find("-18, -12, -6, 0"), std::string::npos);

    const DeviceProfile pa = reg.combine("", "nrf24l01p-pa-lna");
    r = pa.radio;
    r.tx_power_dbm = 0.0;
    EXPECT_NE(pa.check(r).find("2, 8, 14, 20"), std::string::npos);
}

TEST(Device, Wiring)
{
    const DeviceRegistry reg = DeviceRegistry::builtin();

    // Pico + nRF24: every pin has a place, nothing to warn about
    Wiring w = reg.wiring("rpi-pico", "nrf24l01p");
    ASSERT_EQ(w.rows.size(), 8u);
    const auto pin = [&](const std::string &module_pin) {
        for (const WiringRow &r : w.rows) {
            if (r.module_pin == module_pin) {
                return r.mcu_pin;
            }
        }
        return std::string("<none>");
    };
    EXPECT_NE(pin("VCC").find("3V3"), std::string::npos);
    EXPECT_NE(pin("SCK").find("GP18"), std::string::npos);
    EXPECT_NE(pin("CSN").find("GP17"), std::string::npos);
    EXPECT_NE(pin("CE").find("GP20"), std::string::npos);
    EXPECT_NE(pin("IRQ").find("GP21"), std::string::npos);
    EXPECT_TRUE(w.warnings.empty());
    EXPECT_TRUE(has(w.notes, "10 uF"));
    EXPECT_TRUE(has(w.notes, "Pico SDK"));

    // the 1 W LoRa module needs more current than a Pico's 3.3 V pin gives
    w = reg.wiring("rpi-pico", "e32-900m30s");
    EXPECT_TRUE(has(w.warnings, "from its own supply"));

    // 5 V logic: nRF24 inputs take it, the SX1276 does not
    DeviceRegistry five = DeviceRegistry::builtin();
    five.add(parse_mcu(R"({"name": "avr", "title": "AVR", "logic_v": 5.0, "supply_out_ma": 50,
                          "pins": {"3v3": "3V3", "gnd": "GND", "spi_sck": "D13", "spi_mosi": "D11",
                                   "spi_miso": "D12", "spi_cs": "D10"},
                          "gpio": ["D9", "D2", "D3"]})"));
    w = five.wiring("avr", "nrf24l01p");
    EXPECT_FALSE(has(w.warnings, "level shifter"));
    w = five.wiring("avr", "nrf24l01p-pa-lna");
    EXPECT_TRUE(has(w.warnings, "about 50 mA"));
    w = five.wiring("avr", "sx1276");
    EXPECT_TRUE(has(w.warnings, "level shifter"));

    // without an MCU there is nothing to map, but the module's pins are listed
    w = reg.wiring("", "sx1262");
    EXPECT_EQ(w.rows.size(), 9u);
    EXPECT_TRUE(w.rows[0].mcu_pin.empty());

    EXPECT_THROW(reg.wiring("rpi-pico", "nope"), std::runtime_error);
}

TEST(Device, PinHeader)
{
    const DeviceRegistry reg = DeviceRegistry::builtin();
    const McuProfile &pico = *reg.mcu("rpi-pico");
    ASSERT_TRUE(pico.header.has_value());
    ASSERT_EQ(pico.header->strips.size(), 2u);
    const auto number = [&](const McuProfile &m, const std::string &pin) {
        const auto at = m.find_pin(pin);
        return at.first < 0 ? -1
                            : m.header->strips[static_cast<size_t>(at.first)]
                                  .pins[static_cast<size_t>(at.second)]
                                  .number;
    };
    EXPECT_EQ(number(pico, "GP0"), 1);
    EXPECT_EQ(number(pico, "GP15"), 20);
    EXPECT_EQ(number(pico, "GP16"), 21);
    EXPECT_EQ(number(pico, "3V3(OUT)"), 36);
    EXPECT_EQ(number(pico, "VBUS"), 40);
    EXPECT_EQ(number(pico, "GP23"), -1); // inside the board
    EXPECT_TRUE(pico.is_io("GP28"));
    EXPECT_FALSE(pico.is_io("GND"));
    ASSERT_EQ(pico.spi.size(), 2u);
    EXPECT_EQ(pico.spi[1].name, "SPI1");
    ASSERT_TRUE(pico.firmware.has_value());
    EXPECT_EQ(pico.firmware->board, "pico");
    EXPECT_EQ(reg.mcu("rpi-pico-w")->firmware->board, "pico_w");
    EXPECT_EQ(reg.mcu("rpi-pico-2")->firmware->board, "pico2");
    EXPECT_FALSE(reg.mcu("esp32")->header.has_value()); // the bare chip, for integrated boards

    // GND appears several times: the one nearest to a place
    const auto gnd = pico.find_pin("GND", pico.find_pin("3V3(OUT)"));
    EXPECT_EQ(gnd, std::make_pair(1, 2)); // pin 38, next to 3V3(OUT)

    // labels and numbers counting down (Nucleo CN5: D8 is pin 1, at the bottom)
    const McuProfile &nucleo = *reg.mcu("nucleo-f401re");
    EXPECT_EQ(number(nucleo, "PA9"), 1);
    EXPECT_EQ(number(nucleo, "PB8"), 10);
    const HeaderPin &d13 = nucleo.header->strips[2].pins[4];
    EXPECT_EQ(d13.name, "PA5");
    EXPECT_EQ(d13.label, "D13");

    // every catalog MCU with a header: its defaults and SPI pins are on it and wire up
    for (const McuProfile *m : reg.mcus()) {
        if (!m->header) {
            continue;
        }
        EXPECT_FALSE(m->io.empty()) << m->name;
        EXPECT_FALSE(m->spi.empty()) << m->name;
        const Wiring w = reg.wiring(m->name, "nrf24l01p");
        for (const WiringRow &r : w.rows) {
            EXPECT_GE(r.mcu_strip, 0) << m->name << " " << r.module_pin << " -> " << r.mcu_name;
        }
        EXPECT_FALSE(w.spi.empty()) << m->name;
    }

    const ModuleProfile &nrf = *reg.module("nrf24l01p");
    ASSERT_EQ(nrf.header.size(), 4u);
    EXPECT_EQ(nrf.header[0][0].name, "GND");
    EXPECT_EQ(nrf.header[3][1].name, "IRQ");
    const ModuleProfile &rfm = *reg.module("sx1276");
    EXPECT_EQ(rfm.header[3][1].name, "VCC");
    EXPECT_EQ(rfm.header[3][1].label, "13 3.3V");
    EXPECT_EQ(reg.module("e32-900m30s")->header.size(), 28u);
}

TEST(Device, ChosenPins)
{
    const DeviceRegistry reg = DeviceRegistry::builtin();
    const auto row = [](const Wiring &w, const std::string &module_pin) {
        for (const WiringRow &r : w.rows) {
            if (r.module_pin == module_pin) {
                return r;
            }
        }
        return WiringRow{};
    };

    // the default, described from the header
    Wiring w = reg.wiring("rpi-pico", "nrf24l01p");
    EXPECT_EQ(w.spi, "SPI0");
    EXPECT_EQ(row(w, "SCK").mcu_pin, "GP18 / SPI0 SCK (pin 24)");
    EXPECT_EQ(row(w, "SCK").mcu_name, "GP18");
    EXPECT_EQ(row(w, "SCK").mcu_number, 24);
    EXPECT_EQ(row(w, "CSN").mcu_pin, "GP17 (pin 22)");
    EXPECT_EQ(row(w, "GND").mcu_number, 38); // the catalog's GND, not the first one on the header
    EXPECT_EQ(row(w, "VCC").mcu_name, "3V3(OUT)");

    // CE elsewhere: IRQ keeps its default
    w = reg.wiring("rpi-pico", "nrf24l01p", {{"CE", "GP15"}});
    EXPECT_EQ(row(w, "CE").mcu_pin, "GP15 (pin 20)");
    EXPECT_EQ(row(w, "IRQ").mcu_name, "GP21");
    // CE onto IRQ's default: IRQ moves to a free GPIO of the list
    w = reg.wiring("rpi-pico", "nrf24l01p", {{"CE", "GP21"}});
    EXPECT_EQ(row(w, "IRQ").mcu_name, "GP20");

    // SPI1
    w = reg.wiring("rpi-pico", "nrf24l01p",
                   {{"SCK", "GP10"}, {"MOSI", "GP11"}, {"MISO", "GP12"}, {"CSN", "GP13"}});
    EXPECT_EQ(w.spi, "SPI1");
    EXPECT_EQ(row(w, "MISO").mcu_pin, "GP12 / SPI1 MISO (pin 16)");

    const auto error = [&](const PinChoice &pins, const std::string &mcu = "rpi-pico") {
        try {
            reg.wiring(mcu, "nrf24l01p", pins);
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_EQ(error({{"CE", "GP99"}}),
              "pins: GP99 is not a GPIO on the header of Raspberry Pi Pico");
    EXPECT_EQ(error({{"CE", "VBUS"}}),
              "pins: VBUS is not a GPIO on the header of Raspberry Pi Pico");
    EXPECT_EQ(error({{"CE", "GP17"}}), "pins: GP17 is wired to both CE and CSN");
    EXPECT_EQ(error({{"VCC", "GP3"}}), "pins: VCC always goes to 3.3 V");
    EXPECT_EQ(error({{"GND", "GP3"}}), "pins: GND always goes to GND");
    EXPECT_EQ(error({{"ANT", "GP3"}}), "pins: nRF24L01+ module (PCB antenna) has no pin 'ANT'");
    EXPECT_NE(error({{"SCK", "GP10"}}).find("must be on one SPI controller (SPI0: SCK GP2/GP6/"),
              std::string::npos);
    EXPECT_EQ(error({{"CE", "GP15"}}, "esp32"),
              "pins: choosing pins needs an MCU whose pin header is in the catalog");

    // other boards: labelled pins, input-only pins, fixed SPI
    w = reg.wiring("nucleo-f401re", "nrf24l01p", {{"CE", "PB10"}});
    EXPECT_EQ(row(w, "CE").mcu_pin, "PB10 (D6) (CN9 pin 7)");
    EXPECT_EQ(row(w, "SCK").mcu_pin, "PA5 (D13) / SPI1 SCK (CN5 pin 6)");
    EXPECT_EQ(row(w, "VCC").mcu_pin, "3V3 (CN6 pin 4)");
    EXPECT_EQ(row(w, "GND").mcu_pin, "GND (CN6 pin 6)");
    EXPECT_EQ(error({{"CE", "GPIO34"}}, "esp32-devkit-v1"),
              "pins: GPIO34 is an input only: it cannot drive CE");
    w = reg.wiring("esp32-devkit-v1", "nrf24l01p", {{"IRQ", "GPIO34"}}); // an input is fine
    EXPECT_EQ(row(w, "IRQ").mcu_pin, "GPIO34 (D34)");
    EXPECT_NE(error({{"SCK", "PA0"}}, "nucleo-f401re").find("SPI1: SCK PA5/PB3, MOSI PA7/PB5"),
              std::string::npos);
    EXPECT_EQ(error({}), "no error");
}

TEST(Device, HeaderParseErrors)
{
    const std::string strip = R"({"side": "left", "pins": ["A", {"name": "B", "label": "b"}]})";
    expect_error(parse_mcu, R"({"name": "x", "header": {"strips": []}})", "non-empty array");
    expect_error(parse_mcu, R"({"name": "x", "header": {"strips": [{"side": "top", "pins": []}]}})",
                 "\"left\", \"right\" or \"bottom\"");
    expect_error(parse_mcu, R"({"name": "x", "header": {"strips": [{"side": "left"}]}})",
                 "needs \"pins\"");
    expect_error(
        parse_mcu,
        R"({"name": "x", "header": {"strips": [{"side": "left", "step": 2, "pins": []}]}})",
        "must be 1 or -1");
    expect_error(parse_mcu,
                 R"({"name": "x", "header": {"strips": [{"side": "left", "pins": [5]}]}})",
                 "pin names or");
    expect_error(parse_mcu,
                 R"({"name": "x", "header": {"strips": [)" + strip + R"(]}, "io": ["C"]})",
                 "C is not on the header");
    expect_error(parse_mcu, R"({"name": "x", "io": ["A"]})", "A is not on the header");
    expect_error(parse_mcu,
                 R"({"name": "x", "header": {"strips": [)" + strip +
                     R"(]}, "io": ["A"], "spi": [{"name": "S", "sck": ["B"]}]})",
                 "B is not one of the GPIOs");
    expect_error(parse_mcu, R"({"name": "x", "spi": {}})", "must be an array");
    expect_error(parse_mcu, R"({"name": "x", "firmware": {"project": "p"}})",
                 "needs \"project\" and \"board\"");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "lora"}, "header": 1})",
                 "rows of pin names");
    expect_error(parse_module,
                 R"({"name": "x", "radio": {"type": "lora"}, "header": [[{"label": "y"}]]})",
                 "a pin needs a name");
}

TEST(Device, WiringWarnsAboutMissingPins)
{
    DeviceRegistry reg = DeviceRegistry::builtin();
    reg.add(parse_mcu(R"({"name": "tiny", "title": "Tiny", "logic_v": 3.3, "supply_out_ma": 20,
                          "pins": {"3v3": "P1", "gnd": "P2"}, "gpio": []})"));
    reg.add(parse_module(R"({"name": "odd", "radio": {"type": "lora"},
                             "supply_v": {"min": 4.5, "max": 5.5}, "current_ma": {"tx": 30}})"));
    Wiring w = reg.wiring("tiny", "nrf24l01p");
    EXPECT_TRUE(has(w.warnings, "no pin listed for SCK"));
    EXPECT_TRUE(has(w.warnings, "no pin listed for CE"));
    EXPECT_FALSE(has(w.warnings, "no pin listed for IRQ")); // optional
    w = reg.wiring("tiny", "odd");
    EXPECT_TRUE(has(w.warnings, "needs 4.5-5.5 V"));
    EXPECT_TRUE(has(w.warnings, "about 20 mA"));
}

TEST(Device, ParseErrors)
{
    expect_error(parse_module, "{", "invalid JSON");
    expect_error(parse_module, R"({"version": 1})", "needs a \"name\"");
    expect_error(parse_module, R"({"name": "x", "radio": {"sf": 7}})", "needs a \"type\"");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "wifi"}})",
                 "\"lora\" or \"nrf24\"");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "nrf24", "sf": 7}})",
                 "not a nrf24 setting");
    expect_error(parse_module, R"({"name": "x", "colour": "red", "radio": {"type": "lora"}})",
                 "unknown key 'colour'");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "lora", "tx_power_dbm": 30},
                                   "limits": {"tx_power_dbm": {"min": 0, "max": 20}}})",
                 "defaults violate the limits");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "lora"},
                                   "limits": {"tx_power_dbm": {"min": 20, "max": 0}}})",
                 "min must not exceed max");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "lora"},
                                   "pins": [{"name": "A", "role": "magic"}]})",
                 "unknown role 'magic'");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "lora"}, "pins": 5})", "array");
    expect_error(parse_module, R"({"name": "x", "radio": {"type": "lora"}, "notes": [1]})",
                 "array of strings");

    expect_error(parse_mcu, R"({"name": "x", "pins": {"vbat": "P1"}})", "unknown key 'vbat'");
    expect_error(parse_mcu, R"({"name": "x", "gpio": "P1"})", "array of strings");
    expect_error(parse_board, R"({"name": "x", "mcu": "rpi-pico"})", "needs a \"module\"");

    DeviceRegistry reg = DeviceRegistry::builtin();
    EXPECT_THROW(reg.add(parse_board(R"({"name": "b", "module": "nope"})")), std::runtime_error);
    EXPECT_THROW(reg.add(parse_board(R"({"name": "b", "mcu": "nope", "module": "sx1276"})")),
                 std::runtime_error);
    EXPECT_EQ(reg.find("b"), nullptr); // a rejected board is not kept
}

TEST(Device, LoadDirectoryAddsAndOverrides)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ksim_test_hardware";
    fs::remove_all(dir);
    fs::create_directories(dir / "modules");
    fs::create_directories(dir / "boards");
    std::ofstream(dir / "modules" / "my-radio.json") << R"({"name": "my-radio", "version": 3,
        "radio": {"type": "nrf24", "channel": 2, "tx_power_dbm": -6}})";
    std::ofstream(dir / "modules" / "sx1276.json") << R"({"name": "sx1276", "version": 9,
        "radio": {"type": "lora", "sf": 12}})";
    std::ofstream(dir / "boards" / "my-board.json") << R"({"name": "my-board", "version": 2,
        "mcu": "rpi-pico", "module": "my-radio"})";
    std::ofstream(dir / "notes.txt") << "ignored";

    DeviceRegistry reg = DeviceRegistry::builtin();
    reg.load_dir(dir.string());
    ASSERT_NE(reg.find("my-board"), nullptr);
    EXPECT_EQ(reg.find("my-board")->version, 2);
    EXPECT_EQ(reg.find("my-board")->module_version, 3);
    EXPECT_EQ(reg.find("my-board")->radio.nrf24.channel, 2);
    // a user module replaces the built-in one, also inside boards that use it
    EXPECT_EQ(reg.find("esp32-sx1276")->module_version, 9);
    EXPECT_EQ(reg.find("esp32-sx1276")->radio.lora.sf, 12);

    std::ofstream(dir / "boards" / "broken.json") << "{";
    EXPECT_THROW(reg.load_dir(dir.string()), std::runtime_error);
    fs::remove(dir / "boards" / "broken.json");
    std::ofstream(dir / "boards" / "orphan.json") << R"({"name": "orphan", "module": "gone"})";
    try {
        reg.load_dir(dir.string());
        ADD_FAILURE() << "loaded a board with an unknown module";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find("orphan.json"), std::string::npos) << e.what();
    }
    EXPECT_THROW(reg.load_dir((dir / "missing").string()), std::runtime_error);
    fs::remove_all(dir);
}

/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// ksim command line: run a scenario file and print a summary.

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <thread>
#include <iterator>
#include <fstream>
#include <string>
#include <vector>

#include "ksim/device.hpp"
#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"
#include "api.hpp"
#include "board.hpp"
#include "firmware.hpp"
#include "http.hpp"
#include "serial.hpp"
#include "virtual_board.hpp"

namespace {

const ksim_stack_t *builtin_stacks[] = {ksim_stack_kervan(), ksim_stack_nrf24_esb()};

std::string program_dir; // where ksim lives; ksim-node is looked up here first

int usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  ksim run <scenario.json> [--seed N] [--duration SECONDS] [--trace FILE]\n"
                 "                           [--hardware DIR] [--port P]\n"
                 "      --port P   TCP port for out-of-process nodes (default: any free port)\n"
                 "  ksim serve [--port P] [--web DIR] [--scenarios DIR] [--hardware DIR]\n"
                 "             [--allow-processes] [--virtual-boards N]\n"
                 "      web UI on http://127.0.0.1:P (default 8080), see docs/web-ui.md\n"
                 "      --virtual-boards N: N boards without hardware (ports virtual:1..N)\n"
                 "  ksim stacks                     list protocol stacks\n"
                 "  ksim devices [--hardware DIR]   list boards, radio modules and MCUs\n"
                 "  ksim wiring BOARD | MCU MODULE [PIN=GPn ...]  how to connect a module to an MCU\n"
                 "      --hardware DIR: more hardware (DIR/mcus, DIR/modules, DIR/boards)\n"
                 "  ksim ports                      list serial ports (real boards)\n"
                 "  ksim hello PORT                 ask the board on PORT what it is\n"
                 "  ksim firmware MCU MODULE [PIN=NAME ...] [--overlay FILE]\n"
                 "      build the board firmware in Docker, e.g. ksim firmware rpi-pico nrf24l01p "
                 "CE=GP15;\n"
                 "      --overlay: only write the Zephyr devicetree overlay for that wiring\n"
                 "  ksim version\n");
    return 2;
}

double seconds(ksim::Time us)
{
    return static_cast<double>(us) / 1e6;
}

void print_summary(const ksim::Scenario &scn, const ksim::Summary &sum)
{
    std::printf("seed %llu, %.3f s simulated, %zu nodes\n", (unsigned long long)scn.seed,
                seconds(sum.duration_us), scn.nodes.size());
    std::printf("models ");
    for (const auto &[name, ver] : sum.versions) {
        std::printf(" %s %s", name.c_str(), ver.c_str());
    }
    std::printf("\n\nradio   frames sent %llu, received %llu, lost: collision %llu, "
                "half-duplex %llu, receiver off %llu\n",
                (unsigned long long)sum.tx, (unsigned long long)sum.rx_ok,
                (unsigned long long)sum.lost_collision, (unsigned long long)sum.lost_half_duplex,
                (unsigned long long)sum.lost_rx_off);
    if (!sum.app_events.empty()) {
        std::printf("app    ");
        for (const auto &[name, count] : sum.app_events) {
            std::printf(" %s %llu", name.c_str(), (unsigned long long)count);
        }
        std::printf("\n");
    }

    std::printf("\n  node   airtime s   duty %%      tx   rx ok  collis.  half-dup  rx-off\n");
    bool over_limit = false;
    for (const ksim::NodeStats &n : sum.nodes) {
        const double duty = sum.duration_us > 0 ? static_cast<double>(n.airtime_us) /
                                                      static_cast<double>(sum.duration_us)
                                                : 0.0;
        const bool warn = duty > scn.duty_cycle_limit;
        over_limit = over_limit || warn;
        std::printf("%6u %11.3f %9.3f %7llu %7llu %8llu %9llu %7llu%s\n", n.id,
                    seconds(n.airtime_us), duty * 100.0, (unsigned long long)n.tx,
                    (unsigned long long)n.rx_ok, (unsigned long long)n.lost_collision,
                    (unsigned long long)n.lost_half_duplex, (unsigned long long)n.lost_rx_off,
                    warn ? "  <- over duty cycle limit" : "");
    }
    if (over_limit) {
        std::printf("\nWARNING: duty cycle limit %.2f %% exceeded\n", scn.duty_cycle_limit * 100.0);
    }
    std::printf("\ntrace hash %016llx\n", (unsigned long long)sum.trace_hash);
}

// Pull "--hardware DIR" (old name: --devices) out of args and load it on top of the built-in
// hardware catalog (DIR/mcus, DIR/modules, DIR/boards).
ksim::DeviceRegistry load_devices(std::vector<std::string> &args)
{
    ksim::DeviceRegistry devices = ksim::DeviceRegistry::builtin();
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == "--hardware" || args[i] == "--devices") {
            devices.load_dir(args[i + 1]);
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                       args.begin() + static_cast<std::ptrdiff_t>(i + 2));
            break;
        }
    }
    return devices;
}

int run(std::vector<std::string> args)
{
    const ksim::DeviceRegistry devices = load_devices(args);
    if (args.empty()) {
        return usage();
    }
    ksim::Scenario scn = ksim::load_scenario_file(args[0], devices);
    std::string trace_path;
    uint16_t port = 0;
    for (size_t i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size()) {
            return usage();
        }
        const std::string &key = args[i];
        const std::string &val = args[i + 1];
        if (key == "--seed") {
            scn.seed = std::strtoull(val.c_str(), nullptr, 0);
        } else if (key == "--duration") {
            scn.duration_us = static_cast<ksim::Time>(std::strtod(val.c_str(), nullptr) * 1e6);
        } else if (key == "--trace") {
            trace_path = val;
        } else if (key == "--port") {
            port = static_cast<uint16_t>(std::strtoul(val.c_str(), nullptr, 10));
        } else {
            return usage();
        }
    }

    ksim::Simulator sim(scn);
    sim.set_remote_options(port, program_dir, [](uint32_t node, uint16_t listen_port) {
        std::printf("node %u waits for its stack process: start it with KSIM_PORT=%u "
                    "KSIM_NODE_ID=%u (e.g. in a debugger). Virtual time is paused.\n",
                    node, unsigned{listen_port}, node);
        std::fflush(stdout);
    });
    for (const ksim_stack_t *stack : builtin_stacks) {
        sim.register_stack(stack);
    }
    std::ofstream trace;
    if (!trace_path.empty()) {
        trace.open(trace_path, std::ios::binary);
        if (!trace) {
            std::fprintf(stderr, "cannot write %s\n", trace_path.c_str());
            return 1;
        }
        sim.set_trace(&trace);
    }
    const ksim::Summary sum = sim.run();
    print_summary(scn, sum);
    return 0;
}

int serve(std::vector<std::string> args)
{
    ksim::serve::Options opt;
    opt.devices = load_devices(args);
    opt.stacks.assign(std::begin(builtin_stacks), std::end(builtin_stacks));
    opt.program_dir = program_dir;
    opt.scenario_dir = KSIM_DEFAULT_SCENARIO_DIR;
    std::string web_dir = KSIM_DEFAULT_WEB_DIR;
    uint16_t port = 8080;
    int virtual_boards = 0;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &key = args[i];
        if (key == "--allow-processes") {
            opt.allow_processes = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            return usage();
        }
        const std::string &val = args[++i];
        if (key == "--port") {
            port = static_cast<uint16_t>(std::strtoul(val.c_str(), nullptr, 10));
        } else if (key == "--web") {
            web_dir = val;
        } else if (key == "--scenarios") {
            opt.scenario_dir = val;
        } else if (key == "--virtual-boards") {
            virtual_boards = std::atoi(val.c_str());
            if (virtual_boards < 1 || virtual_boards > 8) {
                return usage();
            }
        } else {
            return usage();
        }
    }

    // virtual boards (docs/hardware.md) next to the serial ports
    const auto boards = std::make_shared<ksim::hw::VirtualBoards>(virtual_boards);
    if (virtual_boards > 0) {
        opt.list_ports = [boards] {
            std::vector<std::string> ports = ksim::hw::list_serial_ports();
            const std::vector<std::string> v = boards->ports();
            ports.insert(ports.end(), v.begin(), v.end());
            return ports;
        };
        opt.open_port = [boards](const std::string &p) {
            return boards->owns(p) ? boards->open(p) : ksim::hw::open_serial(p);
        };
    }

    ksim::hw::FirmwareBuilder::Options build;
    build.source_dir = KSIM_SOURCE_DIR;
    opt.builder = std::make_shared<ksim::hw::FirmwareBuilder>(build);

    const std::string scenario_dir = opt.scenario_dir;
    const ksim::serve::Api api(std::move(opt));
    ksim::serve::HttpServer http(api, web_dir);
    const uint16_t bound = http.bind(port);
    std::printf("ksim serve: open http://127.0.0.1:%u  (Ctrl+C stops)\n"
                "  scenarios  %s\n  web ui     %s\n",
                unsigned{bound}, scenario_dir.c_str(), web_dir.c_str());
    std::fflush(stdout);
    http.listen();
    return 0;
}

int list_devices(std::vector<std::string> args)
{
    const ksim::DeviceRegistry devices = load_devices(args);
    std::printf("boards (\"device\" in a scenario)\n");
    for (const ksim::DeviceProfile *d : devices.all()) {
        std::printf("  %-22s v%-3d %-6s %s\n", d->name.c_str(), d->version,
                    ksim::phy_for(d->radio.type).name(), d->description.c_str());
    }
    std::printf("\nradio modules (\"module\")\n");
    for (const ksim::ModuleProfile *m : devices.modules()) {
        std::printf("  %-22s v%-3d %-6s %s\n", m->name.c_str(), m->version,
                    ksim::phy_for(m->radio.type).name(), m->title.c_str());
    }
    std::printf("\nMCUs (\"mcu\", optional)\n");
    for (const ksim::McuProfile *m : devices.mcus()) {
        std::printf("  %-22s v%-3d %-10s %s\n", m->name.c_str(), m->version, m->chip.c_str(),
                    m->title.c_str());
    }
    return 0;
}

// ksim firmware MCU MODULE [PIN=GPn ...]: build the board firmware for that wiring in Docker
int build_firmware(std::vector<std::string> args)
{
    const ksim::DeviceRegistry devices = load_devices(args);
    std::string overlay_file; // --overlay FILE: write the Zephyr overlay only
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == "--overlay") {
            overlay_file = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                       args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            break;
        }
    }
    if (args.size() < 2) {
        return usage();
    }
    ksim::PinChoice pins;
    for (size_t i = 2; i < args.size(); i++) {
        const size_t eq = args[i].find('=');
        if (eq == std::string::npos) {
            return usage();
        }
        pins[args[i].substr(0, eq)] = args[i].substr(eq + 1);
    }
    const ksim::hw::FirmwareSpec spec = ksim::hw::firmware_spec(devices, args[0], args[1], pins);
    if (!overlay_file.empty()) {
        if (spec.overlay().empty()) {
            std::fprintf(stderr, "error: %s builds with the Pico SDK: its pins are compile "
                                 "options, not an overlay\n",
                         args[0].c_str());
            return 1;
        }
        std::ofstream(overlay_file, std::ios::binary) << spec.overlay();
        std::printf("%s: board %s, %s\n", overlay_file.c_str(), spec.target.board.c_str(),
                    spec.wiring().c_str());
        return 0;
    }
    std::printf("building %s (%s)\n", spec.name().c_str(), spec.wiring().c_str());
    std::fflush(stdout);
    ksim::hw::FirmwareBuilder::Options opt;
    opt.source_dir = KSIM_SOURCE_DIR;
    ksim::hw::FirmwareBuilder builder(opt);
    builder.start(spec);
    size_t shown = 0;
    ksim::hw::FirmwareBuilder::Status s;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        s = builder.status();
        for (; shown < s.log.size(); shown++) { // the log keeps its last lines only
            std::printf("%s\n", s.log[shown].c_str());
        }
        std::fflush(stdout);
    } while (s.state == "running");
    if (s.state != "done") {
        std::fprintf(stderr, "error: %s\n", s.error.c_str());
        return 1;
    }
    const std::string how =
        s.flash == "esptool"
            ? "python -m esptool --chip " + spec.target.esp_chip + " --port PORT write_flash " +
                  spec.target.flash_address + " " + s.file
        : s.drive_kind == "rp2"    ? "hold BOOTSEL while plugging the Pico in, copy the file onto "
                                     "the RPI-RP2 drive"
        : s.drive_kind == "stlink" ? "copy the file onto the board's ST-LINK drive (NODE_...)"
                                   : "copy the file onto the board's J-Link drive (JLINK)";
    std::printf("firmware: %s\nflash it: %s\n", s.file.c_str(), how.c_str());
    return 0;
}

// ksim wiring BOARD | MCU MODULE
int print_wiring(std::vector<std::string> args)
{
    const ksim::DeviceRegistry devices = load_devices(args);
    std::string mcu;
    std::string module;
    if (args.size() == 1) {
        const ksim::BoardProfile *b = devices.board_profile(args[0]);
        if (b == nullptr) {
            std::fprintf(stderr, "no board '%s' (see `ksim devices`)\n", args[0].c_str());
            return 1;
        }
        if (b->integrated) {
            std::printf("%s: the radio is on the board, nothing to wire.\n", b->title.c_str());
            return 0;
        }
        mcu = b->mcu;
        module = b->module;
    } else if (args.size() >= 2) {
        mcu = args[0];
        module = args[1];
    } else {
        return usage();
    }
    ksim::PinChoice pins; // MCU MODULE CE=GP15 ...
    for (size_t i = 2; i < args.size(); i++) {
        const size_t eq = args[i].find('=');
        if (eq == std::string::npos) {
            return usage();
        }
        pins[args[i].substr(0, eq)] = args[i].substr(eq + 1);
    }
    const ksim::Wiring w = devices.wiring(mcu, module, pins);
    std::printf("%-10s  %-30s  %s\n", "module", "mcu", "role");
    for (const ksim::WiringRow &r : w.rows) {
        std::printf("%-10s  %-30s  %s\n", r.module_pin.c_str(),
                    r.mcu_pin.empty() ? "-" : r.mcu_pin.c_str(), r.role.c_str());
    }
    for (const std::string &s : w.warnings) {
        std::printf("WARNING: %s\n", s.c_str());
    }
    for (const std::string &s : w.notes) {
        std::printf("note: %s\n", s.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 0) {
        std::error_code ec;
        program_dir = std::filesystem::absolute(argv[0], ec).parent_path().string();
    }
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }
    try {
        const std::string cmd = args[0];
        args.erase(args.begin());
        if (cmd == "run") {
            return run(args);
        }
        if (cmd == "serve") {
            return serve(args);
        }
        if (cmd == "devices") {
            return list_devices(args);
        }
        if (cmd == "wiring") {
            return print_wiring(args);
        }
        if (cmd == "ports" && args.empty()) {
            for (const std::string &p : ksim::hw::list_serial_ports()) {
                std::printf("%s\n", p.c_str());
            }
            return 0;
        }
        if (cmd == "hello" && args.size() == 1) {
            const auto io = ksim::hw::open_serial(args[0]);
            std::printf("%s\n", ksim::hw::hello(*io, 3000).json.c_str());
            return 0;
        }
        if (cmd == "firmware") {
            return build_firmware(args);
        }
        if (cmd == "stacks") {
            for (const ksim_stack_t *stack : builtin_stacks) {
                std::printf("%-12s v%s\n", stack->name, stack->version);
            }
            return 0;
        }
        if (cmd == "version") {
            std::printf("ksim %s (stack ABI %u)\n", ksim::version(), KSIM_ABI_VERSION);
            return 0;
        }
        return usage();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

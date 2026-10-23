/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "firmware.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

#include "../remote/process.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ksim::hw {

namespace fs = std::filesystem;

namespace {

constexpr size_t kLogLines = 400;

std::string join(const std::vector<std::string> &items, const char *sep)
{
    std::string out;
    for (const std::string &i : items) {
        out += (out.empty() ? "" : sep) + i;
    }
    return out;
}

// The number at the end of a pin name: "GP18" -> 18, "PA5" -> 5, "P1.15" -> 15.
int trailing_number(const std::string &name)
{
    size_t i = name.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1])) != 0) {
        i--;
    }
    if (i == name.size()) {
        throw std::runtime_error(name + " has no pin number");
    }
    return std::atoi(name.c_str() + i);
}

// For file names: "GP18" -> "18", "GPIO18" -> "18", "PA5" -> "pa5", "P1.15" -> "p1_15".
std::string short_pin(const std::string &name)
{
    for (const char *prefix : {"GPIO", "GP"}) {
        const size_t n = std::char_traits<char>::length(prefix);
        if (name.size() > n && name.compare(0, n, prefix) == 0 &&
            name.find_first_not_of("0123456789", n) == std::string::npos) {
            return name.substr(n);
        }
    }
    std::string out;
    for (char c : name) {
        out += c == '.' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string lower(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// A GPIO as a devicetree phandle with flags: "&gpioa 5 GPIO_ACTIVE_LOW".
std::string gpio_ref(const FirmwareTarget &t, const std::string &pin, const char *flags)
{
    if (t.pinctrl == "stm32") { // PA5
        if (pin.size() < 3 || pin[0] != 'P') {
            throw std::runtime_error(pin + " is not an STM32 pin");
        }
        return "&gpio" + lower(pin.substr(1, 1)) + " " + std::to_string(trailing_number(pin)) +
               " " + flags;
    }
    if (t.pinctrl == "nrf") { // P1.15
        const size_t dot = pin.find('.');
        if (pin.size() < 4 || pin[0] != 'P' || dot == std::string::npos) {
            throw std::runtime_error(pin + " is not an nRF pin");
        }
        return "&gpio" + pin.substr(1, dot - 1) + " " + std::to_string(trailing_number(pin)) + " " +
               flags;
    }
    const int n = trailing_number(pin); // ESP32: GPIO0-31 on gpio0, 32-48 on gpio1
    return (n < 32 ? "&gpio0 " + std::to_string(n) : "&gpio1 " + std::to_string(n - 32)) + " " +
           flags;
}

// nRF pinctrl: NRF_PSEL(SPIM_SCK, 1, 15)
std::string nrf_psel(const char *fn, const std::string &pin)
{
    const size_t dot = pin.find('.');
    return std::string("NRF_PSEL(") + fn + ", " + pin.substr(1, dot - 1) + ", " +
           std::to_string(trailing_number(pin)) + ")";
}

// A failed build's output, turned into what to do about it.
std::string diagnose(const std::vector<std::string> &log, int code, const std::string &job)
{
    for (const std::string &l : log) {
        if (l.find("Cannot connect to the Docker daemon") != std::string::npos ||
            l.find("docker daemon is not running") != std::string::npos ||
            l.find("error during connect") != std::string::npos ||
            l.find("failed to connect to the docker API") != std::string::npos) {
            return "Docker is not running: start Docker Desktop and build again";
        }
        if (l.find("Unable to find image") != std::string::npos ||
            l.find("pull access denied") != std::string::npos) {
            return "the dev container image is missing: run `docker build -t kervan-dev "
                   ".devcontainer` in the repository once";
        }
        if (l.find("No module named esptool") != std::string::npos) {
            return "esptool is missing: pip install esptool";
        }
        if (l.find("Failed to connect to") != std::string::npos ||
            l.find("could not open port") != std::string::npos) {
            return "esptool cannot reach the board: is it on that port, and the port free (no "
                   "live session, no serial monitor)?";
        }
    }
    return "the " + job + " failed (exit code " + std::to_string(code) + "): see the log";
}

} // namespace

std::string FirmwareSpec::name() const
{
    return "kervan_nrf24-" + mcu + "-" + lower(spi) + "-sck" + short_pin(sck) + "-mosi" +
           short_pin(mosi) + "-miso" + short_pin(miso) + "-csn" + short_pin(csn) + "-ce" +
           short_pin(ce);
}

std::string FirmwareSpec::wiring() const
{
    return spi + " SCK=" + sck + " MOSI=" + mosi + " MISO=" + miso + " CSN=" + csn + " CE=" + ce;
}

std::vector<std::string> FirmwareSpec::cmake_args() const
{
    if (target.project != "pico_nrf24") {
        return {"-DBOARD=" + target.board};
    }
    return {"-DPICO_BOARD=" + target.board,
            "-DKFW_SPI=" + std::to_string(trailing_number(spi)),
            "-DKFW_PIN_SCK=" + std::to_string(trailing_number(sck)),
            "-DKFW_PIN_MOSI=" + std::to_string(trailing_number(mosi)),
            "-DKFW_PIN_MISO=" + std::to_string(trailing_number(miso)),
            "-DKFW_PIN_CSN=" + std::to_string(trailing_number(csn)),
            "-DKFW_PIN_CE=" + std::to_string(trailing_number(ce))};
}

std::string FirmwareSpec::overlay() const
{
    if (target.project == "pico_nrf24") {
        return {};
    }
    const FirmwareTarget &t = target;
    const std::string bus = t.spi_node.empty() ? lower(spi) : t.spi_node;
    std::string head = "/* " + name() + ": written by ksim for this wiring (docs/hardware.md) */\n";
    std::string pins; // the bus node's pinctrl properties
    if (t.pinctrl == "stm32") {
        const std::string p = lower(spi); // spi1_sck_pa5
        pins = "\tpinctrl-0 = <&" + p + "_sck_" + lower(sck) + " &" + p + "_miso_" + lower(miso) +
               " &" + p + "_mosi_" + lower(mosi) + ">;\n\tpinctrl-names = \"default\";\n";
    } else if (t.pinctrl == "nrf") {
        const std::string psels = "<" + nrf_psel("SPIM_SCK", sck) + ">, <" +
                                  nrf_psel("SPIM_MOSI", mosi) + ">, <" +
                                  nrf_psel("SPIM_MISO", miso) + ">";
        head += "&pinctrl {\n"
                "\tkervan_spi_default: kervan_spi_default {\n"
                "\t\tgroup1 {\n\t\t\tpsels = " +
                psels +
                ";\n\t\t};\n\t};\n"
                "\tkervan_spi_sleep: kervan_spi_sleep {\n"
                "\t\tgroup1 {\n\t\t\tpsels = " +
                psels + ";\n\t\t\tlow-power-enable;\n\t\t};\n\t};\n};\n\n";
        pins = "\tpinctrl-0 = <&kervan_spi_default>;\n\tpinctrl-1 = <&kervan_spi_sleep>;\n"
               "\tpinctrl-names = \"default\", \"sleep\";\n";
    } else { // esp32
        const std::string chip = t.esp_chip.empty() ? "esp32" : t.esp_chip;
        const std::string mux = t.spi_pinmux;
        head += "#include <zephyr/dt-bindings/pinctrl/" + chip +
                "-pinctrl.h>\n\n"
                "&pinctrl {\n"
                "\tkervan_spi: kervan_spi {\n"
                "\t\tgroup1 {\n\t\t\tpinmux = <" +
                mux + "_MISO_GPIO" + std::to_string(trailing_number(miso)) + ">, <" + mux +
                "_SCLK_GPIO" + std::to_string(trailing_number(sck)) +
                ">;\n\t\t};\n"
                "\t\tgroup2 {\n\t\t\tpinmux = <" +
                mux + "_MOSI_GPIO" + std::to_string(trailing_number(mosi)) +
                ">;\n\t\t\toutput-low;\n\t\t};\n\t};\n};\n\n";
        pins = "\tpinctrl-0 = <&kervan_spi>;\n\tpinctrl-names = \"default\";\n";
    }
    return head + "&" + bus + " {\n\tstatus = \"okay\";\n" + pins + "\tcs-gpios = <" +
           gpio_ref(t, csn, "GPIO_ACTIVE_LOW") +
           ">;\n\n"
           "\tkervan_nrf24: nrf24@0 {\n"
           "\t\tcompatible = \"kervan,nrf24\";\n"
           "\t\treg = <0>;\n"
           "\t\tspi-max-frequency = <4000000>;\n"
           "\t\tce-gpios = <" +
           gpio_ref(t, ce, "GPIO_ACTIVE_HIGH") +
           ">;\n"
           "\t\tkervan,board = \"" +
           mcu +
           "\";\n"
           "\t\tkervan,wiring = \"" +
           wiring() + "\";\n\t};\n};\n";
}

FirmwareSpec firmware_spec(const DeviceRegistry &devices, const std::string &mcu,
                           const std::string &module, const PinChoice &pins)
{
    const McuProfile *u = devices.mcu(mcu);
    const ModuleProfile *m = devices.module(module);
    if (u == nullptr || m == nullptr) {
        throw std::runtime_error(
            "unknown " + std::string(u == nullptr ? "MCU '" + mcu : "module '" + module) + "'");
    }
    if (!u->firmware) {
        throw std::runtime_error("there is no Kervan board firmware for " + u->title + " yet");
    }
    if (m->radio.type != PhyType::Nrf24) {
        throw std::runtime_error("the board firmware drives nRF24 modules, not " + m->title);
    }
    const Wiring w = devices.wiring(mcu, module, pins);
    FirmwareSpec spec;
    spec.target = *u->firmware;
    spec.mcu = mcu;
    spec.module = module;
    spec.spi = w.spi;
    if (spec.spi.empty()) {
        throw std::runtime_error("the wiring uses no SPI controller of " + u->title);
    }
    for (const WiringRow &r : w.rows) {
        std::string *slot = r.role == "spi_sck"    ? &spec.sck
                            : r.role == "spi_mosi" ? &spec.mosi
                            : r.role == "spi_miso" ? &spec.miso
                            : r.role == "spi_cs"   ? &spec.csn
                            : r.role == "gpio_out" ? &spec.ce
                                                   : nullptr;
        if (slot != nullptr) {
            *slot = r.mcu_name;
        }
    }
    spec.overlay(); // checks that the pins fit the board's pin naming
    return spec;
}

std::vector<std::string> docker_build_command(const FirmwareSpec &spec,
                                              const std::string &source_dir,
                                              const std::string &image)
{
    const std::string dir = "/build/fw/" + spec.name();
    const std::string out = "build/fw/" + spec.name() + "." + spec.extension();
    std::string script = "set -e; cmake -S firmware/" + spec.target.project + " -B " + dir +
                         " -G Ninja " + join(spec.cmake_args(), " ");
    std::string result;
    if (spec.target.project == "pico_nrf24") {
        result = dir + "/kervan_nrf24.uf2";
    } else {
        script += " -DEXTRA_DTC_OVERLAY_FILE=/workspaces/kervan/build/fw/" + spec.name() +
                  ".overlay -DUSER_CACHE_DIR=/build/zephyr-cache"; // Zephyr's tree is read-only
        result = dir + "/zephyr/zephyr." + spec.extension();
    }
    script += "; cmake --build " + dir + "; mkdir -p build/fw; cp " + result + " " + out +
              "; echo built " + out;
    return {"docker",
            "run",
            "--rm",
            "--mount",
            "type=bind,source=" + source_dir + ",target=/workspaces/kervan",
            "--mount",
            "type=volume,source=kervan-build,target=/build",
            image,
            "bash",
            "-c",
            script};
}

std::vector<std::string> esptool_command(const FirmwareTarget &target, const std::string &port,
                                         const std::string &file)
{
    return {"python", "-m",     "esptool", "--chip",      target.esp_chip,      "--port",
            port,     "--baud", "460800",  "write_flash", target.flash_address, file};
}

// ---- builder ----

FirmwareBuilder::FirmwareBuilder(Options opt) : opt_(std::move(opt)) {}

FirmwareBuilder::~FirmwareBuilder()
{
    {
        const std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::string FirmwareBuilder::out_dir() const
{
    return (fs::path(opt_.source_dir) / "build" / "fw").string();
}

bool FirmwareBuilder::start(const FirmwareSpec &spec)
{
    std::vector<std::string> argv =
        opt_.command ? opt_.command(spec) : docker_build_command(spec, opt_.source_dir);
    const std::string overlay = spec.overlay();
    const fs::path file = fs::path(out_dir()) / (spec.name() + "." + spec.extension());
    const FirmwareTarget target = spec.target;
    {
        const std::lock_guard<std::mutex> lock(m_);
        if (status_.state == "running") {
            return false;
        }
    }
    if (!overlay.empty()) { // Zephyr reads it during the build
        std::error_code ec;
        fs::create_directories(out_dir(), ec);
        std::ofstream(fs::path(out_dir()) / (spec.name() + ".overlay"), std::ios::binary)
            << overlay;
    }
    const bool ok = begin("build", argv, [this, file, overlay, target](int code) -> std::string {
        std::error_code ec;
        if (code != 0 || !fs::is_regular_file(file, ec)) {
            return diagnose(status().log, code, "build");
        }
        const std::lock_guard<std::mutex> lock(m_);
        status_.file = file.lexically_normal().make_preferred().string();
        built_ = target;
        return {};
    });
    if (ok) {
        const std::lock_guard<std::mutex> lock(m_);
        status_.name = spec.name();
        status_.wiring = spec.wiring();
        status_.flash = spec.target.flash;
        status_.drive_kind = drive_kind_for(spec.extension());
    }
    return ok;
}

bool FirmwareBuilder::flash_esptool(const std::string &port)
{
    Status last;
    FirmwareTarget target;
    {
        const std::lock_guard<std::mutex> lock(m_);
        last = status_;
        target = built_;
    }
    if (last.file.empty() || target.flash != "esptool") {
        return false;
    }
    std::vector<std::string> argv = opt_.esptool ? opt_.esptool(target, port, last.file)
                                                 : esptool_command(target, port, last.file);
    const bool ok = begin("flash", argv, [this](int code) -> std::string {
        return code == 0 ? std::string() : diagnose(status().log, code, "flashing");
    });
    if (ok) {
        // keep what was built: the next flash needs it too
        const std::lock_guard<std::mutex> lock(m_);
        status_.name = last.name;
        status_.wiring = last.wiring;
        status_.file = last.file;
        status_.flash = last.flash;
        status_.drive_kind = last.drive_kind;
    }
    return ok;
}

FirmwareBuilder::Status FirmwareBuilder::status() const
{
    const std::lock_guard<std::mutex> lock(m_);
    return status_;
}

bool FirmwareBuilder::begin(const std::string &job, std::vector<std::string> argv,
                            std::function<std::string(int code)> finish)
{
    const std::lock_guard<std::mutex> lock(m_);
    if (status_.state == "running") {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join(); // the last job's thread has finished
    }
    status_ = Status{};
    status_.state = "running";
    status_.job = job;
    status_.log.push_back("$ " + join(argv, " "));
    thread_ = std::thread([this, argv, finish] { run(argv, finish); });
    return true;
}

void FirmwareBuilder::run(std::vector<std::string> argv,
                          std::function<std::string(int code)> finish)
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const std::string log_path =
        !opt_.log_file.empty() ? opt_.log_file
                               : (fs::temp_directory_path() / "ksim-firmware-build.log").string();
    const auto end = [&](const std::string &error) {
        const std::lock_guard<std::mutex> lock(m_);
        status_.state = error.empty() ? "done" : "failed";
        status_.error = error;
        status_.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    };

    std::error_code ec;
    fs::create_directories(out_dir(), ec);
    // empty the log first: the reader below must not see the last job's lines
    std::ofstream(log_path, std::ios::trunc).close();
    Process proc;
    try {
        proc = Process::spawn(argv, {}, "", log_path);
    } catch (const std::exception &e) {
        end(std::string(e.what()) + ": is " + argv[0] + " installed and on the PATH?");
        return;
    }

    std::ifstream log;
    std::string partial;
    const auto read_log = [&] {
        if (!log.is_open()) {
            log.open(log_path, std::ios::binary);
        }
        if (!log.is_open()) {
            return;
        }
        log.clear();
        char buf[4096];
        while (true) {
            log.read(buf, sizeof(buf));
            const std::streamsize n = log.gcount();
            if (n <= 0) {
                break;
            }
            std::vector<std::string> lines;
            for (std::streamsize i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') { // esptool rewrites a line with \r
                    if (!partial.empty()) {
                        lines.push_back(std::move(partial));
                    }
                    partial.clear();
                } else {
                    partial += buf[i];
                }
            }
            const std::lock_guard<std::mutex> lock(m_);
            for (std::string &l : lines) {
                status_.log.push_back(std::move(l));
            }
            if (status_.log.size() > kLogLines) {
                status_.log.erase(status_.log.begin(),
                                  status_.log.end() - static_cast<std::ptrdiff_t>(kLogLines));
            }
        }
    };

    std::optional<int> code;
    while (!(code = proc.exit_code())) {
        {
            const std::lock_guard<std::mutex> lock(m_);
            if (stop_) {
                break;
            }
            status_.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        }
        read_log();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!code) {
        proc.kill();
        end("stopped");
        return;
    }
    read_log();
    if (!partial.empty()) {
        const std::lock_guard<std::mutex> lock(m_);
        status_.log.push_back(partial);
    }
    end(finish(*code));
}

// ---- boot drives ----

std::string drive_kind_for(const std::string &extension)
{
    return extension == "uf2"   ? "rp2"
           : extension == "bin" ? "stlink"
           : extension == "hex" ? "jlink"
                                : "";
}

namespace {

// What kind of firmware drive a mounted volume is, "" if none.
std::string kind_of(const fs::path &root, const std::string &label)
{
    std::error_code ec;
    std::ifstream info(root / "INFO_UF2.TXT");
    const std::string text((std::istreambuf_iterator<char>(info)),
                           std::istreambuf_iterator<char>());
    if (text.find("RPI-RP2") != std::string::npos || text.find("RP2040") != std::string::npos ||
        text.find("RP2350") != std::string::npos) {
        return "rp2";
    }
    if (label.rfind("NODE_", 0) == 0 || label.rfind("NOD_", 0) == 0) {
        return "stlink"; // ST-LINK V2-1/V3 mass storage on Nucleo boards
    }
    if (label == "JLINK" || fs::exists(root / "Segger.html", ec)) {
        return "jlink"; // J-Link OB mass storage on Nordic DKs
    }
    return {};
}

} // namespace

std::vector<BootDrive> boot_drives()
{
    std::vector<BootDrive> out;
#if defined(_WIN32)
    const DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; i++) { // C: onwards; A: and B: would wake floppy drivers
        if ((drives & (1u << i)) == 0) {
            continue;
        }
        const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
        if (GetDriveTypeA(root.c_str()) != DRIVE_REMOVABLE) {
            continue;
        }
        char label[MAX_PATH + 1] = {};
        if (!GetVolumeInformationA(root.c_str(), label, sizeof(label), nullptr, nullptr, nullptr,
                                   nullptr, 0)) {
            continue;
        }
        const std::string kind = kind_of(root, label);
        if (!kind.empty()) {
            out.push_back({root, kind, label});
        }
    }
#else
    std::error_code ec;
    std::vector<fs::path> roots;
    for (const char *base : {"/Volumes", "/media", "/run/media"}) {
        for (const auto &entry : fs::directory_iterator(base, ec)) {
            roots.push_back(entry.path());
            for (const auto &sub : fs::directory_iterator(entry.path(), ec)) {
                roots.push_back(sub.path()); // /media/<user>/<label>
            }
        }
    }
    for (const fs::path &p : roots) {
        const std::string label = p.filename().string();
        const std::string kind = fs::is_directory(p, ec) ? kind_of(p, label) : "";
        if (!kind.empty()) {
            out.push_back({p.string(), kind, label});
        }
    }
#endif
    return out;
}

void flash_to_drive(const std::string &file, const BootDrive &drive)
{
    const fs::path from(file);
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) {
        throw std::runtime_error(file + " does not exist");
    }
    const std::string ext = from.extension().string().substr(1);
    if (drive_kind_for(ext) != drive.kind) {
        throw std::runtime_error(drive.path + " (" + drive.label + ") does not take ." + ext +
                                 " files");
    }
    fs::copy_file(from, fs::path(drive.path) / from.filename(),
                  fs::copy_options::overwrite_existing, ec);
    // the board restarts as soon as it has the file, and the drive can vanish while copying ends
    if (ec && fs::exists(drive.path, ec)) {
        throw std::runtime_error("copying to " + drive.path + " failed");
    }
}

void reveal(const std::string &path)
{
    const fs::path p = fs::path(path).lexically_normal().make_preferred();
    std::error_code ec;
    const bool dir = fs::is_directory(p, ec);
#if defined(_WIN32)
    std::vector<std::string> argv =
        dir ? std::vector<std::string>{"explorer.exe", p.string()}
            : std::vector<std::string>{"explorer.exe", "/select," + p.string()};
#elif defined(__APPLE__)
    std::vector<std::string> argv = dir ? std::vector<std::string>{"open", p.string()}
                                        : std::vector<std::string>{"open", "-R", p.string()};
#else
    std::vector<std::string> argv = {"xdg-open", dir ? p.string() : p.parent_path().string()};
#endif
    Process::spawn(argv, {}, "").detach();
}

} // namespace ksim::hw

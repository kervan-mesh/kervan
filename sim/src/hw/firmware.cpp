/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "firmware.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
const char *const kTarget = "kervan_nrf24"; // the executable of firmware/pico_nrf24

int gpio_number(const std::string &name)
{
    if (name.size() < 3 || name.compare(0, 2, "GP") != 0 ||
        name.find_first_not_of("0123456789", 2) != std::string::npos) {
        throw std::runtime_error(name + " is not a GPIO");
    }
    return std::atoi(name.c_str() + 2);
}

std::string join(const std::vector<std::string> &items, const char *sep)
{
    std::string out;
    for (const std::string &i : items) {
        out += (out.empty() ? "" : sep) + i;
    }
    return out;
}

// A failed build's output, turned into what to do about it.
std::string diagnose(const std::vector<std::string> &log, int code)
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
    }
    return "the build failed (exit code " + std::to_string(code) + "): see the log";
}

} // namespace

std::string FirmwareSpec::name() const
{
    return std::string(kTarget) + "-" + mcu + "-spi" + std::to_string(spi) + "-sck" +
           std::to_string(sck) + "-mosi" + std::to_string(mosi) + "-miso" + std::to_string(miso) +
           "-csn" + std::to_string(csn) + "-ce" + std::to_string(ce);
}

std::string FirmwareSpec::wiring() const
{
    return "SPI" + std::to_string(spi) + " SCK=GP" + std::to_string(sck) + " MOSI=GP" +
           std::to_string(mosi) + " MISO=GP" + std::to_string(miso) + " CSN=GP" +
           std::to_string(csn) + " CE=GP" + std::to_string(ce);
}

std::vector<std::string> FirmwareSpec::cmake_args() const
{
    return {"-DPICO_BOARD=" + board,
            "-DKFW_SPI=" + std::to_string(spi),
            "-DKFW_PIN_SCK=" + std::to_string(sck),
            "-DKFW_PIN_MOSI=" + std::to_string(mosi),
            "-DKFW_PIN_MISO=" + std::to_string(miso),
            "-DKFW_PIN_CSN=" + std::to_string(csn),
            "-DKFW_PIN_CE=" + std::to_string(ce)};
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
        throw std::runtime_error("there is no Kervan board firmware for " + u->title +
                                 " yet (only Raspberry Pi Pico and Pico W)");
    }
    if (m->radio.type != PhyType::Nrf24) {
        throw std::runtime_error("the board firmware drives nRF24 modules, not " + m->title);
    }
    const Wiring w = devices.wiring(mcu, module, pins);
    FirmwareSpec spec;
    spec.project = u->firmware->project;
    spec.board = u->firmware->board;
    spec.mcu = mcu;
    spec.module = module;
    if (w.spi.size() < 4 || w.spi.compare(0, 3, "SPI") != 0) {
        throw std::runtime_error("the wiring uses no SPI controller of " + u->title);
    }
    spec.spi = std::atoi(w.spi.c_str() + 3);
    for (const WiringRow &r : w.rows) {
        int *slot = r.role == "spi_sck"    ? &spec.sck
                    : r.role == "spi_mosi" ? &spec.mosi
                    : r.role == "spi_miso" ? &spec.miso
                    : r.role == "spi_cs"   ? &spec.csn
                    : r.role == "gpio_out" ? &spec.ce
                                           : nullptr;
        if (slot != nullptr) {
            *slot = gpio_number(r.mcu_name);
        }
    }
    return spec;
}

std::vector<std::string> docker_build_command(const FirmwareSpec &spec,
                                              const std::string &source_dir,
                                              const std::string &image)
{
    const std::string dir = "/build/fw/" + spec.name();
    const std::string script = "set -e; cmake -S firmware/" + spec.project + " -B " + dir +
                               " -G Ninja " + join(spec.cmake_args(), " ") + "; cmake --build " +
                               dir + "; mkdir -p build/uf2; cp " + dir + "/" + kTarget +
                               ".uf2 build/uf2/" + spec.name() + ".uf2; echo built build/uf2/" +
                               spec.name() + ".uf2";
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
    return (fs::path(opt_.source_dir) / "build" / "uf2").string();
}

bool FirmwareBuilder::start(const FirmwareSpec &spec)
{
    std::vector<std::string> argv =
        opt_.command ? opt_.command(spec) : docker_build_command(spec, opt_.source_dir);
    const std::lock_guard<std::mutex> lock(m_);
    if (status_.state == "running") {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join(); // the last build's thread has finished
    }
    status_ = Status{};
    status_.state = "running";
    status_.name = spec.name();
    status_.wiring = spec.wiring();
    status_.log.push_back("$ " + join(argv, " "));
    thread_ = std::thread([this, spec, argv] { run(spec, argv); });
    return true;
}

FirmwareBuilder::Status FirmwareBuilder::status() const
{
    const std::lock_guard<std::mutex> lock(m_);
    return status_;
}

void FirmwareBuilder::run(FirmwareSpec spec, std::vector<std::string> argv)
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const std::string log_path =
        !opt_.log_file.empty() ? opt_.log_file
                               : (fs::temp_directory_path() / "ksim-firmware-build.log").string();
    const auto finish = [&](const std::string &state, const std::string &error,
                            const std::string &file) {
        const std::lock_guard<std::mutex> lock(m_);
        status_.state = state;
        status_.error = error;
        status_.file = file;
        status_.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    };

    std::error_code ec;
    fs::create_directories(out_dir(), ec);
    // empty the log first: the reader below must not see the last build's lines
    std::ofstream(log_path, std::ios::trunc).close();
    Process proc;
    try {
        proc = Process::spawn(argv, {}, "", log_path);
    } catch (const std::exception &e) {
        finish("failed", std::string(e.what()) + ": is " + argv[0] + " installed and on the PATH?",
               "");
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
                if (buf[i] == '\n') {
                    if (!partial.empty() && partial.back() == '\r') {
                        partial.pop_back();
                    }
                    lines.push_back(std::move(partial));
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
        finish("failed", "stopped", "");
        return;
    }
    read_log();
    if (!partial.empty()) {
        const std::lock_guard<std::mutex> lock(m_);
        status_.log.push_back(partial);
    }
    const fs::path file = fs::path(out_dir()) / (spec.name() + ".uf2");
    if (*code == 0 && fs::is_regular_file(file, ec)) {
        finish("done", "", file.lexically_normal().make_preferred().string());
    } else {
        finish("failed", diagnose(status().log, *code), "");
    }
}

// ---- boot drives ----

std::vector<std::string> uf2_drives()
{
    std::vector<std::string> out;
    const auto is_boot_drive = [](const fs::path &dir) {
        std::ifstream info(dir / "INFO_UF2.TXT");
        std::string text((std::istreambuf_iterator<char>(info)), std::istreambuf_iterator<char>());
        return text.find("RPI-RP2") != std::string::npos ||
               text.find("RP2040") != std::string::npos;
    };
#if defined(_WIN32)
    const DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; i++) { // C: onwards; A: and B: would wake floppy drivers
        if ((drives & (1u << i)) == 0) {
            continue;
        }
        const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
        if (GetDriveTypeA(root.c_str()) == DRIVE_REMOVABLE && is_boot_drive(root)) {
            out.push_back(root);
        }
    }
#else
    std::vector<fs::path> candidates = {"/Volumes/RPI-RP2", "/media/RPI-RP2"};
    std::error_code ec;
    for (const char *base : {"/media", "/run/media"}) {
        for (const auto &user : fs::directory_iterator(base, ec)) {
            candidates.push_back(user.path() / "RPI-RP2");
        }
    }
    for (const fs::path &p : candidates) {
        if (fs::is_directory(p, ec) && is_boot_drive(p)) {
            out.push_back(p.string());
        }
    }
#endif
    return out;
}

void flash_uf2(const std::string &file, const std::string &drive)
{
    const std::vector<std::string> drives = uf2_drives();
    if (std::find(drives.begin(), drives.end(), drive) == drives.end()) {
        throw std::runtime_error(drive + " is not a Pico in BOOTSEL mode (hold BOOTSEL while "
                                         "plugging it in)");
    }
    const fs::path from(file);
    std::error_code ec;
    if (from.extension() != ".uf2" || !fs::is_regular_file(from, ec)) {
        throw std::runtime_error(file + " is not a .uf2 file");
    }
    fs::copy_file(from, fs::path(drive) / from.filename(), fs::copy_options::overwrite_existing,
                  ec);
    // the board restarts as soon as it has the file, and the drive can vanish while copying ends
    if (ec && fs::exists(drive, ec)) {
        throw std::runtime_error("copying to " + drive + " failed");
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

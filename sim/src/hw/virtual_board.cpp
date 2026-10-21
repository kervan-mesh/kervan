/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "virtual_board.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "fake_nrf24.h"
#include "kfw/app.h"

namespace ksim::hw {

using Clock = std::chrono::steady_clock;

class VirtualAir {
public:
    VirtualAir() { fake_air_init(&air); }
    std::mutex m;
    fake_air_t air{};
    Clock::time_point t0 = Clock::now();
};

namespace {

class VirtualBoard final : public LineIo {
public:
    VirtualBoard(std::shared_ptr<VirtualAir> air, std::string id, bool radio)
        : air_(std::move(air)), id_(std::move(id))
    {
        const std::lock_guard<std::mutex> lock(air_->m);
        fake_nrf24_init(&chip_, &air_->air);
        kfw_app_host_t host{};
        host.write_line = [](void *ctx, const char *line) {
            static_cast<VirtualBoard *>(ctx)->out_.emplace_back(line);
        };
        host.ctx = this;
        host.board = "virtual";
        host.board_id = id_.c_str();
        const kfw_nrf24_io_t io = fake_nrf24_io(&chip_);
        kfw_app_init(&app_, &host, radio ? &io : nullptr);
    }

    ~VirtualBoard() override
    {
        const std::lock_guard<std::mutex> lock(air_->m);
        fake_air_t &a = air_->air;
        for (size_t i = 0; i < a.n; i++) {
            if (a.chips[i] == &chip_) {
                a.chips[i] = a.chips[--a.n];
                break;
            }
        }
    }

    VirtualBoard(const VirtualBoard &) = delete;
    VirtualBoard &operator=(const VirtualBoard &) = delete;

    void write_line(const std::string &line) override
    {
        const std::lock_guard<std::mutex> lock(air_->m);
        kfw_app_line(&app_, line.data(), line.size(), now());
    }

    std::optional<std::string> read_line(int timeout_ms) override
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            {
                const std::lock_guard<std::mutex> lock(air_->m);
                kfw_app_poll(&app_, now());
                if (!out_.empty()) {
                    std::string line = std::move(out_.front());
                    out_.pop_front();
                    return line;
                }
            }
            if (Clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    uint64_t now() const
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - air_->t0).count());
    }

    std::shared_ptr<VirtualAir> air_;
    std::string id_;
    fake_nrf24_t chip_{};
    kfw_app_t app_{};
    std::deque<std::string> out_;
};

} // namespace

VirtualBoards::VirtualBoards(int count) : count_(count), air_(std::make_shared<VirtualAir>()) {}

VirtualBoards::~VirtualBoards() = default;

std::vector<std::string> VirtualBoards::ports() const
{
    std::vector<std::string> out;
    for (int i = 1; i <= count_; i++) {
        out.push_back("virtual:" + std::to_string(i));
    }
    return out;
}

bool VirtualBoards::owns(const std::string &port) const
{
    for (const std::string &p : ports()) {
        if (p == port) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<LineIo> VirtualBoards::open(const std::string &port, bool radio) const
{
    if (!owns(port)) {
        throw std::runtime_error(port + ": no such port");
    }
    return std::make_unique<VirtualBoard>(air_, "VIRTUAL-" + port.substr(8), radio);
}

} // namespace ksim::hw

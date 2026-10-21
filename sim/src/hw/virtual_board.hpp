/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Boards without hardware: the board firmware's own logic (firmware/common) on modelled nRF24
// chips (firmware/testing) that share one perfect radio space. `ksim serve --virtual-boards N`
// offers them as ports "virtual:1".."virtual:N", to try live sessions with boards on any PC;
// tests use them too.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "serial.hpp"

namespace ksim::hw {

class VirtualAir; // the radio space the boards share

class VirtualBoards {
public:
    explicit VirtualBoards(int count);
    ~VirtualBoards();

    std::vector<std::string> ports() const; // "virtual:1", ...
    bool owns(const std::string &port) const;
    // A board for one of ports(), with an nRF24 module unless radio is false.
    std::unique_ptr<LineIo> open(const std::string &port, bool radio = true) const;

private:
    int count_;
    std::shared_ptr<VirtualAir> air_;
};

} // namespace ksim::hw

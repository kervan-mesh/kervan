/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ksim/engine.hpp"
#include "ksim/lora.hpp"
#include "ksim/nrf24.hpp"
#include "ksim/stack.h"

namespace ksim {

enum class PhyType { LoRa = KSIM_PHY_LORA, Nrf24 = KSIM_PHY_NRF24 };

// A node's radio: which PHY and its settings. Only the block matching `type` is used.
struct RadioConfig {
    PhyType type = PhyType::LoRa;
    LoraParams lora;
    Nrf24Params nrf24;
    double tx_power_dbm = 14.0;
};

// Everything that differs between radio technologies. Path loss, shadowing, the half-duplex rule
// and tracing are shared and live in the simulator. Radios of different types never interact.
class Phy {
public:
    virtual ~Phy() = default;

    virtual PhyType type() const = 0;
    virtual const char *name() const = 0; // scenario "type"
    virtual int version() const = 0;     // bump whenever results may change

    virtual std::string validate(const RadioConfig &r) const = 0;
    virtual bool frame_len_ok(size_t len) const = 0;
    virtual Time airtime_us(const RadioConfig &r, size_t len) const = 0;
    virtual uint64_t carrier_hz(const RadioConfig &r) const = 0;
    virtual double noise_floor_dbm(const RadioConfig &r, double noise_figure_db) const = 0;

    // A receiver tuned to `rx` can demodulate a frame sent with `tx`.
    virtual bool can_receive(const RadioConfig &rx, const RadioConfig &tx) const = 0;
    // The signal is strong enough to decode (ignoring interference).
    virtual bool decodable(const RadioConfig &tx, double rssi_dbm, double snr_db) const = 0;
    // A frame sent with `other` disturbs the reception of one sent with `wanted`.
    virtual bool interferes(const RadioConfig &wanted, const RadioConfig &other) const = 0;
    // How much stronger the wanted frame must be than an interferer to survive.
    virtual double capture_db(const RadioConfig &wanted, double lora_capture_db) const = 0;

    // PHY-specific KSIM_RADIO_* parameters (common ones are handled by the simulator).
    virtual int get(const RadioConfig &r, uint32_t param, int64_t *value) const = 0;
    virtual int set(RadioConfig &r, uint32_t param, int64_t value) const = 0;
};

const Phy &phy_for(PhyType type);

// nullptr if unknown
const Phy *phy_by_name(const std::string &name);

} // namespace ksim

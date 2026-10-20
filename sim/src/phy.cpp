/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ksim/phy.hpp"

#include <cmath>

namespace ksim {

namespace {

class LoraPhy final : public Phy {
public:
    PhyType type() const override { return PhyType::LoRa; }
    const char *name() const override { return "lora"; }
    int version() const override { return 1; }

    std::string validate(const RadioConfig &r) const override { return lora_validate(r.lora); }
    bool frame_len_ok(size_t len) const override { return len >= 1 && len <= 255; }
    Time airtime_us(const RadioConfig &r, size_t len) const override
    {
        return lora_airtime_us(r.lora, len);
    }
    uint64_t carrier_hz(const RadioConfig &r) const override { return r.lora.freq_hz; }
    double noise_floor_dbm(const RadioConfig &r, double nf) const override
    {
        return ksim::noise_floor_dbm(r.lora.bw_hz, nf);
    }

    bool can_receive(const RadioConfig &rx, const RadioConfig &tx) const override
    {
        return same_channel(rx.lora, tx.lora);
    }
    bool decodable(const RadioConfig &tx, double, double snr_db) const override
    {
        return snr_db >= lora_snr_threshold_db(tx.lora.sf);
    }
    // Different spreading factors are treated as orthogonal.
    bool interferes(const RadioConfig &wanted, const RadioConfig &other) const override
    {
        return same_channel(wanted.lora, other.lora);
    }
    double capture_db(const RadioConfig &, double lora_capture_db) const override
    {
        return lora_capture_db;
    }

    int get(const RadioConfig &r, uint32_t param, int64_t *value) const override
    {
        switch (param) {
        case KSIM_RADIO_FREQ_HZ:
            *value = static_cast<int64_t>(r.lora.freq_hz);
            return KSIM_OK;
        case KSIM_RADIO_LORA_SF:
            *value = r.lora.sf;
            return KSIM_OK;
        case KSIM_RADIO_LORA_BW_HZ:
            *value = r.lora.bw_hz;
            return KSIM_OK;
        default:
            return KSIM_ERR_UNSUPPORTED;
        }
    }

    int set(RadioConfig &r, uint32_t param, int64_t value) const override
    {
        LoraParams p = r.lora;
        switch (param) {
        case KSIM_RADIO_FREQ_HZ:
            p.freq_hz = value > 0 ? static_cast<uint64_t>(value) : 0u;
            break;
        case KSIM_RADIO_LORA_SF:
            p.sf = static_cast<int>(value);
            break;
        case KSIM_RADIO_LORA_BW_HZ:
            p.bw_hz = value > 0 && value <= 500000 ? static_cast<uint32_t>(value) : 0u;
            break;
        default:
            return KSIM_ERR_UNSUPPORTED;
        }
        if (!lora_validate(p).empty()) {
            return KSIM_ERR_INVALID;
        }
        r.lora = p;
        return KSIM_OK;
    }

private:
    static bool same_channel(const LoraParams &a, const LoraParams &b)
    {
        return a.freq_hz == b.freq_hz && a.sf == b.sf && a.bw_hz == b.bw_hz;
    }
};

class Nrf24Phy final : public Phy {
public:
    PhyType type() const override { return PhyType::Nrf24; }
    const char *name() const override { return "nrf24"; }
    int version() const override { return 1; }

    std::string validate(const RadioConfig &r) const override { return nrf24_validate(r.nrf24); }
    bool frame_len_ok(size_t len) const override
    {
        return len >= kNrf24MinFrame && len <= kNrf24MaxFrame;
    }
    Time airtime_us(const RadioConfig &r, size_t len) const override
    {
        return nrf24_airtime_us(r.nrf24, len);
    }
    uint64_t carrier_hz(const RadioConfig &r) const override { return nrf24_carrier_hz(r.nrf24); }
    double noise_floor_dbm(const RadioConfig &r, double nf) const override
    {
        const double bw_hz = 2e6 * nrf24_half_band_mhz(r.nrf24.data_rate);
        return -174.0 + 10.0 * std::log10(bw_hz) + nf;
    }

    bool can_receive(const RadioConfig &rx, const RadioConfig &tx) const override
    {
        return rx.nrf24.channel == tx.nrf24.channel && rx.nrf24.data_rate == tx.nrf24.data_rate;
    }
    bool decodable(const RadioConfig &tx, double rssi_dbm, double) const override
    {
        return rssi_dbm >= nrf24_sensitivity_dbm(tx.nrf24.data_rate);
    }
    bool interferes(const RadioConfig &wanted, const RadioConfig &other) const override
    {
        return nrf24_bands_overlap(wanted.nrf24, other.nrf24);
    }
    double capture_db(const RadioConfig &wanted, double) const override
    {
        return nrf24_capture_db(wanted.nrf24.data_rate);
    }

    int get(const RadioConfig &r, uint32_t param, int64_t *value) const override
    {
        switch (param) {
        case KSIM_RADIO_NRF24_CHANNEL:
            *value = r.nrf24.channel;
            return KSIM_OK;
        case KSIM_RADIO_NRF24_RATE:
            *value = r.nrf24.data_rate;
            return KSIM_OK;
        default:
            return KSIM_ERR_UNSUPPORTED;
        }
    }

    int set(RadioConfig &r, uint32_t param, int64_t value) const override
    {
        Nrf24Params p = r.nrf24;
        switch (param) {
        case KSIM_RADIO_NRF24_CHANNEL:
            p.channel = value >= 0 && value <= 1000 ? static_cast<int>(value) : -1;
            break;
        case KSIM_RADIO_NRF24_RATE:
            p.data_rate = value > 0 && value <= 2000000 ? static_cast<uint32_t>(value) : 0u;
            break;
        default:
            return KSIM_ERR_UNSUPPORTED;
        }
        if (!nrf24_validate(p).empty()) {
            return KSIM_ERR_INVALID;
        }
        r.nrf24 = p;
        return KSIM_OK;
    }
};

const LoraPhy kLora;
const Nrf24Phy kNrf24;

} // namespace

const Phy &phy_for(PhyType type)
{
    return type == PhyType::Nrf24 ? static_cast<const Phy &>(kNrf24)
                                  : static_cast<const Phy &>(kLora);
}

const Phy *phy_by_name(const std::string &name)
{
    if (name == kLora.name()) {
        return &kLora;
    }
    if (name == kNrf24.name()) {
        return &kNrf24;
    }
    return nullptr;
}

} // namespace ksim

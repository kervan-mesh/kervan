/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Encoding of the remote protocol fields (little-endian, docs/remote-protocol.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace ksim::wire {

class Writer {
public:
    explicit Writer(uint8_t type) : buf_(4, 0) { u8(type); }

    Writer &u8(uint8_t v)
    {
        buf_.push_back(v);
        return *this;
    }
    Writer &u16(uint16_t v) { return le(v, 2); }
    Writer &u32(uint32_t v) { return le(v, 4); }
    Writer &u64(uint64_t v) { return le(v, 8); }
    Writer &i32(int32_t v) { return le(static_cast<uint32_t>(v), 4); }
    Writer &i64(int64_t v) { return le(static_cast<uint64_t>(v), 8); }
    Writer &f64(double v)
    {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return le(bits, 8);
    }
    Writer &bytes(const uint8_t *data, size_t len)
    {
        if (len > 0xFFFFu) {
            throw std::length_error("field longer than 65535 bytes");
        }
        u16(static_cast<uint16_t>(len));
        buf_.insert(buf_.end(), data, data + len);
        return *this;
    }
    Writer &str(const std::string &s)
    {
        return bytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    // The complete message including its length prefix.
    const std::vector<uint8_t> &finish()
    {
        const auto len = static_cast<uint32_t>(buf_.size() - 4);
        for (int i = 0; i < 4; i++) {
            buf_[static_cast<size_t>(i)] = static_cast<uint8_t>(len >> (8 * i));
        }
        return buf_;
    }

private:
    Writer &le(uint64_t v, int n)
    {
        for (int i = 0; i < n; i++) {
            buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        return *this;
    }
    std::vector<uint8_t> buf_;
};

// Reads one message body (type byte first). Throws std::runtime_error when it is too short.
class Reader {
public:
    explicit Reader(std::vector<uint8_t> body) : buf_(std::move(body)) {}

    uint8_t u8() { return static_cast<uint8_t>(le(1)); }
    uint16_t u16() { return static_cast<uint16_t>(le(2)); }
    uint32_t u32() { return static_cast<uint32_t>(le(4)); }
    uint64_t u64() { return le(8); }
    int32_t i32() { return static_cast<int32_t>(static_cast<uint32_t>(le(4))); }
    int64_t i64() { return static_cast<int64_t>(le(8)); }
    double f64()
    {
        const uint64_t bits = le(8);
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::vector<uint8_t> bytes()
    {
        const size_t n = u16();
        need(n);
        std::vector<uint8_t> out(buf_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                 buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return out;
    }
    std::string str()
    {
        const std::vector<uint8_t> b = bytes();
        return std::string(b.begin(), b.end());
    }

private:
    void need(size_t n) const
    {
        if (buf_.size() - pos_ < n) {
            throw std::runtime_error("malformed message (too short)");
        }
    }
    uint64_t le(int n)
    {
        need(static_cast<size_t>(n));
        uint64_t v = 0;
        for (int i = 0; i < n; i++) {
            v |= static_cast<uint64_t>(buf_[pos_++]) << (8 * i);
        }
        return v;
    }
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
};

} // namespace ksim::wire

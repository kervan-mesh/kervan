/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Internal helpers shared by the scenario and device-profile parsers.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

#include <nlohmann/json.hpp>

#include "ksim/phy.hpp"

namespace ksim::json_util {

using nlohmann::json;

[[noreturn]] void fail(const std::string &where, const std::string &msg);

// obj must be an object whose keys are all in `allowed`.
void check_object(const json &obj, std::initializer_list<const char *> allowed,
                  const std::string &where);

double get_double(const json &obj, const char *key, double def, const std::string &where);
uint64_t get_u64(const json &obj, const char *key, uint64_t def, uint64_t max,
                 const std::string &where);
int get_int(const json &obj, const char *key, int def, const std::string &where);
bool get_bool(const json &obj, const char *key, bool def, const std::string &where);
std::string get_string(const json &obj, const char *key, const std::string &def,
                       const std::string &where);

// Apply a "radio" object on top of `base`. With "type" the PHY may change (other settings then
// start from that PHY's defaults); keys of another PHY are rejected.
RadioConfig parse_radio(const json &j, RadioConfig base, const std::string &where);

// The settings of r's PHY in the format parse_radio reads.
json radio_json(const RadioConfig &r);

// A stack config object {"a": 1, "b": true} as the string stacks get: "a=1;b=1" (keys sorted, so
// the string is deterministic). Values must be numbers, strings or booleans without ';' or '='.
std::string config_string(const json &cfg, const std::string &where);

} // namespace ksim::json_util

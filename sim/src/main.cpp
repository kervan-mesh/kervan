/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// ksim command line: run a scenario file and print a summary.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "ksim/scenario.hpp"
#include "ksim/simulator.hpp"
#include "ksim/stacks/kervan.h"

namespace {

const ksim_stack_t *builtin_stacks[] = {ksim_stack_kervan()};

int usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  ksim run <scenario.json> [--seed N] [--duration SECONDS] [--trace FILE]\n"
                 "  ksim stacks\n");
    return 2;
}

double seconds(ksim::Time us)
{
    return static_cast<double>(us) / 1e6;
}

void print_summary(const ksim::Scenario &scn, const ksim::Summary &sum)
{
    std::printf("seed %llu, %.3f s simulated, %zu nodes\n\n", (unsigned long long)scn.seed,
                seconds(sum.duration_us), scn.nodes.size());
    std::printf("radio   frames sent %llu, received %llu, lost: collision %llu, half-duplex %llu\n",
                (unsigned long long)sum.tx, (unsigned long long)sum.rx_ok,
                (unsigned long long)sum.lost_collision, (unsigned long long)sum.lost_half_duplex);
    if (!sum.app_events.empty()) {
        std::printf("app    ");
        for (const auto &[name, count] : sum.app_events) {
            std::printf(" %s %llu", name.c_str(), (unsigned long long)count);
        }
        std::printf("\n");
    }

    std::printf("\n  node   airtime s   duty %%      tx   rx ok  collis.  half-dup\n");
    bool over_limit = false;
    for (const ksim::NodeStats &n : sum.nodes) {
        const double duty =
            sum.duration_us > 0 ? static_cast<double>(n.airtime_us) / static_cast<double>(sum.duration_us)
                                : 0.0;
        const bool warn = duty > scn.duty_cycle_limit;
        over_limit = over_limit || warn;
        std::printf("%6u %11.3f %9.3f %7llu %7llu %8llu %9llu%s\n", n.id, seconds(n.airtime_us),
                    duty * 100.0, (unsigned long long)n.tx, (unsigned long long)n.rx_ok,
                    (unsigned long long)n.lost_collision, (unsigned long long)n.lost_half_duplex,
                    warn ? "  <- over duty cycle limit" : "");
    }
    if (over_limit) {
        std::printf("\nWARNING: duty cycle limit %.2f %% exceeded\n", scn.duty_cycle_limit * 100.0);
    }
    std::printf("\ntrace hash %016llx\n", (unsigned long long)sum.trace_hash);
}

int run(const std::vector<std::string> &args)
{
    if (args.empty()) {
        return usage();
    }
    ksim::Scenario scn = ksim::load_scenario_file(args[0]);
    std::string trace_path;
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
        } else {
            return usage();
        }
    }

    ksim::Simulator sim(scn);
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

} // namespace

int main(int argc, char **argv)
{
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
        if (cmd == "stacks") {
            for (const ksim_stack_t *stack : builtin_stacks) {
                std::printf("%s\n", stack->name);
            }
            return 0;
        }
        return usage();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

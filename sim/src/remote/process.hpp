/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Start and stop a child process (Windows / POSIX).

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ksim {

class Process {
public:
    using Env = std::vector<std::pair<std::string, std::string>>;

    Process() = default;
    ~Process(); // kills the process if it still runs
    Process(Process &&other) noexcept;
    Process &operator=(Process &&other) noexcept;
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    // argv[0] without a directory is looked up in search_dir first, then on PATH.
    // `env` is added to the child's environment. Throws if the program cannot be started.
    static Process spawn(const std::vector<std::string> &argv, const Env &env,
                         const std::string &search_dir);

    bool started() const;
    // Exit code once the process has ended, nullopt while it runs.
    std::optional<int> exit_code();
    // Wait up to timeout_ms for a clean exit, then kill.
    void wait_or_kill(int timeout_ms);
    void kill();

private:
#if defined(_WIN32)
    void *handle_ = nullptr;
#else
    int pid_ = -1;
#endif
    std::optional<int> exit_code_;
};

} // namespace ksim

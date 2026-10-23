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
    // `env` is added to the child's environment. With `output`, the child's stdout and stderr go
    // to that file (created or truncated) and it gets no console window. Throws if the program
    // cannot be started.
    static Process spawn(const std::vector<std::string> &argv, const Env &env,
                         const std::string &search_dir, const std::string &output = "");

    bool started() const;
    // Exit code once the process has ended, nullopt while it runs.
    std::optional<int> exit_code();
    // Wait up to timeout_ms for a clean exit, then kill.
    void wait_or_kill(int timeout_ms);
    void kill();
    // Let the process run on by itself (for programs that should outlive this object).
    void detach();

private:
#if defined(_WIN32)
    void *handle_ = nullptr;
#else
    int pid_ = -1;
#endif
    std::optional<int> exit_code_;
};

} // namespace ksim

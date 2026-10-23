/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "process.hpp"

#include <chrono>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ksim {

namespace {

std::string resolve(const std::string &program, const std::string &search_dir)
{
    namespace fs = std::filesystem;
    if (search_dir.empty() || fs::path(program).has_parent_path()) {
        return program;
    }
    for (const char *ext : {"", ".exe"}) {
        const fs::path candidate = fs::path(search_dir) / (program + ext);
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
    }
    return program; // PATH
}

#if defined(_WIN32)
// Quote one argument for CreateProcess / CommandLineToArgvW rules.
std::string quote(const std::string &arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            backslashes++;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        } else {
            backslashes = 0;
        }
        out += c;
    }
    out.append(backslashes, '\\');
    out += '"';
    return out;
}
#endif

} // namespace

Process::~Process()
{
    kill();
}

Process::Process(Process &&other) noexcept
{
    *this = std::move(other);
}

Process &Process::operator=(Process &&other) noexcept
{
    if (this != &other) {
        kill();
#if defined(_WIN32)
        handle_ = other.handle_;
        other.handle_ = nullptr;
#else
        pid_ = other.pid_;
        other.pid_ = -1;
#endif
        exit_code_ = other.exit_code_;
    }
    return *this;
}

Process Process::spawn(const std::vector<std::string> &argv, const Env &env,
                       const std::string &search_dir, const std::string &output)
{
    if (argv.empty()) {
        throw std::invalid_argument("empty command");
    }
    const std::string program = resolve(argv[0], search_dir);
    Process p;
#if defined(_WIN32)
    for (const auto &[name, value] : env) {
        SetEnvironmentVariableA(name.c_str(), value.c_str()); // inherited by the child
    }
    std::string cmdline = quote(program);
    for (size_t i = 1; i < argv.size(); i++) {
        cmdline += " " + quote(argv[i]);
    }
    PROCESS_INFORMATION pi{};
    if (output.empty()) {
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &pi)) {
            throw std::runtime_error("cannot start '" + program + "'");
        }
    } else {
        // the child inherits the output file and nothing else (not ksim's sockets)
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE out = CreateFileA(output.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot create " + output);
        }
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<char> storage(size);
        auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        InitializeProcThreadAttributeList(attrs, 1, 0, &size);
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof(out),
                                  nullptr, nullptr);
        STARTUPINFOEXA si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = out;
        si.StartupInfo.hStdError = out;
        si.lpAttributeList = attrs;
        const BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                       nullptr, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(out);
        if (!ok) {
            throw std::runtime_error("cannot start '" + program + "'");
        }
    }
    CloseHandle(pi.hThread);
    p.handle_ = pi.hProcess;
#else
    std::vector<char *> args;
    std::vector<std::string> copy = argv;
    copy[0] = program;
    for (std::string &a : copy) {
        args.push_back(a.data());
    }
    args.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        if (!output.empty()) {
            const int fd = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            const int in = open("/dev/null", O_RDONLY);
            if (fd < 0 || in < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0 || dup2(in, 0) < 0) {
                _exit(126);
            }
        }
        for (const auto &[name, value] : env) {
            setenv(name.c_str(), value.c_str(), 1);
        }
        execvp(args[0], args.data());
        _exit(127); // exec failed: reported as exit code 127
    }
    p.pid_ = pid;
#endif
    return p;
}

bool Process::started() const
{
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return pid_ > 0;
#endif
}

std::optional<int> Process::exit_code()
{
    if (exit_code_ || !started()) {
        return exit_code_;
    }
#if defined(_WIN32)
    if (WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(handle_, &code);
        exit_code_ = static_cast<int>(code);
        CloseHandle(handle_);
        handle_ = nullptr;
    }
#else
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) == pid_) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        pid_ = -1;
    }
#endif
    return exit_code_;
}

void Process::wait_or_kill(int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (started() && !exit_code() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill();
}

void Process::kill()
{
    if (!started() || exit_code()) {
        return;
    }
#if defined(_WIN32)
    TerminateProcess(handle_, 1);
    WaitForSingleObject(handle_, INFINITE);
    CloseHandle(handle_);
    handle_ = nullptr;
#else
    ::kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = -1;
#endif
    exit_code_ = -1;
}

void Process::detach()
{
#if defined(_WIN32)
    if (handle_ != nullptr) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
#else
    pid_ = -1; // reaped by nobody: a short-lived helper leaves a zombie until ksim exits
#endif
}

} // namespace ksim

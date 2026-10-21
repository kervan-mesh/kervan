/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "serial.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ksim::hw {

void LineSplitter::feed(const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char c = data[i];
        if (c == '\n') {
            if (!partial_.empty() && partial_.back() == '\r') {
                partial_.pop_back();
            }
            ready_.push_back(std::move(partial_));
            partial_.clear();
        } else if (partial_.size() < max_len_) {
            partial_ += c;
        }
    }
}

std::optional<std::string> LineSplitter::next()
{
    if (first_ >= ready_.size()) {
        ready_.clear();
        first_ = 0;
        return std::nullopt;
    }
    return std::move(ready_[first_++]);
}

namespace {

using Clock = std::chrono::steady_clock;

int remaining_ms(Clock::time_point deadline)
{
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

#if defined(_WIN32)

std::string win_error(DWORD code)
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return "no such port";
    case ERROR_ACCESS_DENIED:
        return "in use by another program (a serial monitor, another ksim?)";
    default:
        return "Windows error " + std::to_string(code);
    }
}

class SerialPort final : public LineIo {
public:
    explicit SerialPort(const std::string &port) : port_(port)
    {
        const std::string path = port.rfind("\\\\.\\", 0) == 0 ? port : "\\\\.\\" + port;
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                         nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(port + ": " + win_error(GetLastError()));
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h_, &dcb)) {
            const DWORD code = GetLastError();
            CloseHandle(h_);
            throw std::runtime_error(port + ": not a serial port (" + win_error(code) + ")");
        }
        dcb.BaudRate = 115200; // USB boards ignore it
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE; // a Pico only talks while DTR is set
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(h_, &dcb);
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }

    ~SerialPort() override { CloseHandle(h_); }

    void write_line(const std::string &line) override
    {
        const std::string data = line + "\n";
        timeouts(MAXDWORD);
        size_t done = 0;
        while (done < data.size()) {
            DWORD n = 0;
            if (!WriteFile(h_, data.data() + done, static_cast<DWORD>(data.size() - done), &n,
                           nullptr) ||
                n == 0) {
                throw std::runtime_error(port_ + ": cannot write (board unplugged?)");
            }
            done += n;
        }
    }

    std::optional<std::string> read_line(int timeout_ms) override
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (auto line = split_.next()) {
                return line;
            }
            const int left = remaining_ms(deadline);
            timeouts(static_cast<DWORD>(std::max(left, 1)));
            char buf[512];
            DWORD n = 0;
            if (!ReadFile(h_, buf, sizeof(buf), &n, nullptr)) {
                throw std::runtime_error(port_ + ": disconnected");
            }
            split_.feed(buf, n);
            if (n == 0 && Clock::now() >= deadline) {
                return split_.next();
            }
        }
    }

private:
    // Reads return as soon as a byte is there, or after read_ms; writes give up after 1 s.
    void timeouts(DWORD read_ms)
    {
        COMMTIMEOUTS t{};
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant = read_ms == MAXDWORD ? 1 : read_ms;
        t.WriteTotalTimeoutConstant = 1000;
        SetCommTimeouts(h_, &t);
    }

    std::string port_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    LineSplitter split_;
};

#else

class SerialPort final : public LineIo {
public:
    explicit SerialPort(const std::string &port) : port_(port)
    {
        fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            const int e = errno;
            throw std::runtime_error(port + ": " +
                                     (e == ENOENT ? std::string("no such port")
                                      : e == EACCES
                                          ? std::string("permission denied (on Linux: add "
                                                        "yourself to the dialout group)")
                                          : std::string(std::strerror(e))));
        }
        if (::ioctl(fd_, TIOCEXCL) != 0) {
            // not fatal: only keeps other programs out
        }
        termios t{};
        if (::tcgetattr(fd_, &t) != 0) {
            ::close(fd_);
            throw std::runtime_error(port + ": not a serial port");
        }
        ::cfmakeraw(&t);
        ::cfsetispeed(&t, B115200);
        ::cfsetospeed(&t, B115200);
        t.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
        t.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
        ::tcsetattr(fd_, TCSANOW, &t);
        int dtr = TIOCM_DTR;
        ::ioctl(fd_, TIOCMBIS, &dtr); // a Pico only talks while DTR is set
        ::tcflush(fd_, TCIOFLUSH);
    }

    ~SerialPort() override { ::close(fd_); }

    void write_line(const std::string &line) override
    {
        const std::string data = line + "\n";
        size_t done = 0;
        while (done < data.size()) {
            const ssize_t n = ::write(fd_, data.data() + done, data.size() - done);
            if (n > 0) {
                done += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                throw std::runtime_error(port_ + ": cannot write (board unplugged?)");
            }
            pollfd p{fd_, POLLOUT, 0};
            if (::poll(&p, 1, 1000) <= 0) {
                throw std::runtime_error(port_ + ": cannot write (board not reading)");
            }
        }
    }

    std::optional<std::string> read_line(int timeout_ms) override
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (auto line = split_.next()) {
                return line;
            }
            pollfd p{fd_, POLLIN, 0};
            const int r = ::poll(&p, 1, remaining_ms(deadline));
            if (r < 0 && errno != EINTR) {
                throw std::runtime_error(port_ + ": disconnected");
            }
            if (r > 0) {
                if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 &&
                    (p.revents & POLLIN) == 0) {
                    throw std::runtime_error(port_ + ": disconnected");
                }
                char buf[512];
                const ssize_t n = ::read(fd_, buf, sizeof(buf));
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    throw std::runtime_error(port_ + ": disconnected");
                }
                if (n > 0) {
                    split_.feed(buf, static_cast<size_t>(n));
                }
            }
            if (Clock::now() >= deadline) {
                return split_.next();
            }
        }
    }

private:
    std::string port_;
    int fd_ = -1;
    LineSplitter split_;
};

#endif

} // namespace

std::unique_ptr<LineIo> open_serial(const std::string &port)
{
    return std::make_unique<SerialPort>(port);
}

std::vector<std::string> list_serial_ports()
{
    std::vector<std::string> ports;
#if defined(_WIN32)
    char target[512];
    for (int i = 1; i <= 256; i++) {
        const std::string name = "COM" + std::to_string(i);
        if (QueryDosDeviceA(name.c_str(), target, sizeof(target)) != 0) {
            ports.push_back(name);
        }
    }
#else
    static const char *const prefixes[] = {"ttyACM", "ttyUSB", "cu.usbmodem", "cu.usbserial"};
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        for (const char *p : prefixes) {
            if (name.rfind(p, 0) == 0) {
                ports.push_back("/dev/" + name);
            }
        }
    }
#endif
    // COM2 before COM10
    std::sort(ports.begin(), ports.end(), [](const std::string &a, const std::string &b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

} // namespace ksim::hw

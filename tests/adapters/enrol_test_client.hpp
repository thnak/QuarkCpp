// Shared raw-socket client for the enrolment checks over TCP (ADR-074 D4c): a loopback client with deadline-bounded recv, and a driver that runs a
// joiner session over it. Header-only, tests only.
#pragma once

#include <array>
#include <chrono>
#include <thread>

#include "adapters/enrol_test_pki.hpp"
#include "pal/net.hpp"

namespace enrol_test {

// A raw client socket with a deadline-bounded recv. Sends/receives on the loopback.
struct Client {
    pal::fd_t fd = pal::invalid_fd;
    explicit Client(std::uint16_t port) {
        auto r = pal::tcp_connect(pal::ipv4_loopback, port);
        if (r) fd = *r;
    }
    ~Client() {
        if (fd != pal::invalid_fd) pal::close_fd(fd);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool send_all(const Bytes& b) {
        std::size_t off = 0;
        for (int spins = 0; off < b.size() && spins < 5000; ++spins) {
            auto r = pal::send_some(fd, b.data() + off, b.size() - off);
            if (r) {
                off += *r;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));  // connect still completing / would_block
        }
        return off == b.size();
    }
    // Waits up to `ms` for data or EOF. Returns what arrived in `got`; `eof` is set on an orderly close or a reset.
    void recv_for(int ms, Bytes& got, bool& eof) {
        eof = false;
        std::array<std::byte, 8192> buf;
        for (int i = 0; i < ms; ++i) {
            auto r = pal::recv_some(fd, buf.data(), buf.size());
            if (r && *r > 0) {
                got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*r));
                return;
            }
            if (r && *r == 0) {
                eof = true;
                return;
            }
            if (!r && r.error() != pal::would_block()) {
                eof = true;  // reset
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

// Runs a joiner session over a socket to completion (or until `ms` elapse).
inline EnrolJoinerSession::State drive(EnrolJoinerSession& j, Client& c, int ms) {
    Bytes out;
    j.start(out);
    for (int i = 0; i < ms && !(j.state() == EnrolJoinerSession::State::Joined || j.state() == EnrolJoinerSession::State::Rejected ||
                                 j.state() == EnrolJoinerSession::State::Failed);
         ++i) {
        if (!out.empty()) {
            if (!c.send_all(out)) break;
            out.clear();
        }
        Bytes got;
        bool eof = false;
        c.recv_for(20, got, eof);
        if (!got.empty()) j.feed(got, out);
        if (eof && got.empty()) break;
    }
    return j.state();
}
}  // namespace enrol_test

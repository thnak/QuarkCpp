// ADR-059: direct test of the Windows IOCP reactor (pal::IocpContext) — posted tasks, timers, accept/
// connect/read/write readiness, EOF, mod_fd interest change, del_fd-from-handler. Windows-only; on
// other platforms it is a no-op pass (the type does not exist there).
#include <cstdio>

#if !defined(_WIN32)
int main() { std::puts("pal_iocp_context_test: skipped (Windows-only)"); return 0; }
#else
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "pal/net.hpp"

using namespace quark::pal;

static bool g_ok = true;
static void check(bool c, const char* w) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", w); g_ok = false; }
}

int main() {
    IocpContext io;
    check(io.ok(), "AFD device opened");
    if (!io.ok()) return 1;

    std::atomic<int> posted{0};
    std::atomic<bool> timer_fired{false};
    bool accepted = false, connected = false, got_data = false, got_eof = false, client_read = false;
    fd_t lfd = invalid_fd, srv = invalid_fd, cli = invalid_fd;
    std::string rx;
    std::thread::id loop_id;
    auto maybe_stop = [&] { if (got_eof && timer_fired.load() && posted.load() == 1) io.stop(); };

    auto L = tcp_listen(ipv4_loopback, 0);
    check(L.has_value(), "listen");
    lfd = *L;
    const auto port = *local_port(lfd);

    // Everything below is registered from the loop thread (add_fd is loop-thread-only).
    io.post([&] {
        loop_id = std::this_thread::get_id();
        io.add_fd(lfd, EPOLLIN, [&](std::uint32_t) {
            for (;;) {
                auto a = accept_one(lfd);
                if (!a) break;
                accepted = true;
                srv = *a;
                io.add_fd(srv, EPOLLIN, [&](std::uint32_t ev) {
                    std::byte buf[64];
                    if (ev & EPOLLIN) {
                        for (;;) {
                            auto r = recv_some(srv, buf, sizeof(buf));
                            if (!r) break;                 // would_block
                            if (*r == 0) {                 // orderly EOF
                                got_eof = true;
                                io.del_fd(srv);            // del_fd from inside own handler
                                close_fd(srv);
                                maybe_stop();
                                return;
                            }
                            rx.append(reinterpret_cast<char*>(buf), *r);
                            got_data = true;
                            // echo back: exercises srv-side send
                            (void)send_some(srv, buf, *r);
                        }
                    }
                });
            }
        });
        auto c = tcp_connect(ipv4_loopback, port);
        check(c.has_value(), "connect");
        cli = *c;
        io.add_fd(cli, EPOLLIN | EPOLLOUT, [&](std::uint32_t ev) {
            if (ev & EPOLLOUT) {
                if (!connected) {
                    check(connect_result(cli).has_value(), "connect_result ok");
                    connected = true;
                    const char* m = "hello-iocp";
                    (void)send_some(cli, reinterpret_cast<const std::byte*>(m), 10);
                    io.mod_fd(cli, EPOLLIN);  // drop OUT interest: must not spin on writable
                }
            }
            if (ev & EPOLLIN) {
                std::byte buf[64];
                auto r = recv_some(cli, buf, sizeof(buf));
                if (r && *r > 0) {
                    client_read = true;
                    io.del_fd(cli);
                    close_fd(cli);  // triggers EOF on srv
                    cli = invalid_fd;
                }
            }
        });
        io.post_after(20'000'000, [&] { timer_fired = true; maybe_stop(); });
    });

    // cross-thread post from a foreign thread while the loop is (about to be) blocked in the port
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        io.post([&] { posted++; maybe_stop(); check(std::this_thread::get_id() == loop_id, "post runs on loop thread"); });
    });
    std::thread watchdog([&] {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        io.stop();
    });
    io.run();
    t.join();
    watchdog.join();

    check(accepted, "accepted");
    check(connected, "connect completion via EPOLLOUT");
    check(got_data && rx == "hello-iocp", "server received payload");
    check(client_read, "client read echo");
    check(got_eof, "server saw EOF after client close");
    check(timer_fired.load(), "timer fired");
    check(posted.load() == 1, "cross-thread post ran");
    close_fd(lfd);
    std::puts(g_ok ? "pal_iocp_context_test: PASS" : "pal_iocp_context_test: FAIL");
    return g_ok ? 0 : 1;
}
#endif

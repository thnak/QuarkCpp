// ADR-080 S3a: the shipped blocking SocketByteStream through the PAL. The property that matters for the
// transport's stop() is close() waking a read() parked in another thread (the reader thread), which is
// why close() shuts the socket down before closing it. Mutation control: removing shutdown_socket() from
// close() makes the woken read report an error instead of EOF (on Windows) or stay blocked, so this check
// fails; the wait is bounded, so the control does not hang.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <span>
#include <thread>

#include "pal/net.hpp"
#include "quark/client/socket_byte_stream.hpp"

using namespace quark;

namespace {
bool check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
    return cond;
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
}  // namespace

int main() {
    bool ok = true;
    auto lfd = pal::tcp_listen(pal::ipv4_loopback, 0);
    check(static_cast<bool>(lfd), "listener opens", ok);
    if (!lfd) return 1;
    const auto port = pal::local_port(*lfd);
    check(static_cast<bool>(port), "listener port is readable", ok);
    if (!port) return 1;

    auto cfd = pal::tcp_connect_blocking(pal::ipv4_loopback, *port);
    check(static_cast<bool>(cfd), "the blocking connect succeeds", ok);
    if (!cfd) return 1;

    pal::fd_t sfd = pal::invalid_fd;
    check(wait_until(
              [&] {
                  if (sfd != pal::invalid_fd) return true;
                  if (auto a = pal::accept_one(*lfd)) sfd = *a;
                  return sfd != pal::invalid_fd;
              },
              std::chrono::milliseconds(2000)),
          "the listener accepts the connection", ok);
    pal::close_fd(*lfd);
    if (sfd == pal::invalid_fd) return 1;
    check(static_cast<bool>(pal::set_blocking(sfd)), "the accepted socket becomes blocking", ok);

    client::SocketByteStream stream(sfd);
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    std::atomic<std::ptrdiff_t> read_result{-1};
    std::thread reader([&] {
        std::byte b[1];
        entered.store(true);
        read_result.store(stream.read(std::span<std::byte>(b)));
        done.store(true);
    });

    check(wait_until([&] { return entered.load(); }, std::chrono::milliseconds(2000)),
          "the reader starts", ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!done.load(), "read() is parked on the blocking socket", ok);

    stream.close();
    const bool woke = wait_until([&] { return done.load(); }, std::chrono::milliseconds(2000));
    // shutdown() first is what makes this a clean EOF; a bare close() while another thread is in recv()
    // is unspecified (on Windows it wakes the reader with an error), which is why the property is one check.
    check(woke && read_result.load() == 0, "close() unblocks a blocked read with a clean EOF", ok);
    if (woke) reader.join();
    else reader.detach();  // the process is about to fail; do not hang the test on join

    pal::close_fd(*cfd);
    std::printf("client_socket_byte_stream_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

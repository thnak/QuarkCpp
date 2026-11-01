// Implements 019-Platform-Abstraction-Layer §"Android target" and 021 §"Mobile member profile"; ADR-071.
// A tiny native runner over the C ABI of libquark_android (include/quark/android/quark_android.h): ONE cluster
// node per process, no APK, no JNI. `adb push` it to /data/local/tmp and run it in the emulator against a
// copy of the same program on the Windows host (ADR-071 tests A5/A6). It exercises exactly the surface the
// app's JNI glue will call, over real TCP, including the mobile lifecycle:
//
//   quark_mobile_runner seed  --id 1 --listen 47001 --seconds 40
//       the always-on PC: listens, logs every state change of node 2, sends a probe frame to it each
//       second, and prints a RESULT line.
//   quark_mobile_runner phone --id 2 --seed-id 1 --seed-host 127.0.0.1 --seed-port 47001
//                             --frames 50 --suspend-seconds 20 --lease-seconds 60
//       joins the seed, sends N probe frames, SUSPENDS for --suspend-seconds (real time), RESUMES, sends N
//       more, prints a RESULT line. With --network-change it also calls network_changed() after resume.
// Both exit 0 on PASS, 1 on FAIL. The check that matters (ADR-071 A6): across the suspension the seed
// NEVER reports node 2 as Suspect/Dead/Left; it reports it SUSPENDED and then ALIVE again.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "quark/android/quark_android.h"

namespace {

using Clock = std::chrono::steady_clock;

const char* arg(int argc, char** argv, const char* name, const char* def) {
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}
bool flag(int argc, char** argv, const char* name) {
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}
std::uint32_t parse_ipv4(const char* s) {
    std::uint32_t v = 0;
    const char* p = s;
    for (int i = 0; i < 4; ++i) {
        char* end = nullptr;
        const unsigned long part = std::strtoul(p, &end, 10);
        if (end == p || part > 255) return 0;
        v = (v << 8) | static_cast<std::uint32_t>(part);
        p = end;
        if (i < 3) {
            if (*p != '.') return 0;
            ++p;
        }
    }
    return v;
}
const char* state_name(int s) {
    switch (s) {
        case QUARK_PEER_ALIVE: return "ALIVE";
        case QUARK_PEER_SUSPECT: return "SUSPECT";
        case QUARK_PEER_DEAD: return "DEAD";
        case QUARK_PEER_LEFT: return "LEFT";
        case QUARK_PEER_SUSPENDED: return "SUSPENDED";
        default: return "UNKNOWN";
    }
}
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

int run_seed(int argc, char** argv) {
    const std::uint64_t id = std::strtoull(arg(argc, argv, "--id", "1"), nullptr, 10);
    const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(arg(argc, argv, "--listen", "47001")));
    const int seconds = std::atoi(arg(argc, argv, "--seconds", "40"));
    const std::uint64_t peer = std::strtoull(arg(argc, argv, "--peer-id", "2"), nullptr, 10);
    QuarkAndroidNode* n = quark_android_node_create(id, 0xA11CE, parse_ipv4("127.0.0.1"), port, 200, /*allow_insecure_dev=*/1);
    if (!n || quark_android_node_start(n) != 0) {
        std::printf("RESULT seed FAIL start\n");
        return 1;
    }
    std::printf("seed up on port %u\n", quark_android_node_port(n));
    std::fflush(stdout);
    const auto t0 = Clock::now();
    int last = -1;
    bool saw_alive = false, saw_suspended = false, saw_alive_after = false, bad = false;
    std::uint64_t probes = 0;
    double alive_after_t = 0;
    while (since(t0) < seconds) {
        const int s = quark_android_node_peer_state(n, peer);
        if (s != last) {
            std::printf("STATE t=%.1f peer=%llu %s\n", since(t0), static_cast<unsigned long long>(peer), state_name(s));
            std::fflush(stdout);
            last = s;
        }
        if (s == QUARK_PEER_ALIVE) {
            if (saw_suspended && !saw_alive_after) alive_after_t = since(t0);
            (saw_suspended ? saw_alive_after : saw_alive) = true;
        }
        if (saw_alive_after && since(t0) - alive_after_t > 2.5) break;  // resumed and stable: the phone may now exit
        if (s == QUARK_PEER_SUSPENDED) saw_suspended = true;
        if (s == QUARK_PEER_SUSPECT || s == QUARK_PEER_DEAD || s == QUARK_PEER_LEFT) bad = true;
        if (static_cast<long>(since(t0)) > static_cast<long>(probes)) quark_android_node_send_probe(n, peer, probes++);
        sleep_ms(100);
    }
    const std::uint64_t rx = quark_android_node_frames_received(n);
    const bool pass = saw_alive && saw_suspended && saw_alive_after && !bad;
    std::printf("RESULT seed %s frames_from_peer=%llu probes_sent=%llu saw_alive=%d saw_suspended=%d alive_after_resume=%d suspect_or_dead_or_left=%d\n",
                pass ? "PASS" : "FAIL", static_cast<unsigned long long>(rx), static_cast<unsigned long long>(probes),
                saw_alive, saw_suspended, saw_alive_after, bad);
    quark_android_node_destroy(n);
    return pass ? 0 : 1;
}

int run_phone(int argc, char** argv) {
    const std::uint64_t id = std::strtoull(arg(argc, argv, "--id", "2"), nullptr, 10);
    const std::uint64_t seed_id = std::strtoull(arg(argc, argv, "--seed-id", "1"), nullptr, 10);
    const std::uint32_t seed_ip = parse_ipv4(arg(argc, argv, "--seed-host", "127.0.0.1"));
    const std::uint16_t seed_port = static_cast<std::uint16_t>(std::atoi(arg(argc, argv, "--seed-port", "47001")));
    const int frames = std::atoi(arg(argc, argv, "--frames", "50"));
    const int suspend_s = std::atoi(arg(argc, argv, "--suspend-seconds", "20"));
    const int lease_s = std::atoi(arg(argc, argv, "--lease-seconds", "60"));
    QuarkAndroidNode* n = quark_android_node_create(id, 0xA11CE, 0, 0, 200, /*allow_insecure_dev=*/1);
    if (!n || quark_android_node_start(n) != 0) {
        std::printf("RESULT phone FAIL start\n");
        return 1;
    }
    quark_android_node_add_peer(n, seed_id, seed_ip, seed_port);
    quark_android_node_join(n, seed_id);
    const auto t0 = Clock::now();
    while (quark_android_node_peer_state(n, seed_id) != QUARK_PEER_ALIVE && since(t0) < 20) sleep_ms(100);
    const bool joined = quark_android_node_peer_state(n, seed_id) == QUARK_PEER_ALIVE;
    std::printf("phone joined=%d view=%llu t=%.1f\n", joined, static_cast<unsigned long long>(quark_android_node_view_size(n)), since(t0));
    std::fflush(stdout);
    for (int i = 0; i < frames; ++i) quark_android_node_send_probe(n, seed_id, static_cast<std::uint64_t>(i));
    sleep_ms(1500);

    std::printf("phone SUSPEND for %d s (lease %d s)\n", suspend_s, lease_s);
    std::fflush(stdout);
    quark_android_node_suspend(n, QUARK_SUSPEND_APP_BACKGROUND, static_cast<std::uint32_t>(lease_s) * 1000u);
    const bool suspended = quark_android_node_self_suspended(n) == 1;
    sleep_ms(suspend_s * 1000);
    quark_android_node_resume(n);
    std::printf("phone RESUMED t=%.1f\n", since(t0));
    std::fflush(stdout);
    if (flag(argc, argv, "--network-change")) {
        quark_android_node_network_changed(n);
        std::printf("phone network_changed() called\n");
    }
    for (int i = 0; i < frames; ++i) quark_android_node_send_probe(n, seed_id, 1000u + static_cast<std::uint64_t>(i));
    sleep_ms(3000);
    const bool alive_after = quark_android_node_peer_state(n, seed_id) == QUARK_PEER_ALIVE;
    const std::uint64_t rx = quark_android_node_frames_received(n);
    const bool pass = joined && suspended && alive_after;
    std::printf("RESULT phone %s joined=%d suspended=%d seed_alive_after_resume=%d frames_from_seed=%llu\n",
                pass ? "PASS" : "FAIL", joined, suspended, alive_after, static_cast<unsigned long long>(rx));
    quark_android_node_destroy(n);
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 2 && std::strcmp(argv[1], "seed") == 0) return run_seed(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "phone") == 0) return run_phone(argc, argv);
    std::fprintf(stderr, "usage: quark_mobile_runner seed|phone [options]  (see the file header)\n");
    return 2;
}

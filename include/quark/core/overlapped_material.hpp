// Implements ADR-040 (mTLS Node Transport with Live Certificate Rotation) — `OverlappedMaterial<T>`,
// the generic HOT-RELOAD primitive behind rotating a node's TLS identity (tls_identity.hpp) and its
// trust roots without a coordinated cluster restart: "current + grace-windowed previous", mutex-guarded,
// monotonic generation. Proven shape (ADR-040 Design 2, claim S1): 1 writer + 4 readers, 4M reads,
// 870,533 rotations, 0 ASan reports.
//
// CLOCK: `rotate()` takes an explicit monotonic `now_ns` (governance.hpp's convention — clock-free,
// deterministic under test) and converts `grace_ns` to an ABSOLUTE deadline immediately; `snapshot()`
// never touches a clock — it is a pure comparison against `now_ns` passed by the READER, so a read is
// one short lock + two shared_ptr copies, no syscall.
//
// TWO DISTINCT USES, deliberately not distinguished by this type (021 spec rec, ADR-040 judge note):
//   - TrustStore (OverlappedMaterial<TrustedRoots>): `previous` IS re-presented at handshake time — a
//     peer's chain verifies against the UNION of current+previous while inside the grace window (S4).
//   - IdentityMaterial (OverlappedMaterial<TlsIdentity>): a FRESH handshake always signs with `current`
//     only; `previous` exists purely so the rotation sweep can tell "this open PeerSession predates the
//     current identity but is still inside grace — leave it alone" (proven C1: 0 rotation-attributable
//     closes before the window elapses).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace quark {

template <typename T>
class OverlappedMaterial {
public:
    struct Snapshot {
        std::shared_ptr<const T> current;
        std::uint64_t generation = 0;

        // `previous` is non-null only if a rotation left one AND `now_ns` (passed to snapshot()) has
        // not yet reached `previous_deadline_ns`. Once past the deadline, snapshot() reports it absent
        // even if rotate()/evict_previous() hasn't run again — no background reaper thread needed.
        std::shared_ptr<const T> previous;
        std::uint64_t previous_generation = 0;
    };

    explicit OverlappedMaterial(std::shared_ptr<const T> initial) noexcept
        : current_(std::move(initial)) {}

    OverlappedMaterial(const OverlappedMaterial&) = delete;
    OverlappedMaterial& operator=(const OverlappedMaterial&) = delete;

    // Hot-reload: `next` becomes `current`; the outgoing material becomes `previous`, visible to
    // snapshot() readers until `now_ns + grace_ns` (0 ⇒ previous is immediately absent, the S5
    // compromise-during-rotation edge case where no overlap window should be left open at all).
    void rotate(std::shared_ptr<const T> next, std::int64_t now_ns, std::int64_t grace_ns) {
        std::lock_guard<std::mutex> g(mu_);
        previous_ = std::move(current_);
        previous_generation_ = generation_;
        previous_deadline_ns_ = grace_ns > 0 ? now_ns + grace_ns : now_ns;
        current_ = std::move(next);
        ++generation_;
    }

    // Drop `previous` immediately, independent of its deadline (e.g. the material being replaced is
    // itself known-compromised — no grace window is safe to honor).
    void evict_previous() noexcept {
        std::lock_guard<std::mutex> g(mu_);
        previous_.reset();
        previous_deadline_ns_ = 0;
    }

    [[nodiscard]] Snapshot snapshot(std::int64_t now_ns) const {
        std::lock_guard<std::mutex> g(mu_);
        Snapshot s;
        s.current = current_;
        s.generation = generation_;
        if (previous_ && now_ns < previous_deadline_ns_) {
            s.previous = previous_;
            s.previous_generation = previous_generation_;
        }
        return s;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return generation_;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const T> current_;
    std::uint64_t generation_ = 0;
    std::shared_ptr<const T> previous_;
    std::uint64_t previous_generation_ = 0;
    std::int64_t previous_deadline_ns_ = 0;
};

}  // namespace quark

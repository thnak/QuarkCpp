// Tests 008 §Type identity + 016 fingerprint (#18): same-layout message types must not share a
// durable/wire key when one declares `quark_type_salt`, internal delivery records never collide with
// a user event of the same shape, and `recover()` hands such an event to the caller's fold.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/delivery.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

struct Deposit {
    std::uint64_t account = 0;
    std::uint64_t amount = 0;
};
QUARK_SERIALIZE(Deposit, (1, account), (2, amount))

struct Withdraw {
    static constexpr std::uint64_t quark_type_salt = 0x57495448ULL;
    std::uint64_t account = 0;
    std::uint64_t amount = 0;
};
QUARK_SERIALIZE(Withdraw, (1, account), (2, amount))

// A user event with EXACTLY the layout of WatermarkRecord (the #18 recover() misrouting case).
struct UserEvent {
    std::uint64_t sender = 0;
    std::uint64_t seq = 0;
};
QUARK_SERIALIZE(UserEvent, (1, sender), (2, seq))

static_assert(fingerprint_v<Deposit> != fingerprint_v<Withdraw>, "salt separates same-layout types");
static_assert(fingerprint_v<UserEvent> != fingerprint_v<WatermarkRecord>, "internal record is salted");
static_assert(fingerprint_v<UserEvent> != fingerprint_v<OutboxRecord>);
static_assert(fingerprint_v<UserEvent> != fingerprint_v<ReplyRecord>);
static_assert(detail::protocol_keys_distinct<Deposit, Withdraw>());
static_assert(!detail::protocol_keys_distinct<UserEvent, UserEvent>(),
              "equal keys in one protocol are detected");

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;
    constexpr ActorId self{TypeKey{0xA1}, 1};
    constexpr ActorId peer{TypeKey{0xA2}, 1};

    InMemoryStore store;
    FenceToken f = *store.acquire_fence(self);
    EffectivelyOnceLane<InMemoryStore> lane(store, self, f, store.last_seq(self) + 1);

    auto c = lane.begin(MsgKey{peer.hash(), 5});
    check(c.event(UserEvent{peer.hash(), 999}).has_value(), "stage user event", ok);
    check(c.commit().has_value(), "commit", ok);

    EffectivelyOnceLane<InMemoryStore> again(store, self, *store.acquire_fence(self),
                                             store.last_seq(self) + 1);
    std::vector<std::uint64_t> seen;
    auto rc = again.recover([&](std::span<const std::byte> bytes) -> result<void> {
        auto ev = decode_durable<UserEvent>(bytes);
        if (!ev) return std::unexpected(ev.error());
        seen.push_back(ev->seq);
        return {};
    });
    check(rc.has_value(), "recover ok", ok);
    check(seen.size() == 1 && seen[0] == 999, "user event reached the caller's fold", ok);
    // The real watermark (5) must be intact, NOT overwritten by the user event's seq (999).
    check(again.is_duplicate(MsgKey{peer.hash(), 5}), "real watermark rebuilt", ok);
    check(!again.is_duplicate(MsgKey{peer.hash(), 6}), "user event did not poison the watermark", ok);

    std::puts(ok ? "type_key_collision_test: OK" : "type_key_collision_test: FAILED");
    return ok ? 0 : 1;
}

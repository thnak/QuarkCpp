// Tests 006-Messaging-and-Addressing §Actor identity / §Actor references — ActorRef identity and
// equality: `get<A>(key)` resolves identity + placement only (never blocks, never creates state);
// the same (type, key) yields equal refs, different keys/types yield distinct ones; the ref is a
// cheap value over ActorId, safe to copy.
#include <cassert>
#include <cstdio>
#include <type_traits>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct A : Actor<A, Sequential> {
    using protocol = Protocol<int>;
    void handle(const int&) noexcept {}
};
struct B : Actor<B, Sequential> {
    using protocol = Protocol<int>;
    void handle(const int&) noexcept {}
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

// ActorRef is cheap-to-copy (a value over identity + a delivery-context pointer).
static_assert(std::is_trivially_copyable_v<ActorRef<A>>, "ActorRef must be a cheap value type");

int main() {
    bool ok = true;

    detail::MessagePool pool(8);
    LocalRouter router(PostCourier{}, pool);  // get() touches only identity — no engine needed

    ActorRef<A> a1 = router.get<A>(1);
    ActorRef<A> a1b = router.get<A>(1);
    ActorRef<A> a2 = router.get<A>(2);
    ActorRef<B> b1 = router.get<B>(1);

    check(a1 == a1b, "same (type,key) ⇒ equal refs (stable identity)", ok);
    check(!(a1 == a2), "different key ⇒ distinct refs", ok);
    check(a1.id() == actor_id_of<A>(1), "ref carries the stable ActorId", ok);
    check(a1.id().type != b1.id().type, "distinct actor types have distinct TypeKeys", ok);
    check(a1.id().type == a2.id().type, "same actor type shares one TypeKey across keys", ok);

    // Copy is cheap and preserves identity.
    ActorRef<A> copy = a1;
    check(copy == a1, "copy preserves identity", ok);

    std::printf("actor_ref_identity_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

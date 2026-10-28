// ADR-062 compile-time gate (tests/CMakeLists.txt, try_compile at configure time). Built twice:
//   -DWITH_NAME     the POSITIVE CONTROL: an actor that declares quark_type_name MUST compile.
//   (no define)     an actor WITHOUT quark_type_name MUST be rejected by the ADR-062 static_assert.
#include <string_view>

#include "quark/core/actor.hpp"
#include "quark/core/metadata.hpp"

using namespace quark;

struct Msg {
    int v = 0;
};
QUARK_SERIALIZE(Msg, (1, v))

struct Gated : Actor<Gated, Sequential> {
#ifdef WITH_NAME
    static constexpr std::string_view quark_type_name = "quark.compile_fail.Gated";
#endif
    using protocol = Protocol<Msg>;
    void handle(const Msg&) {}
};

[[maybe_unused]] constexpr TypeKey kKey = type_key_of<Gated>();

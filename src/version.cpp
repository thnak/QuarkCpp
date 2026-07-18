// Minimal non-template translation unit so `quark` is a real compiled library target from
// day one (headers can then rely on a linked lib). Real subsystems add their own units here.
#include "quark/version.hpp"

namespace quark {

const char* version_string() noexcept { return "0.1.0"; }

}  // namespace quark

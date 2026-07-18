// Tests 022 §4 — weighted fair-share admission: one hot key that floods degrades ITSELF (exhausts
// its own weighted share) while a quiet key is unaffected, so a misbehaving-but-authorized source
// never starves its neighbours. Weight controls the split of a shared rate. O(1), no allocation.
#include <cassert>
#include <cstdio>

#include "quark/core/governance.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}
}  // namespace

int main() {
    const GovernanceKey a{1}, b{2}, c{3};

    // ---- Equal weights: a flood from A does not steal B's share ----------------------------
    {
        FairShare<8> fs(20.0, 10.0);  // total 20/s, total burst 10
        fs.configure(a, 1.0);
        fs.configure(b, 1.0);
        fs.finalize();  // each key: burst 5, 5/s

        // A floods: it drains its own 5-token share and is then shed.
        int a_ok = 0;
        for (int i = 0; i < 20; ++i)
            if (fs.admit(a, 0) == Admit::Accept) ++a_ok;
        check(a_ok == 5, "hot key A admitted only its weighted share (5), then shed");

        // B is untouched by A's flood — it still has its full share.
        int b_ok = 0;
        for (int i = 0; i < 20; ++i)
            if (fs.admit(b, 0) == Admit::Accept) ++b_ok;
        check(b_ok == 5, "quiet key B keeps its full share despite A's flood (no starvation)");
    }

    // ---- Weighted split: a 3:1 weighting gives 3x the share --------------------------------
    {
        FairShare<8> fs(40.0, 40.0);  // total burst 40
        fs.configure(a, 3.0);
        fs.configure(b, 1.0);
        fs.finalize();  // A: burst 30, B: burst 10

        int a_ok = 0, b_ok = 0;
        for (int i = 0; i < 100; ++i) {
            if (fs.admit(a, 0) == Admit::Accept) ++a_ok;
            if (fs.admit(b, 0) == Admit::Accept) ++b_ok;
        }
        check(a_ok == 30, "weight 3 ⇒ 30 admissions");
        check(b_ok == 10, "weight 1 ⇒ 10 admissions");
    }

    // ---- An unregistered key is not fair-share governed (Accept) ---------------------------
    {
        FairShare<8> fs(10.0, 5.0);
        fs.configure(a, 1.0);
        fs.finalize();
        int c_ok = 0;
        for (int i = 0; i < 50; ++i)
            if (fs.admit(c, 0) == Admit::Accept) ++c_ok;
        check(c_ok == 50, "unkeyed traffic is admitted (opt-in governance)");
    }

    std::printf("governance_fair_share_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}

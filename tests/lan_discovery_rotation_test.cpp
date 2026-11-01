// Tests ADR-070 D3 (021 §2 "LAN discovery adapter"; ADR-044 rotation): across a window boundary nodes
// holding the same ids still find each other. Node A rotates first (advertises window 2, accepts 2 and
// 1); node B has not rotated yet (still advertising window 1). A finds B through the PREVIOUS-window
// accept id. Control: a browser that accepts only the current window loses the not-yet-rotated B. After
// B rotates, A sees B's old advert go away (Lost) and the new one appear (Found); the old registration
// is withdrawn and the host label changes with the window. The adapter never computes an id: the test
// (the application) owns the "key" and the clock.
#include <cstdio>

#include "lan_discovery_test_util.hpp"

using namespace quark;
using namespace quark::test;

int main() {
    Check check;
    net::FakeDnsSdNetwork lan;
    Node a(lan, 0xA1, 1001), b(lan, 0xB2, 1002);
    a.open();
    b.open();
    check(a.disc.start_advertising(a.scope).has_value() && b.disc.start_advertising(b.scope).has_value(),
          "both advertise window 1");

    // A rotates; B has not.
    a.cur = 2;
    check(a.disc.window_boundary(a.scope).has_value(), "A window boundary");
    check(lan.live_registrations() == 2, "A withdrew its window-1 registration and made a window-2 one (2 live, not 3)");
    check(a.disc.start_browsing(a.scope).has_value(), "A browses");
    {
        std::lock_guard g(a.m);
        check(a.found.size() == 1, "A (accept {2,1}) finds not-yet-rotated B");
        check(!a.found.empty() && a.found[0].previous_window, "...and reports it as a previous-window match");
        check(!a.found.empty() && a.found[0].port == 1002, "...with B's listening port");
    }

    // Control: current-window-only browsing loses the not-yet-rotated peer.
    {
        Node c(lan, 0xC3, 1003);
        c.cur = 2;
        c.accept_previous = false;
        c.open();
        check(c.disc.start_browsing(c.scope).has_value(), "control browses");
        std::lock_guard g(c.m);
        bool saw_a = false, saw_b = false;
        for (auto& p : c.found) {
            saw_a |= p.port == 1001;
            saw_b |= p.port == 1002;
        }
        check(saw_a && !saw_b, "CONTROL current-only: finds rotated A, loses not-yet-rotated B");
        std::printf("  control: current-only found A=%d B=%d (expected 1/0)\n", saw_a, saw_b);
    }

    // B rotates: A sees B's old advert withdrawn and the new one appear.
    b.cur = 2;
    check(b.disc.window_boundary(b.scope).has_value(), "B window boundary");
    {
        std::lock_guard g(a.m);
        check(a.lost.size() == 1, "A is told B's window-1 advert went away");
        check(a.found.size() == 2, "A finds B again under window 2");
        check(a.found.size() == 2 && !a.found[1].previous_window, "...as a current-window match");
    }
    // Host label rotates with the window (application-supplied each time).
    const auto log = lan.adverts_seen();
    bool labels_differ = false;
    for (auto& x : log)
        for (auto& y : log)
            if (x.port == y.port && x.host_label != y.host_label) labels_differ = true;
    check(labels_differ, "host label changed across the boundary (it is supplied per window)");

    // The window two boundaries ago is no longer accepted: A at window 3 does not accept window 1.
    Node d(lan, 0xD4, 1004);  // known to the application before A's boundary, so A's lists include it
    d.cur = 1;
    a.cur = 3;
    (void)a.disc.window_boundary(a.scope);
    d.open();
    (void)d.disc.start_advertising(d.scope);
    {
        std::lock_guard g(a.m);
        bool saw_d = false;
        for (auto& p : a.found) saw_d |= p.port == 1004;
        check(!saw_d, "window 1 is two windows old at A (cur 3): not accepted");
    }
    // Control: the same device advertising the PREVIOUS window (2) of A (cur 3) is accepted.
    d.cur = 2;
    (void)d.disc.window_boundary(d.scope);
    {
        std::lock_guard g(a.m);
        bool saw_d2 = false;
        for (auto& p : a.found) saw_d2 |= p.port == 1004;
        check(saw_d2, "CONTROL: D rotated to window 2 (A's previous window): accepted");
    }

    std::printf("lan_discovery_rotation_test: %s\n", check.ok ? "OK" : "FAIL");
    return check.ok ? 0 : 1;
}

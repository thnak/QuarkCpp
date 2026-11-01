// Implements 021-Cluster-Formation-and-Lifecycle §"Mobile member profile (ADR-071)" and
// 019-Platform-Abstraction-Layer §"Android target" (the lifecycle contract). The HOST-side entry point of the
// mobile lifecycle: the application (Kotlin/JNI glue on Android) calls suspend(reason) / resume() /
// network_changed() on this object; Quark never calls up into Java. The app owns everything the OS makes
// the app's job: the foreground service, WifiManager.MulticastLock, NsdManager, Doze/standby handling and
// the decision of WHEN to suspend. This class turns those three calls into the membership and transport
// actions that make the node a well-behaved mobile member:
//
//   suspend(reason, lease)  -> SwimMembership::suspend: announce a graceful, bounded-lease SUSPEND to every
//                              live peer (so none raises suspicion), then go quiet (no probes/gossip/acks).
//   resume()                -> SwimMembership::resume: bump incarnation, forgive stale local timers, tell
//                              every peer at once; peers clear the lease on that first frame (no churn).
//   network_changed()       -> drop every live peer connection through the transport's existing
//                              reset_peer_connection seam (which salvages the one unstarted frame, keeps
//                              the per-peer queue and re-arms a jittered reconnect; SecureTransport's
//                              peer-down hook then retires the dead session so the replay guard restarts
//                              on a fresh one), and forgive the membership's path-dependent state.
//
// Threading: like SwimMembership, call on the node's single driving thread (post() it onto the
// TcpTransport loop when the host's lifecycle callbacks arrive on a Java thread).
#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "quark/core/cluster.hpp"
#include "quark/core/mobile_lease.hpp"

namespace quark {

class MobileMember {
public:
    // `reset_connection(peer)`: force-drop the live connection to `peer` WITHOUT declaring it dead and let the
    // transport reconnect (for net::TcpTransport: `[&tcp](NodeId p){ tcp.reset_peer_connection(p); }`).
    // May be empty for a transport with no connection state (the in-process loopback).
    using ResetConnectionFn = std::function<void(NodeId)>;

    MobileMember(SwimMembership& swim, ResetConnectionFn reset_connection, MobileProfile profile = {})
        : swim_(&swim), reset_(std::move(reset_connection)), profile_(profile) {}

    // The app is going to the background / Doze / lost its network. `lease_ns <= 0` uses the profile default.
    void suspend(SuspendReason why = SuspendReason::AppBackground, std::int64_t lease_ns = 0) {
        if (swim_->self_suspended()) return;
        swim_->suspend(why, lease_ns > 0 ? lease_ns : profile_.default_lease_ns);
        ++suspends_;
    }

    // The app is running again. Safe to call without a prior suspend (no-op).
    void resume() {
        if (!swim_->self_suspended()) return;
        swim_->resume();
        ++resumes_;
    }

    // The default network changed. A no-op while suspended (the reconnect then happens on resume's traffic,
    // over whatever path exists by then; the dead connections are dropped lazily by the transport).
    void network_changed() {
        if (swim_->self_suspended()) return;
        if (reset_) {
            const MembershipView view = swim_->view();  // hold the snapshot: nodes() points into it
            for (const NodeId peer : view.nodes())
                if (peer != swim_->self()) reset_(peer);
        }
        swim_->network_changed();
        ++network_changes_;
    }

    [[nodiscard]] bool suspended() const noexcept { return swim_->self_suspended(); }
    [[nodiscard]] std::uint64_t suspends() const noexcept { return suspends_; }
    [[nodiscard]] std::uint64_t resumes() const noexcept { return resumes_; }
    [[nodiscard]] std::uint64_t network_changes() const noexcept { return network_changes_; }

private:
    SwimMembership* swim_;
    ResetConnectionFn reset_;
    MobileProfile profile_;
    std::uint64_t suspends_ = 0;
    std::uint64_t resumes_ = 0;
    std::uint64_t network_changes_ = 0;
};

}  // namespace quark

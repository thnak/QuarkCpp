// Implements 021-Cluster-Formation-and-Lifecycle §2 "LAN discovery adapter (DNS-SD)" (Draft) and
// ADR-070 (issue #42) — a Discovery ADAPTER outside the core. It finds peers on the local network
// with no configured address, advertising ONLY an opaque, application-supplied, rotating instance id.
//
// What this header is: the adapter (`LanDiscovery`), its platform seam (`DnsSdPlatform`), and the
// wire-visible encoding (`encode_instance_label`). The actual mDNS/DNS-SD calls live behind
// `DnsSdPlatform`: tests use the in-memory fake (lan_discovery_fake.hpp), Windows has an opt-in
// implementation (adapters/dnssd/windows_dnssd.hpp, QUARK_WITH_DNSSD), and an application that routes OS
// authority through its own layer (AeroCoWorker HAL, invariant A2) supplies its own.
//
// Rules this file enforces (each has a test, ADR-070 §3):
//   * DISCOVERY GRANTS NOTHING. The output is `DiscoveredPeer` (an address + the id the peer
//     advertised). It carries no NodeId and no trust; it only feeds the normal handshake, and
//     admission (NodeAuthority) still decides.
//   * The advertisement contains the application-supplied service type, the application-supplied
//     host label, the encoded application-supplied id bytes, the listening port, and TXT pairs
//     ONLY if the application supplied them. Nothing is derived from a host name, user name, node id.
//   * Quark never sees the group key and never owns the clock: the application hands over a window
//     provider and signals boundaries (`window_boundary`); the adapter advertises the CURRENT id and
//     browses for current AND previous ids.
//   * ONE ID PER DEVICE, EXACT MATCH (AeroCoWorker ADR-044 R16, resolves ADR-070 F1): each device
//     advertises its own rotating id (no shared group prefix, no suffix), and a browser accepts the
//     EXACT names of every other active device (current + previous window, ~196 names per scope). A
//     sighting is accepted only when its decoded label bytes EQUAL an accepted id; there is no prefix
//     matching. The host label must be random per window (ADR-070 F2): the application supplies it.
//   * Detection of client isolation is passive-only; `visibility()` reports Unknown (ADR-070 §5).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/error.hpp"

namespace quark::net {

using DnsSdBytes = std::vector<std::byte>;

// ---- The advertised-name encoding -----------------------------------------------------------------
// RFC 4648 base32, lowercase, no padding. DNS labels are limited to 63 octets, so at most 39 bytes.
inline constexpr std::size_t kMaxInstanceIdBytes = 36;

[[nodiscard]] inline std::string encode_instance_label(std::span<const std::byte> bytes) {
    static constexpr char kAlpha[] = "abcdefghijklmnopqrstuvwxyz234567";
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::byte b : bytes) {
        acc = (acc << 8) | static_cast<std::uint32_t>(b);
        bits += 8;
        while (bits >= 5) {
            out.push_back(kAlpha[(acc >> (bits - 5)) & 31u]);
            bits -= 5;
        }
        acc &= (1u << bits) - 1u;
    }
    if (bits > 0) out.push_back(kAlpha[(acc << (5 - bits)) & 31u]);
    return out;
}

// Strict inverse: lowercase alphabet only, canonical (zero trailing bits). nullopt for anything else.
[[nodiscard]] inline std::optional<DnsSdBytes> decode_instance_label(std::string_view label) {
    if (label.empty() || label.size() > (kMaxInstanceIdBytes * 8 + 4) / 5) return std::nullopt;
    DnsSdBytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : label) {
        std::uint32_t v;
        if (c >= 'a' && c <= 'z') v = static_cast<std::uint32_t>(c - 'a');
        else if (c >= '2' && c <= '7') v = static_cast<std::uint32_t>(c - '2') + 26u;
        else return std::nullopt;
        acc = (acc << 5) | v;
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<std::byte>((acc >> (bits - 8)) & 0xFFu));
            bits -= 8;
        }
        acc &= (1u << bits) - 1u;
    }
    if (acc != 0) return std::nullopt;  // non-canonical trailing bits
    return out;
}

// ---- The platform seam ----------------------------------------------------------------------------
// Exactly what is handed to the OS (or a fake). A test asserts this struct byte-for-byte (D2).
struct DnsSdAdvert {
    std::string service_type;    // e.g. "_example._tcp" — application-supplied, never hard-coded here
    std::string instance_label;  // encode_instance_label(application id bytes)
    std::string host_label;      // application-supplied DNS-SD host label (A/AAAA owner name)
    std::uint32_t ipv4 = 0;      // host order; 0 = let the platform choose its own addresses
    std::uint16_t port = 0;
    std::vector<std::pair<std::string, std::string>> txt;  // empty unless the application supplied it
    friend bool operator==(const DnsSdAdvert&, const DnsSdAdvert&) = default;
};

// A resolved service as seen on the network (what browse yields).
struct DnsSdSighting {
    std::string service_type;
    std::string instance_label;
    std::string host_label;
    std::uint32_t ipv4 = 0;
    std::uint16_t port = 0;
};

enum class DnsSdEventKind : std::uint8_t { Found, Lost };
struct DnsSdEvent {
    DnsSdEventKind kind = DnsSdEventKind::Found;
    DnsSdSighting what;
};

using DnsSdHandle = std::uint64_t;

// The OS seam. Implementations are cold-path. The event callback may be invoked from any thread,
// including synchronously from inside `browse` (the adapter tolerates both).
class DnsSdPlatform {
public:
    virtual ~DnsSdPlatform() = default;
    [[nodiscard]] virtual result<DnsSdHandle> register_service(const DnsSdAdvert& advert) = 0;
    virtual void deregister_service(DnsSdHandle h) = 0;
    [[nodiscard]] virtual result<DnsSdHandle> browse(std::string_view service_type,
                                                     std::function<void(const DnsSdEvent&)> on_event) = 0;
    virtual void stop_browse(DnsSdHandle h) = 0;
};

// ---- Application-facing types ---------------------------------------------------------------------
// One rotation window, as the application computes it (ADR-044 R16: this device's id = a truncated
// HMAC of window || device key; every device has its own id). Quark treats every byte string as opaque.
struct DiscoveryWindow {
    // Encoded into the instance label and advertised: THIS device's own id for the window. It must be
    // unique per device (mDNS instance names are unique per network; ADR-070 §4 F1, resolved by R16).
    DnsSdBytes advertise_id;
    // The exact ids of every other active device for the CURRENT window. A sighting is accepted only
    // when its decoded label bytes are EQUAL to one of these (no prefix matching). Empty ids ignored.
    std::vector<DnsSdBytes> accept_ids;
    // Must be application-supplied and random per window (ADR-070 §4 F2).
    std::string host_label;
    // The exact ids of every other active device for the PREVIOUS window (the overlap while peers
    // have not rotated yet). A match here reports DiscoveredPeer::previous_window. Empty ids ignored.
    std::vector<DnsSdBytes> accept_previous_ids;
};
using WindowProvider = std::function<DiscoveryWindow()>;

struct DiscoveryScopeConfig {
    std::string service_type;  // application-supplied
    std::uint16_t port = 0;    // where this node LISTENS for the normal handshake
    std::uint32_t ipv4 = 0;    // advertised address, host order; 0 = platform chooses
    std::vector<std::pair<std::string, std::string>> txt;  // none unless the application supplies
    WindowProvider window;
};

struct DiscoveredPeer {
    std::uint64_t scope = 0;
    DnsSdBytes instance_id;  // the full decoded label: exactly one of the accepted ids (ADR-044 R16)
    std::string host_label;
    std::uint32_t ipv4 = 0;  // host order
    std::uint16_t port = 0;
    bool previous_window = false;  // matched via accept_previous_ids (and not accept_ids)
};

// "No peers visible" vs "no peers present" cannot be told apart passively (ADR-070 §5).
enum class NetworkVisibility : std::uint8_t { Unknown };

using ScopeId = std::uint64_t;

// The adapter. Thread-safe; a recursive mutex lets the fake/real platform call back synchronously.
// `on_found`/`on_lost` run with that lock held on the calling thread: do not block in them.
class LanDiscovery {
public:
    using FoundFn = std::function<void(const DiscoveredPeer&)>;
    LanDiscovery(DnsSdPlatform& platform, FoundFn on_found, FoundFn on_lost = {})
        : platform_(platform), on_found_(std::move(on_found)), on_lost_(std::move(on_lost)) {}
    ~LanDiscovery() {
        std::lock_guard g(m_);
        for (auto& [id, s] : scopes_) teardown(s);
    }
    LanDiscovery(const LanDiscovery&) = delete;
    LanDiscovery& operator=(const LanDiscovery&) = delete;

    [[nodiscard]] result<ScopeId> open_scope(DiscoveryScopeConfig cfg) {
        if (cfg.service_type.empty() || !cfg.window) return fail(errc::validation, "scope needs service_type and window");
        std::lock_guard g(m_);
        const ScopeId id = ++next_scope_;
        Scope s;
        s.cfg = std::move(cfg);
        scopes_.emplace(id, std::move(s));
        return id;
    }
    void close_scope(ScopeId id) {
        std::lock_guard g(m_);
        auto it = scopes_.find(id);
        if (it == scopes_.end()) return;
        teardown(it->second);
        scopes_.erase(it);
    }

    [[nodiscard]] result<void> start_advertising(ScopeId id) {
        std::lock_guard g(m_);
        Scope* s = find(id);
        if (!s) return fail(errc::not_found, "scope");
        if (s->adv) return {};
        set_window(*s);
        return register_current(*s);
    }
    void stop_advertising(ScopeId id) {
        std::lock_guard g(m_);
        if (Scope* s = find(id)) unregister(*s);
    }
    [[nodiscard]] result<void> start_browsing(ScopeId id) {
        std::lock_guard g(m_);
        Scope* s = find(id);
        if (!s) return fail(errc::not_found, "scope");
        if (s->browse) return {};
        set_window(*s);
        s->browse = 0;  // marks browsing before the platform may call back synchronously
        auto h = platform_.browse(s->cfg.service_type,
                                  [this, id](const DnsSdEvent& e) { on_event(id, e); });
        s = find(id);
        if (!h) {
            if (s) s->browse.reset();
            return std::unexpected(h.error());
        }
        if (s) s->browse = *h;
        return {};
    }
    void stop_browsing(ScopeId id) {
        std::lock_guard g(m_);
        if (Scope* s = find(id)) stop_browse(*s);
    }

    // The application signals a window boundary (it owns the clock and the key). The adapter re-reads
    // the provider, re-advertises under the new id if advertising, and widens/narrows what it accepts.
    // Peers already found stay found; peers that rotate later are matched through the new accept set.
    [[nodiscard]] result<void> window_boundary(ScopeId id) {
        std::lock_guard g(m_);
        Scope* s = find(id);
        if (!s) return fail(errc::not_found, "scope");
        set_window(*s);
        if (s->adv) {
            unregister(*s);
            return register_current(*s);
        }
        return {};
    }

    [[nodiscard]] bool advertising(ScopeId id) const {
        std::lock_guard g(m_);
        auto it = scopes_.find(id);
        return it != scopes_.end() && it->second.adv.has_value();
    }
    [[nodiscard]] bool browsing(ScopeId id) const {
        std::lock_guard g(m_);
        auto it = scopes_.find(id);
        return it != scopes_.end() && it->second.browse.has_value();
    }
    [[nodiscard]] NetworkVisibility visibility(ScopeId) const noexcept { return NetworkVisibility::Unknown; }

private:
    struct Scope {
        DiscoveryScopeConfig cfg;
        DiscoveryWindow win;
        std::unordered_map<std::string, bool> accept;  // exact id bytes -> previous window?
        std::optional<DnsSdHandle> adv, browse;
        std::string own_label;                       // so we never report ourselves
        std::map<std::string, DiscoveredPeer> seen;  // key = instance_label|ipv4|port
    };

    // Every assignment of Scope::win goes through here so the O(1) lookup is never stale.
    // Value: true = previous window. Current wins when an id is in both lists.
    static void set_window_into(Scope& s, DiscoveryWindow w) {
        s.win = std::move(w);
        s.accept.clear();
        s.accept.reserve(s.win.accept_ids.size() + s.win.accept_previous_ids.size());
        auto key = [](const DnsSdBytes& b) {
            return std::string(reinterpret_cast<const char*>(b.data()), b.size());
        };
        for (const auto& id : s.win.accept_previous_ids)
            if (!id.empty()) s.accept.emplace(key(id), true);
        for (const auto& id : s.win.accept_ids)
            if (!id.empty()) s.accept.insert_or_assign(key(id), false);
    }
    static void set_window(Scope& s) { set_window_into(s, s.cfg.window()); }

    Scope* find(ScopeId id) {
        auto it = scopes_.find(id);
        return it == scopes_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] result<void> register_current(Scope& s) {
        if (s.win.advertise_id.empty() || s.win.advertise_id.size() > kMaxInstanceIdBytes ||
            s.win.host_label.empty())
            return fail(errc::validation, "window needs an id (<=36 bytes) and a host label");
        DnsSdAdvert a;
        a.service_type = s.cfg.service_type;
        a.instance_label = encode_instance_label(s.win.advertise_id);
        a.host_label = s.win.host_label;
        a.ipv4 = s.cfg.ipv4;
        a.port = s.cfg.port;
        a.txt = s.cfg.txt;
        s.own_label = a.instance_label;  // set first: a synchronous echo must not report us
        auto h = platform_.register_service(a);
        if (!h) {
            s.own_label.clear();
            return std::unexpected(h.error());
        }
        s.adv = *h;
        return {};
    }
    void unregister(Scope& s) {
        if (!s.adv) return;
        platform_.deregister_service(*s.adv);
        s.adv.reset();
    }
    void stop_browse(Scope& s) {
        if (!s.browse) return;
        platform_.stop_browse(*s.browse);
        s.browse.reset();
        s.seen.clear();
    }
    void teardown(Scope& s) {
        unregister(s);
        stop_browse(s);
    }

    void on_event(ScopeId id, const DnsSdEvent& e) {
        std::lock_guard g(m_);
        Scope* s = find(id);
        if (!s || !s->browse || e.what.service_type != s->cfg.service_type) return;
        if (e.what.instance_label == s->own_label) return;  // our own advert
        auto bytes = decode_instance_label(e.what.instance_label);
        if (!bytes) return;
        auto m = s->accept.find(std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        if (m == s->accept.end()) return;  // not the exact id of a known device in the current or previous window
        const bool previous = m->second;
        const std::string key = e.what.instance_label + '|' + std::to_string(e.what.ipv4) + '|' +
                                std::to_string(e.what.port);
        if (e.kind == DnsSdEventKind::Found) {
            if (s->seen.contains(key)) return;
            DiscoveredPeer p{id, std::move(*bytes), e.what.host_label, e.what.ipv4, e.what.port, previous};
            auto it = s->seen.emplace(key, std::move(p)).first;
            if (on_found_) on_found_(it->second);
        } else {
            auto it = s->seen.find(key);
            if (it == s->seen.end()) return;
            DiscoveredPeer p = std::move(it->second);
            s->seen.erase(it);
            if (on_lost_) on_lost_(p);
        }
    }

    DnsSdPlatform& platform_;
    FoundFn on_found_, on_lost_;
    mutable std::recursive_mutex m_;
    std::map<ScopeId, Scope> scopes_;
    ScopeId next_scope_ = 0;
};

}  // namespace quark::net

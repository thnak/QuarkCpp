// Implements ADR-070 §2 (tests) — an in-memory DnsSdPlatform: one `FakeDnsSdNetwork` stands for a LAN
// (multicast domain), each node gets its own `FakeDnsSdPlatform` handle onto it. Models what real
// mDNS does that the adapter must survive: browsers see existing and later registrations, goodbye on
// deregister, and an instance-name CONFLICT (same type + instance label from another owner is
// refused, as mDNS probing would). `adverts_seen()` records every DnsSdAdvert handed to any platform
// so a test can assert the wire-visible bytes (D2). `set_isolated` models client isolation (no
// delivery between owners); it is a test knob, the adapter cannot observe it (ADR-070 §5).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "quark/net/lan_discovery.hpp"

namespace quark::net {

class FakeDnsSdNetwork {
public:
    [[nodiscard]] std::vector<DnsSdAdvert> adverts_seen() const {
        std::lock_guard g(m_);
        return log_;
    }
    [[nodiscard]] std::size_t live_registrations() const {
        std::lock_guard g(m_);
        return regs_.size();
    }
    void set_isolated(bool on) {
        std::lock_guard g(m_);
        isolated_ = on;
    }

private:
    friend class FakeDnsSdPlatform;
    struct Reg {
        int owner;
        DnsSdAdvert advert;
    };
    struct Browser {
        int owner;
        std::string type;
        std::function<void(const DnsSdEvent&)> cb;
    };
    static DnsSdSighting sight(const DnsSdAdvert& a) {
        return DnsSdSighting{a.service_type, a.instance_label, a.host_label, a.ipv4, a.port};
    }

    mutable std::mutex m_;
    std::map<DnsSdHandle, Reg> regs_;
    std::map<DnsSdHandle, Browser> browsers_;
    std::vector<DnsSdAdvert> log_;
    DnsSdHandle next_ = 0;
    bool isolated_ = false;
    int next_owner_ = 0;
};

class FakeDnsSdPlatform final : public DnsSdPlatform {
public:
    explicit FakeDnsSdPlatform(FakeDnsSdNetwork& net) : net_(net) {
        std::lock_guard g(net_.m_);
        owner_ = ++net_.next_owner_;
    }

    [[nodiscard]] result<DnsSdHandle> register_service(const DnsSdAdvert& a) override {
        std::vector<std::pair<std::function<void(const DnsSdEvent&)>, DnsSdEvent>> out;
        DnsSdHandle h;
        {
            std::lock_guard g(net_.m_);
            net_.log_.push_back(a);
            for (auto& [id, r] : net_.regs_)
                if (r.advert.service_type == a.service_type && r.advert.instance_label == a.instance_label &&
                    r.owner != owner_)
                    return fail(errc::validation, "dns-sd instance name conflict");
            h = ++net_.next_;
            net_.regs_.emplace(h, FakeDnsSdNetwork::Reg{owner_, a});
            if (!net_.isolated_)
                for (auto& [id, b] : net_.browsers_)
                    if (b.owner != owner_ && b.type == a.service_type)
                        out.emplace_back(b.cb, DnsSdEvent{DnsSdEventKind::Found, FakeDnsSdNetwork::sight(a)});
        }
        for (auto& [cb, ev] : out) cb(ev);
        return h;
    }
    void deregister_service(DnsSdHandle h) override {
        std::vector<std::pair<std::function<void(const DnsSdEvent&)>, DnsSdEvent>> out;
        {
            std::lock_guard g(net_.m_);
            auto it = net_.regs_.find(h);
            if (it == net_.regs_.end()) return;
            const DnsSdAdvert a = it->second.advert;
            net_.regs_.erase(it);
            if (!net_.isolated_)
                for (auto& [id, b] : net_.browsers_)
                    if (b.owner != owner_ && b.type == a.service_type)
                        out.emplace_back(b.cb, DnsSdEvent{DnsSdEventKind::Lost, FakeDnsSdNetwork::sight(a)});
        }
        for (auto& [cb, ev] : out) cb(ev);
    }
    [[nodiscard]] result<DnsSdHandle> browse(std::string_view type,
                                             std::function<void(const DnsSdEvent&)> cb) override {
        std::vector<DnsSdEvent> existing;
        DnsSdHandle h;
        {
            std::lock_guard g(net_.m_);
            h = ++net_.next_;
            net_.browsers_.emplace(h, FakeDnsSdNetwork::Browser{owner_, std::string(type), cb});
            if (!net_.isolated_)
                for (auto& [id, r] : net_.regs_)
                    if (r.owner != owner_ && r.advert.service_type == type)
                        existing.push_back(DnsSdEvent{DnsSdEventKind::Found, FakeDnsSdNetwork::sight(r.advert)});
        }
        for (auto& ev : existing) cb(ev);
        return h;
    }
    void stop_browse(DnsSdHandle h) override {
        std::lock_guard g(net_.m_);
        net_.browsers_.erase(h);
    }

private:
    FakeDnsSdNetwork& net_;
    int owner_ = 0;
};

}  // namespace quark::net

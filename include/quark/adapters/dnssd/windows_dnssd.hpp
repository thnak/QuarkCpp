// Implements 021 §2 "LAN discovery adapter" (Draft) and ADR-070 §2 — the Windows DnsSdPlatform over the
// dnsapi DNS-SD calls (DnsServiceRegister / DnsServiceDeRegister / DnsServiceBrowse /
// DnsServiceResolve; Windows 10 1809+). OPT-IN: built only with QUARK_WITH_DNSSD, never in the core
// (019: OS authority behind the PAL/adapter seam). An application that must route OS calls through its
// own layer supplies its own DnsSdPlatform instead (AeroCoWorker HAL, invariant A2).
//
// Leaks this implementation cannot avoid, recorded in ADR-070 §4:
//   * Windows requires a host name to register; it is the application-supplied `host_label` (+ ".local").
//     If the application passes a stable label, that label is a stable identifier on the wire.
//   * An A record for `host_label` carries the advertised IPv4 (a LAN-visible address; not a name).
//   * The listening port is in the SRV record.
//   * Windows may also answer mDNS queries for the machine's own computer name via the OS resolver,
//     outside this adapter; that is the OS's behaviour and is not controlled here.
//
// OS callbacks only enqueue; a dispatcher thread delivers events, so a platform call made while the
// adapter holds its lock can never deadlock against an OS callback thread.
#pragma once

#if !defined(_WIN32)
#error "adapters/dnssd/windows_dnssd.hpp is the Windows DnsSdPlatform; other OSes need their own."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <windns.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "quark/net/lan_discovery.hpp"

namespace quark::net {

class WindowsDnsSdPlatform final : public DnsSdPlatform {
public:
    WindowsDnsSdPlatform() : worker_([this] { run(); }) {}
    ~WindowsDnsSdPlatform() override {
        std::vector<DnsSdHandle> regs, brs;
        {
            std::lock_guard g(m_);
            for (auto& [h, r] : regs_) regs.push_back(h);
            for (auto& [h, b] : browses_) brs.push_back(h);
        }
        for (auto h : brs) stop_browse(h);
        for (auto h : regs) deregister_service(h);
        {
            std::lock_guard g(qm_);
            quit_ = true;
        }
        qcv_.notify_all();
        worker_.join();
    }
    WindowsDnsSdPlatform(const WindowsDnsSdPlatform&) = delete;
    WindowsDnsSdPlatform& operator=(const WindowsDnsSdPlatform&) = delete;

    // Blocks (bounded, 8 s) until the OS reports registration complete; the OS call is asynchronous.
    [[nodiscard]] result<DnsSdHandle> register_service(const DnsSdAdvert& a) override {
        const std::wstring full = widen(a.instance_label + "." + a.service_type + ".local");
        const std::wstring host = widen(a.host_label + ".local");
        std::vector<std::wstring> keys, vals;
        for (auto& [k, v] : a.txt) {
            keys.push_back(widen(k));
            vals.push_back(widen(v));
        }
        std::vector<PCWSTR> kp, vp;
        for (auto& k : keys) kp.push_back(k.c_str());
        for (auto& v : vals) vp.push_back(v.c_str());
        IP4_ADDRESS ip4 = a.ipv4 ? htonl(a.ipv4) : 0;
        PDNS_SERVICE_INSTANCE inst = DnsServiceConstructInstance(
            full.c_str(), host.c_str(), a.ipv4 ? &ip4 : nullptr, nullptr, a.port, 0, 0,
            static_cast<DWORD>(kp.size()), kp.empty() ? nullptr : kp.data(), vp.empty() ? nullptr : vp.data());
        if (!inst) return fail(errc::internal, "DnsServiceConstructInstance");

        auto ctx = std::make_shared<RegCtx>();
        ctx->self = this;
        DNS_SERVICE_REGISTER_REQUEST req{};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex = 0;
        req.pServiceInstance = inst;
        req.pRegisterCompletionCallback = &WindowsDnsSdPlatform::on_register_done;
        req.pQueryContext = ctx.get();
        req.unicastEnabled = FALSE;
        const DWORD rc = DnsServiceRegister(&req, nullptr);
        if (rc != DNS_REQUEST_PENDING) {
            DnsServiceFreeInstance(inst);
            return fail(errc::unavailable, "DnsServiceRegister");
        }
        {
            std::unique_lock g(ctx->m);
            if (!ctx->cv.wait_for(g, std::chrono::seconds(8), [&] { return ctx->done; })) {
                g.unlock();
                DnsServiceDeRegister(&req, nullptr);
                // keep ctx alive for a late callback: leak the shared_ptr intentionally into the map
                std::lock_guard gg(m_);
                orphan_.push_back(ctx);
                DnsServiceFreeInstance(inst);
                return fail(errc::timeout, "DnsServiceRegister completion");
            }
            if (ctx->status != ERROR_SUCCESS) {
                g.unlock();
                DnsServiceFreeInstance(inst);
                return fail(errc::unavailable, "DnsServiceRegister status");
            }
        }
        std::lock_guard g(m_);
        const DnsSdHandle h = ++next_;
        regs_.emplace(h, Reg{inst, ctx});
        return h;
    }
    void deregister_service(DnsSdHandle h) override {
        Reg r;
        {
            std::lock_guard g(m_);
            auto it = regs_.find(h);
            if (it == regs_.end()) return;
            r = it->second;
            regs_.erase(it);
        }
        r.ctx->reset();
        DNS_SERVICE_REGISTER_REQUEST req{};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.pServiceInstance = r.inst;
        req.pRegisterCompletionCallback = &WindowsDnsSdPlatform::on_register_done;
        req.pQueryContext = r.ctx.get();
        if (DnsServiceDeRegister(&req, nullptr) == DNS_REQUEST_PENDING) {
            std::unique_lock g(r.ctx->m);
            r.ctx->cv.wait_for(g, std::chrono::seconds(8), [&] { return r.ctx->done; });
        }
        DnsServiceFreeInstance(r.inst);
    }

    [[nodiscard]] result<DnsSdHandle> browse(std::string_view type,
                                             std::function<void(const DnsSdEvent&)> cb) override {
        auto b = std::make_shared<Browse>();
        b->self = this;
        b->type = std::string(type);
        b->cb = std::move(cb);
        b->query = widen(b->type + ".local");
        DnsSdHandle h;
        {
            std::lock_guard g(m_);
            h = ++next_;
            b->handle = h;
            browses_.emplace(h, b);
        }
        DNS_SERVICE_BROWSE_REQUEST req{};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex = 0;
        req.QueryName = b->query.c_str();
        req.pBrowseCallback = &WindowsDnsSdPlatform::on_browse;
        req.pQueryContext = b.get();
        const DWORD rc = DnsServiceBrowse(&req, &b->cancel);
        if (rc != DNS_REQUEST_PENDING) {
            std::lock_guard g(m_);
            browses_.erase(h);
            return fail(errc::unavailable, "DnsServiceBrowse");
        }
        return h;
    }
    void stop_browse(DnsSdHandle h) override {
        std::shared_ptr<Browse> b;
        {
            std::lock_guard g(m_);
            auto it = browses_.find(h);
            if (it == browses_.end()) return;
            b = it->second;
            browses_.erase(it);
        }
        b->stopped = true;
        DnsServiceBrowseCancel(&b->cancel);
        std::lock_guard g(m_);
        retired_.push_back(b);  // OS callbacks may still hold the raw pointer; keep it alive
    }

private:
    struct RegCtx {
        WindowsDnsSdPlatform* self = nullptr;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        DWORD status = 0;
        void reset() {
            std::lock_guard g(m);
            done = false;
        }
    };
    struct Reg {
        PDNS_SERVICE_INSTANCE inst = nullptr;
        std::shared_ptr<RegCtx> ctx;
    };
    struct Browse {
        WindowsDnsSdPlatform* self = nullptr;
        std::string type;
        std::wstring query;
        std::function<void(const DnsSdEvent&)> cb;
        DNS_SERVICE_CANCEL cancel{};
        DnsSdHandle handle = 0;
        std::atomic<bool> stopped{false};
    };
    struct QItem {
        DnsSdHandle handle = 0;
        DnsSdEvent event;
        std::wstring target;  // SRV target, for the address lookup when no A record came with the browse
    };

    // Diagnostics for bring-up: set QUARK_DNSSD_TRACE=1 to print what the OS callbacks report.
    template <class... A>
    static void trace(const char* fmt, A... a) {
        static const bool on = [] {
            char* v = nullptr;
            std::size_t n = 0;
            const bool set = _dupenv_s(&v, &n, "QUARK_DNSSD_TRACE") == 0 && v != nullptr;
            std::free(v);
            return set;
        }();
        if (on) std::fprintf(stderr, "[dnssd] ");
        if (on) std::fprintf(stderr, fmt, a...);
        if (on) std::fprintf(stderr, "\n");
    }
    static std::wstring widen(const std::string& s) {
        std::wstring w;
        w.reserve(s.size());
        for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));  // labels are ASCII by construction
        return w;
    }
    static std::string narrow(PCWSTR w) {
        std::string s;
        for (; w && *w; ++w) s.push_back(*w < 128 ? static_cast<char>(*w) : '?');
        return s;
    }
    static bool ends_with_ci(const std::string& s, const std::string& suf) {
        if (s.size() < suf.size()) return false;
        for (std::size_t i = 0; i < suf.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i])) !=
                std::tolower(static_cast<unsigned char>(suf[i])))
                return false;
        return true;
    }

    static VOID WINAPI on_register_done(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE) {
        trace("register/deregister done status=%lu", static_cast<unsigned long>(status));
        auto* c = static_cast<RegCtx*>(ctx);
        {
            std::lock_guard g(c->m);
            c->status = status;
            c->done = true;
        }
        c->cv.notify_all();
    }

    // The browse callback delivers a PTR (instance name, TTL 0 = goodbye) plus the instance's SRV
    // (port, host target) and TXT, and an A record when the cache has one. DnsServiceResolve is NOT
    // used: on the bring-up host it never completed for a service registered by the same machine
    // (ADR-070 section 4, F4). A missing address is looked up on the dispatcher thread instead; if that
    // also fails (the registering host does not answer its own multicast A query) the sighting is
    // delivered with ipv4 == 0 rather than dropped.
    static VOID WINAPI on_browse(DWORD status, PVOID ctx, PDNS_RECORD list) {
        auto* b = static_cast<Browse*>(ctx);
        trace("browse callback status=%lu list=%p", static_cast<unsigned long>(status), static_cast<void*>(list));
        if (status == ERROR_SUCCESS && !b->stopped.load()) {
            // The browse callback delivers wide records regardless of the UNICODE macro.
            auto* head = reinterpret_cast<DNS_RECORDW*>(list);
            const std::string tail = "." + b->type + ".local";
            for (auto* r = head; r; r = r->pNext) {
                if (r->wType != DNS_TYPE_PTR || !r->Data.PTR.pNameHost) continue;
                const std::wstring inst_name = r->Data.PTR.pNameHost;
                const std::string full = narrow(inst_name.c_str());
                if (!ends_with_ci(full, tail)) continue;
                QItem it;
                it.handle = b->handle;
                it.event.kind = r->dwTtl == 0 ? DnsSdEventKind::Lost : DnsSdEventKind::Found;
                it.event.what.service_type = b->type;
                it.event.what.instance_label = full.substr(0, full.size() - tail.size());
                std::wstring target;
                for (auto* x = head; x; x = x->pNext) {
                    if (!x->pName || _wcsicmp(x->pName, inst_name.c_str()) != 0) continue;
                    if (x->wType == DNS_TYPE_SRV && x->Data.SRV.pNameTarget) {
                        it.event.what.port = x->Data.SRV.wPort;
                        target = x->Data.SRV.pNameTarget;
                    }
                }
                for (auto* x = head; x && !target.empty(); x = x->pNext)
                    if (x->wType == DNS_TYPE_A && x->pName && _wcsicmp(x->pName, target.c_str()) == 0)
                        it.event.what.ipv4 = ntohl(x->Data.A.IpAddress);
                std::string host = narrow(target.c_str());
                if (ends_with_ci(host, ".local")) host.resize(host.size() - 6);
                it.event.what.host_label = host;
                it.target = target;
                if (it.event.kind == DnsSdEventKind::Found && it.event.what.port == 0) continue;  // SRV not here yet
                b->self->enqueue(std::move(it));
            }
        }
        if (list) DnsRecordListFree(list, DnsFreeRecordList);
    }

    void enqueue(QItem it) {
        {
            std::lock_guard g(qm_);
            q_.push_back(std::move(it));
        }
        qcv_.notify_one();
    }

    // The OS re-delivers the same records often; look a host up at most once per 10 s (a timed-out
    // multicast query costs seconds on the dispatcher thread). Only the dispatcher thread calls this.
    std::uint32_t cached_lookup(const std::wstring& host) {
        const auto now = std::chrono::steady_clock::now();
        auto it = addr_cache_.find(host);
        if (it != addr_cache_.end() && (it->second.first != 0 || now - it->second.second < std::chrono::seconds(10)))
            return it->second.first;
        const std::uint32_t ip = lookup_ipv4(host);
        addr_cache_[host] = {ip, now};
        return ip;
    }
    static std::uint32_t lookup_ipv4(const std::wstring& host) {
        PDNS_RECORD rec = nullptr;
        std::uint32_t out = 0;
        const DNS_STATUS st = DnsQuery_W(host.c_str(), DNS_TYPE_A, DNS_QUERY_BYPASS_CACHE | DNS_QUERY_MULTICAST_ONLY, nullptr,
                                         reinterpret_cast<PDNS_RECORD*>(&rec), nullptr);
        trace("DnsQuery_W(A) status=%ld", static_cast<long>(st));
        if (st == ERROR_SUCCESS)
            for (auto* r = reinterpret_cast<DNS_RECORDW*>(rec); r; r = r->pNext)
                if (r->wType == DNS_TYPE_A) {
                    out = ntohl(r->Data.A.IpAddress);
                    break;
                }
        if (rec) DnsRecordListFree(rec, DnsFreeRecordList);
        return out;
    }

    void run() {
        for (;;) {
            QItem item;
            {
                std::unique_lock g(qm_);
                qcv_.wait(g, [&] { return quit_ || !q_.empty(); });
                if (quit_ && q_.empty()) return;
                item = std::move(q_.front());
                q_.pop_front();
            }
            std::shared_ptr<Browse> b;
            {
                std::lock_guard g(m_);
                auto it = browses_.find(item.handle);
                if (it != browses_.end()) b = it->second;
            }
            if (!b || b->stopped.load()) continue;
            if (item.event.kind == DnsSdEventKind::Found && item.event.what.ipv4 == 0 && !item.target.empty())
                item.event.what.ipv4 = cached_lookup(item.target);
            // ipv4 may stay 0 ("address unresolved"): the adapter reports it and a feed cannot dial it.
            b->cb(item.event);
        }
    }

    std::mutex m_;
    std::map<DnsSdHandle, Reg> regs_;
    std::map<DnsSdHandle, std::shared_ptr<Browse>> browses_;
    std::vector<std::shared_ptr<Browse>> retired_;
    std::vector<std::shared_ptr<RegCtx>> orphan_;
    DnsSdHandle next_ = 0;

    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<QItem> q_;
    std::map<std::wstring, std::pair<std::uint32_t, std::chrono::steady_clock::time_point>> addr_cache_;
    bool quit_ = false;
    std::thread worker_;  // declared last: started after every member above is constructed
};

}  // namespace quark::net

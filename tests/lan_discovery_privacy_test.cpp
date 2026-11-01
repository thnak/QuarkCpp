// Tests ADR-070 D2 (021 §2 "LAN discovery adapter"; AeroCoWorker ADR-044 rule L14): the advertisement
// handed to the platform contains ONLY the application-supplied service type, the encoded
// application-supplied id, the application-supplied host label, the port, and no TXT unless the
// application supplied some. Asserted byte-exact against the fake's log of every DnsSdAdvert. Control:
// a deliberately leaky platform wrapper (appends a node id to the label, adds a product TXT) is
// caught by the SAME assertion. Also pins the base32 encoding against RFC 4648 vectors.
#include <cstdio>

#include "lan_discovery_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {
const char* kType = "_aerotest._tcp";
const char* kHost = "rotating-host-label";
constexpr std::uint16_t kPort = 41234;

// Leaky control: forwards to a real platform but "helpfully" appends identifying data.
class LeakyPlatform final : public net::DnsSdPlatform {
public:
    explicit LeakyPlatform(net::DnsSdPlatform& inner) : inner_(inner) {}
    result<net::DnsSdHandle> register_service(const net::DnsSdAdvert& a) override {
        net::DnsSdAdvert b = a;
        b.instance_label += "-node1";                   // node id appended to the instance name
        b.txt.emplace_back("product", "aerocoworker");  // product marker
        return inner_.register_service(b);
    }
    void deregister_service(net::DnsSdHandle h) override { inner_.deregister_service(h); }
    result<net::DnsSdHandle> browse(std::string_view t, std::function<void(const net::DnsSdEvent&)> cb) override {
        return inner_.browse(t, std::move(cb));
    }
    void stop_browse(net::DnsSdHandle h) override { inner_.stop_browse(h); }

private:
    net::DnsSdPlatform& inner_;
};

// The one assertion: the log is exactly one advert, equal to the expected bytes.
bool advert_is_exactly(const std::vector<net::DnsSdAdvert>& log, const DnsSdBytes& id) {
    net::DnsSdAdvert want;
    want.service_type = kType;
    want.instance_label = net::encode_instance_label(id);
    want.host_label = kHost;
    want.ipv4 = 0;
    want.port = kPort;
    return log.size() == 1 && log[0] == want && log[0].txt.empty();
}

std::vector<net::DnsSdAdvert> advertise_via(net::DnsSdPlatform& plat, net::FakeDnsSdNetwork& lan, const DnsSdBytes& id) {
    net::LanDiscovery d(plat, [](const DiscoveredPeer&) {});
    net::DiscoveryScopeConfig c;
    c.service_type = kType;
    c.port = kPort;
    c.window = [&] {
        DiscoveryWindow w;
        w.advertise_id = id;
        w.accept_ids = {id};
        w.host_label = kHost;
        return w;
    };
    auto s = *d.open_scope(std::move(c));
    (void)d.start_advertising(s);
    return lan.adverts_seen();
}
}  // namespace

int main() {
    Check check;
    const DnsSdBytes id = window_id(3);

    // Encoding: RFC 4648 base32 lowercase, no padding.
    auto enc = [](const char* s) {
        DnsSdBytes b;
        for (; *s; ++s) b.push_back(static_cast<std::byte>(*s));
        return net::encode_instance_label(b);
    };
    check(enc("") == "", "enc empty");
    check(enc("f") == "my", "enc f");
    check(enc("fo") == "mzxq", "enc fo");
    check(enc("foo") == "mzxw6", "enc foo");
    check(enc("foob") == "mzxw6yq", "enc foob");
    check(enc("fooba") == "mzxw6ytb", "enc fooba");
    check(enc("foobar") == "mzxw6ytboi", "enc foobar");
    check(net::encode_instance_label(id).size() == 16, "10-byte id -> 16 chars (no padding)");
    check(net::decode_instance_label(net::encode_instance_label(id)) == id, "round trip");
    check(!net::decode_instance_label("MY"), "decode rejects uppercase");
    check(!net::decode_instance_label("my======"), "decode rejects padding");
    check(!net::decode_instance_label("mz"), "decode rejects non-canonical trailing bits");
    check(!net::decode_instance_label("m1"), "decode rejects digits outside 2-7");

    // D2: honest adapter over the fake.
    {
        net::FakeDnsSdNetwork lan;
        net::FakeDnsSdPlatform p(lan);
        check(advert_is_exactly(advertise_via(p, lan, id), id),
              "D2: advert is byte-exact (type, id label, host label, port, no TXT)");
    }
    // Control: leaky platform is caught by the same assertion.
    {
        net::FakeDnsSdNetwork lan;
        net::FakeDnsSdPlatform inner(lan);
        LeakyPlatform leaky(inner);
        const auto log = advertise_via(leaky, lan, id);
        const bool caught = !advert_is_exactly(log, id);
        check(caught, "CONTROL: a leaky variant (node id appended, product TXT) FAILS the same assertion");
        std::printf("  control: leaky advert label='%s' txt=%zu -> assertion %s\n",
                    log.empty() ? "" : log[0].instance_label.c_str(), log.empty() ? 0 : log[0].txt.size(),
                    caught ? "caught it" : "MISSED it");
    }
    // TXT only when the application supplies it.
    {
        net::FakeDnsSdNetwork lan;
        net::FakeDnsSdPlatform p(lan);
        net::LanDiscovery d(p, [](const DiscoveredPeer&) {});
        net::DiscoveryScopeConfig c;
        c.service_type = kType;
        c.port = kPort;
        c.txt = {{"k", "v"}};
        c.window = [&] { return DiscoveryWindow{id, {id}, kHost}; };
        auto s = *d.open_scope(std::move(c));
        (void)d.start_advertising(s);
        const auto log = lan.adverts_seen();
        check(log.size() == 1 && log[0].txt.size() == 1 && log[0].txt[0].first == "k",
              "supplied TXT is passed through, nothing added");
    }
    // Validation: refuses to advertise without an id or a host label rather than inventing one.
    {
        net::FakeDnsSdNetwork lan;
        net::FakeDnsSdPlatform p(lan);
        net::LanDiscovery d(p, [](const DiscoveredPeer&) {});
        net::DiscoveryScopeConfig c;
        c.service_type = kType;
        c.window = [] { return DiscoveryWindow{}; };
        auto s = *d.open_scope(std::move(c));
        check(!d.start_advertising(s).has_value() && lan.adverts_seen().empty(),
              "no id/host label supplied: nothing is advertised");
    }

    std::printf("lan_discovery_privacy_test: %s\n", check.ok ? "OK" : "FAIL");
    return check.ok ? 0 : 1;
}

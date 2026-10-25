// Tests 020-Security §5 at the STORE layer: `EncryptedStore<S>` seals every snapshot/event/batch
// through any 012 `Store`, binding actor+kind+seq (+fence for snapshots) into the AEAD associated
// data. Fencing/strict-seq/atomic-batch are delegated unchanged; only the opaque record bytes change.
//
// Verified here: InMemoryStore and FileStore ciphertext-at-rest + round-trip, a real FileStore
// reopen, raw ciphertext (no canonical plaintext on disk), batch sealing, a tampered byte rejected,
// a record spliced onto ANOTHER actor rejected, and a wrong key rejected. Uses MockCipher (framing
// only — the production cipher is the mbedTLS AES-GCM adapter).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "quark/core/at_rest.hpp"
#include "quark/core/file_store.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

namespace {
struct Snap {
    std::uint64_t account = 0;
    std::uint64_t balance = 0;
};
QUARK_SERIALIZE(Snap, (1, account), (2, balance))

struct Ev {
    std::uint64_t amount = 0;
    std::uint64_t seq = 0;
};
QUARK_SERIALIZE(Ev, (1, amount), (2, seq))

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

std::vector<std::byte> encode(const Ev& e) {
    auto bytes = encode_durable(e);
    return bytes ? *bytes : std::vector<std::byte>{};
}
std::vector<std::byte> encode(const Snap& s) {
    auto bytes = encode_durable(s);
    return bytes ? *bytes : std::vector<std::byte>{};
}

bool contains(const std::vector<std::byte>& hay, const std::vector<std::byte>& needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

std::filesystem::path unique_dir() {
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("quark_at_rest_" + std::to_string(static_cast<long long>(n)));
}

std::vector<std::byte> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<std::byte> out;
    char c;
    while (in.get(c)) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

int main() {
    bool ok = true;
    MockCipher cipher(0x5EC0DE);
    StaticKeyring ring(cipher);
    const ActorId actor{TypeKey{0xA11CE}, 7};
    const auto ev1 = encode(Ev{100, 1});
    const auto ev2 = encode(Ev{200, 2});
    const auto ev3 = encode(Ev{300, 3});
    const auto snap_bytes = encode(Snap{0xA11CE, 4200});

    // ---------------- InMemoryStore: round-trip + raw ciphertext -------------------------------
    {
        InMemoryStore inner;
        EncryptedStore<InMemoryStore> store(inner, ring);
        static_assert(Store<decltype(store)>, "EncryptedStore satisfies Store");

        const FenceToken fence = store.acquire_fence(actor);
        check(store.append(actor, fence, SeqNo{1}, ev1).has_value(), "append event 1", ok);
        const EventRecord b2{2, ev2}, b3{3, ev3};
        const EventRecord batch[] = {b2, b3};
        check(store.append_batch(actor, fence, batch).has_value(), "append batch 2,3", ok);

        auto log = store.read_log(actor, SeqNo{0});
        check(log.has_value() && log->size() == 3, "read_log opens all three events", ok);
        if (log) {
            auto d1 = decode_durable<Ev>((*log).entries[0].record);
            auto d2 = decode_durable<Ev>((*log).entries[1].record);
            auto d3 = decode_durable<Ev>((*log).entries[2].record);
            check(d1 && d1->amount == 100 && d2 && d2->amount == 200 && d3 && d3->amount == 300,
                  "events round-trip through seal/open", ok);
        }

        // The underlying store must hold ciphertext, not the canonical record.
        auto raw = inner.read_log(actor, SeqNo{0});
        check(raw.has_value() && raw->size() == 3, "raw inner log has the same records", ok);
        if (raw) {
            check(!contains((*raw).entries[0].record, ev1),
                  "raw inner event bytes are not the canonical plaintext", ok);
            check((*raw).entries[0].record.size() == ev1.size() + cipher.tag_size(),
                  "sealed event carries ciphertext + tag", ok);
        }

        SnapshotRecord snap;
        snap.fence = fence;
        snap.through_seq = 3;
        snap.record = snap_bytes;
        check(store.save_snapshot(actor, fence, snap).has_value(), "save sealed snapshot", ok);

        auto loaded = store.load_snapshot(actor);
        check(loaded.has_value() && loaded->has_value(), "load sealed snapshot", ok);
        if (loaded && loaded->has_value()) {
            auto ds = decode_durable<Snap>((**loaded).record);
            check(ds && ds->balance == 4200, "snapshot round-trips through seal/open", ok);
            check((**loaded).fence == fence && (**loaded).through_seq == 3,
                  "fence/through_seq survive sealing", ok);
        }
        auto raw_snap = inner.load_snapshot(actor);
        check(raw_snap.has_value() && raw_snap->has_value() &&
                  !contains((**raw_snap).record, snap_bytes),
              "raw inner snapshot is ciphertext", ok);

        // Wrong key: the same ciphertext does not open.
        MockCipher wrong(0xBAD);
        StaticKeyring wrong_ring(wrong);
        EncryptedStore<InMemoryStore> wrong_store(inner, wrong_ring);
        check(!wrong_store.load_snapshot(actor).has_value(),
              "a wrong data key cannot open the snapshot", ok);
        check(!wrong_store.read_log(actor, SeqNo{0}).has_value(),
              "a wrong data key cannot open events", ok);

        // Cross-actor splice: put actor A's sealed snapshot into actor B's slot. Same key, same
        // fence/through_seq, but the AAD actor differs ⇒ rejected.
        const ActorId other{TypeKey{0xBEEF}, 9};
        const FenceToken other_fence = inner.acquire_fence(other);
        auto raw_a = inner.load_snapshot(actor);
        if (raw_a && raw_a->has_value()) {
            SnapshotRecord stolen = **raw_a;
            stolen.fence = other_fence;
            stolen.through_seq = 0;
            (void)inner.save_snapshot(other, other_fence, stolen);
            check(!store.load_snapshot(other).has_value(),
                  "a snapshot spliced onto another actor is rejected (AAD actor)", ok);
        }

        // Tamper: flip one ciphertext byte in the inner snapshot ⇒ open fails.
        auto raw_t = inner.load_snapshot(actor);
        if (raw_t && raw_t->has_value()) {
            SnapshotRecord victim = **raw_t;
            victim.record[0] ^= std::byte{0xFF};
            (void)inner.save_snapshot(actor, fence, victim);
            auto bad = store.load_snapshot(actor);
            check(!bad.has_value() && bad.error().code == errc::serialization,
                  "a tampered ciphertext byte is rejected", ok);
        }
    }

    // ---------------- FileStore: on-disk ciphertext + reopen + tamper ---------------------------
    {
        const std::filesystem::path dir = unique_dir();
        std::filesystem::remove_all(dir);
        {
            FileStore file(dir.string());
            EncryptedStore<FileStore> store(file, ring);
            const FenceToken fence = store.acquire_fence(actor);
            check(store.append(actor, fence, SeqNo{1}, ev1).has_value(), "FileStore append", ok);
            SnapshotRecord snap;
            snap.fence = fence;
            snap.through_seq = 1;
            snap.record = snap_bytes;
            check(store.save_snapshot(actor, fence, snap).has_value(), "FileStore snapshot", ok);

            // The WAL file must not contain the canonical event/snapshot bytes.
            bool saw_file = false;
            bool plaintext = false;
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;
                saw_file = true;
                const auto bytes = read_file(entry.path());
                if (contains(bytes, ev1) || contains(bytes, snap_bytes)) plaintext = true;
            }
            check(saw_file, "FileStore created a WAL file", ok);
            check(!plaintext, "the WAL file contains no canonical plaintext", ok);
        }

        // Reopen on the same directory: replay must yield decryptable records.
        {
            FileStore file(dir.string());
            EncryptedStore<FileStore> store(file, ring);
            auto log = store.read_log(actor, SeqNo{0});
            check(log.has_value() && log->size() == 1, "reopen reads the sealed event", ok);
            if (log) {
                auto d = decode_durable<Ev>((*log).entries[0].record);
                check(d && d->amount == 100, "reopened event decrypts to the original", ok);
            }
            auto snap = store.load_snapshot(actor);
            check(snap.has_value() && snap->has_value(), "reopen reads the sealed snapshot", ok);
            if (snap && snap->has_value()) {
                auto d = decode_durable<Snap>((**snap).record);
                check(d && d->balance == 4200, "reopened snapshot decrypts to the original", ok);
            }
        }

        // Tamper on disk: corrupt the WAL payload, then a reopen must reject (no plaintext leak).
        {
            std::filesystem::path wal;
            for (const auto& entry : std::filesystem::directory_iterator(dir))
                if (entry.is_regular_file()) wal = entry.path();
            check(!wal.empty(), "found the WAL path for the tamper control", ok);
            if (!wal.empty()) {
                std::vector<std::byte> bytes = read_file(wal);
                if (!bytes.empty()) bytes.back() ^= std::byte{0xFF};  // last byte = snapshot payload
                std::ofstream out(wal, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
                out.close();

                FileStore file(dir.string());
                EncryptedStore<FileStore> store(file, ring);
                auto log = store.read_log(actor, SeqNo{0});
                auto snap = store.load_snapshot(actor);
                const bool event_lost = !log.has_value() || log->empty();
                const bool snap_lost = !snap.has_value() || !snap->has_value();
                check(event_lost || snap_lost,
                      "a tampered WAL record is rejected on reopen", ok);
            }
        }
        std::filesystem::remove_all(dir);
    }

    std::printf("security_at_rest_store_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

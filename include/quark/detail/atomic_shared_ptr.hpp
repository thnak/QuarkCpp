// Implements 019-Platform-Abstraction-Layer §"Android target" / ADR-071 §3 (toolchain gaps): a portable
// `std::atomic<std::shared_ptr<T>>`. The NDK r28 libc++ (LLVM 19) does not provide the C++20 partial
// specialisation (`__cpp_lib_atomic_shared_ptr` is undefined; `std::atomic<shared_ptr>` fails the
// trivially-copyable static_assert), and neither does it provide `std::atomic_ref` (see atomic_relaxed.hpp).
// Where the standard feature exists this header is exactly `std::atomic<std::shared_ptr<T>>` — no cost, no
// behaviour change on Linux/Windows. Where it does not, the fallback is a short mutex around the pointer
// copy: the same observable semantics (load/store/exchange are atomic w.r.t. each other, a loaded pointer
// pins its object), at the price of a lock. Every user (Topic/FanOut COW membership, capability and revocation
// snapshots, the SecureTransport receiver) loads on a path where one uncontended mutex is far below the
// frame cost; ADR-071 §3 records that the Android fallback is NOT benchmarked against the 023 budgets.
// The old pointer is released OUTSIDE the lock so a destructor can never run under it.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <version>

namespace quark::detail {

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
template <class T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#else
template <class T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() noexcept = default;
    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    [[nodiscard]] std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const {
        std::lock_guard<std::mutex> g(mu_);
        return p_;
    }
    void store(std::shared_ptr<T> v, std::memory_order = std::memory_order_seq_cst) {
        {
            std::lock_guard<std::mutex> g(mu_);
            p_.swap(v);
        }
        // `v` now holds the old pointer and is released here, outside the lock.
    }
    [[nodiscard]] std::shared_ptr<T> exchange(std::shared_ptr<T> v, std::memory_order = std::memory_order_seq_cst) {
        std::lock_guard<std::mutex> g(mu_);
        p_.swap(v);
        return v;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<T> p_;
};
#endif

}  // namespace quark::detail

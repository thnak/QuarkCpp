// Diagnostic only (arm64-asan-diag.yml): is ASan's stack-use-after-return fake stack slow on this
// toolchain/arch independent of Quark? Millions of tiny non-inlined calls, like Debug std::vector
// begin()/end(), each with an addressable local so ASan gives the frame a fake-stack slot.
#include <chrono>
#include <cstdio>
#include <vector>

__attribute__((noinline)) static int leaf(const std::vector<int>& v) {
    volatile int local = 0;                // address-taken local => instrumented frame
    const int* p = &local == nullptr ? nullptr : v.data();
    return static_cast<int>(*p) + local;
}

int main() {
    std::vector<int> v(16, 1);
    const auto t0 = std::chrono::steady_clock::now();
    long sum = 0;
    for (long i = 0; i < 50'000'000; ++i) sum += leaf(v);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("calls=5e7 sum=%ld elapsed=%.2fs\n", sum, s);
}

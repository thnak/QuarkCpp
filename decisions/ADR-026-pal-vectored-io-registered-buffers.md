# ADR-026: PAL Proactor Vectored I/O + Registered Buffers Surface Shape and Support Matrix

## Status

Accepted

## Question

019's proactor `IoContext`/`Socket` abstraction left two related questions
open:

1. Should the interface expose vectored/scatter-gather I/O and registered
   buffers (io_uring fixed buffers, a zero-copy optimization) **generically
   across all backends**, or **only where the backend natively supports
   them** (io_uring/IOCP), leaving epoll/kqueue/the sim backend without the
   capability (or with an emulated one)?
2. What minimum OS/kernel versions does each backend require (io_uring
   kernel floor, Windows IOCP feature level), and what is the exact
   fallback policy, documented as a support matrix?

The design must specify the exact surface shape (uniform vs
capability-gated vs tiered) and how caller code behaves identically in
production (native backends) and in `SimEngine` without runtime
backend-dispatch cost, honoring 019's locked completion-shaped API,
compile-time backend selection, fixed per-OS backend table, "PAL headers
only" rule, `std::expected` error convention, and the zero-cost hot-path
invariant.

Three designs were drafted, cross-examined (red team vs. defense), and then
proven or disproven with compiled, executed C++ (g++ 14.2.0 and clang++
20.1.2, -O2/-O3 plus ASan/UBSan/TSan builds, taskset-pinned to ≤4 cores per
the machine-core-limit rule).

## Designs (one-line summaries)

- **Uniform Completion-Shaped Vectored I/O + Registered Buffers (Angle 1)**:
  one identical `IoVec`/`BufferRegion`/`RegisteredSlice` surface and
  `register_buffers()`/`submit_readv()`/`submit_writev()` API on *every*
  backend. The key insight: POSIX `readv`/`writev` and Winsock
  `WSASend`/`WSARecv` already accept arbitrary caller memory with no
  pre-registration step, so on epoll/kqueue/sim "registration" is real O(1)
  bookkeeping (a slot-table index), not an emulation-with-a-copy-loop — the
  uniform surface costs nothing extra on backends that gain nothing from
  it. A `has_native_registered_buffers<Pal>` trait is an optional
  `if constexpr` perf hint only, never a callability gate.
- **Capability-Queried / Trait-Gated PAL I/O Surface**: two independent,
  purely structural C++23 concepts (`SupportsVectoredIo<Pal>`,
  `SupportsRegisteredBuffers<Pal>`) gate extra static members that exist
  *only* on backend types whose `capabilities` table says `true`. No
  emulation code, no runtime flag, no extra bytes on capability-less
  backends; callers write `if constexpr (Supports...<Pal>) ... else
  <hand-written portable fallback>`.
- **Two-tier PAL I/O surface (winner)**: `IoContext` keeps exactly two
  tiers. Tier 1 is a mandatory single-buffer `submit_recv`/`submit_send`,
  present and trivially emulatable on every backend including the sim.
  Tier 2 is *one* named extension point, `IoContext::advanced_io() ->
  std::expected<AdvancedIo, std::error_code>`, queried once at
  connection/session setup and cached as `std::optional<AdvancedIo>`.
  `AdvancedIo` bundles scatter/gather submission and registered-buffer
  zero-copy submission; io_uring/IOCP populate it with real bodies,
  epoll/kqueue/sim declare the identical method set with trivial,
  never-invoked bodies (chosen by the existing compile-time backend
  header). Registered-buffer slot ownership between shard threads
  (producers) and the single I/O loop thread (completer) is arbitrated by
  a lock-free atomic bitmask free-list.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| C1 byte-identical cross-backend scatter writev (uring/epoll-emulated/sim, built fresh from scratch) | Uniform | yes | **CORRECT** | 6/6 builds clean, CRC32-matched reassembly across all 3 backends, both compilers, 20 reps |
| C2 registered vs unregistered writev, epoll, ≤5% overhead | Uniform | yes | **CORRECT** | median delta −1.87% (g++) / −2.36% (clang++), i.e. no overhead |
| C3 register/submit/wait loop-thread-only, 0 atomics needed, TSan clean | Uniform | yes (corrected: post()-marshalling framing was factually wrong vs. net.hpp's actual add_fd/mod_fd/del_fd contract; fixed to loop-thread-only direct calls) | **CORRECT** | 0 TSan reports, 100k×2 cycles, both compilers |
| C4 std::expected degrade path on RLIMIT_MEMLOCK failure works; region-id off-by-one fixed | Uniform | yes (fatal off-by-one conceded and fixed: 1-based ids into a 0-based array) | **CORRECT** | exactly 64 successful registrations, canary untouched, unregistered fallback transfers correct CRC-matched bytes |
| C5 native WRITE_FIXED reduces CPU/op ≥100k IOPS at 40KiB payload | Uniform | yes (attacked on mechanism, defended) | **WRONG** | registered path never reached 100k ops/s at spec payload and was consistently **flat-to-worse** than plain WRITEV (−9.94% to −0.97% CPU/op, wrong direction) |
| C6 out-of-bounds RegisteredSlice rejected before any syscall | Uniform | yes (fatal gap conceded: bounds check was entirely absent from the original sketch) | **CORRECT** | ASan clean after fix; negative control (check bypassed) reliably reproduces heap-buffer-overflow |
| C7 debug-only in-flight-counter catches unregister-while-in-flight; compiles to zero cost in release | Uniform | yes (new claim added to close a serious UAF gap) | **CORRECT** | debug build aborts deterministically; release objdump shows guard fully compiled out; ASan confirms release path is genuine undetected UB, as documented |
| F1r registered recv, small transfers win most (page-pin amortization), shrinking at bulk sizes | Capability-gated | yes (revised prediction from conceded-wrong F1) | **WRONG** | measured **opposite** direction: registered buffers *lose* at 64B/4KiB (−2.6% to −7%) and only *win* at 1MiB (+11–13.7%) |
| F2 templated `if constexpr` dispatch = 0 extra branches/indirect calls vs hand-written call | Capability-gated | yes | **CORRECT** | identical branch/call/indirect counts, both compilers, both -O2/-O3 |
| S1 calling a capability-gated method against a non-satisfying Pal is a hard compile error | Capability-gated | yes | **CORRECT** | both compilers: "no member named ..." |
| S2 free-list race-freedom is loop-thread-only by discipline, provably not by hidden sync | Capability-gated | yes | **CORRECT** | single-thread: 0 TSan reports/1M ops; deliberate cross-thread misuse: 5/5 runs detect the race, both compilers |
| S3 debug double-release assert catches aliasing; compiles to zero bytes under NDEBUG | Capability-gated | yes (new claim, closes a serious double-release gap) | **CORRECT** | debug abort fires; release sizeof/`nm` unchanged |
| C1r `Pal` aggregator + concepts evaluate exactly per the capability table | Capability-gated | yes (fatal gap conceded: no backend ever defined the `Pal` aggregator type the concepts required) | **CORRECT** | 10/10 static_asserts pass both compilers after the fix |
| C2 wire bytes identical, io_uring vectored branch vs sim scratch-copy fallback | Capability-gated | yes | **CORRECT** | byte-for-byte identical |
| C3 `iov.size() > kMaxIov` rejected, 0 SQEs submitted | Capability-gated | yes | **CORRECT** | correct error, SQ tail unchanged |
| C4r kernel-floor construction failure below the true floor | Capability-gated | yes (fatal gap conceded: original 5.19 floor was fabricated from an unused multishot-recv probe; corrected to 5.1, matching syscalls actually used) | **INCONCLUSIVE** | no differently-versioned kernel available in this sandbox to execute either half |
| C5r capability-less backends carry zero added bytes/symbols | Capability-gated | yes (conceded: "additive-only" framing hid an unlanded prerequisite refactor) | **INCONCLUSIVE** | grep confirms current tree has no `Socket` class/backend namespaces at all — untestable until that refactor lands |
| C6 shared `thread_local` iovec buffer aliases across connections | Capability-gated | yes (fatal, conceded and fixed: moved to per-Socket-instance storage) | **CORRECT** | two concurrent connections' vectored recvs land in their own buffers, no aliasing, both compilers |
| C1r byte-identical caller source across every backend (io_uring/epoll/sim) | Two-tier | yes (fatal gap conceded: original truly-empty `AdvancedIo` on non-capable backends failed to compile the design's own showcase code; fixed to declare the same method set everywhere with trivial never-invoked bodies) | **CORRECT** | 6/6 builds clean, `grep` for backend names in caller source is empty, `sizeof(AdvancedIo)==1` on non-capable backends |
| C2 cached capability check adds no measurable epoll overhead vs. Tier-2-stripped baseline | Two-tier | yes | **CORRECT** | delta within ±4% noise, sign flips across compilers/repeats |
| C3 `submit_send_v` (vectored header+body) has lower p50 latency and fewer SQEs than two `submit_send` calls | Two-tier | yes | **CORRECT** | **1.00 vs 2.00 SQEs/frame; p50 6435–6659ns vs 9986–10823ns, a ~35–42% latency reduction, both compilers** |
| C4a `FixedBufferFreeList` claim/release race-free and ABA-free given valid backing-memory lifetime | Two-tier | yes (narrowed: original claim conflated bit-ownership with memory-lifetime, fixed by stating the lifetime precondition explicitly, see C9) | **CORRECT** | 0 TSan reports, 0 ownership mismatches, 1M ops × 10 runs |
| C4b thread-hashed scan fixes the free-list's cache-line CAS-storm contention collapse | Two-tier | yes (attacked, revised fix attempted) | **WRONG** | 4-thread aggregate throughput stayed 60–85% *below* 1-thread throughput for both 64- and 256-slot pools; root cause (whole `free_mask_` fits in one cache line) not fixable by scan-order alone |
| C5 `AdvancedIo` is empty/1-byte on non-capable backends; `advanced_io()` compiles to a trivial load-and-return | Two-tier | yes | **CORRECT** | 2–3 instructions, 0 calls, both compilers (after fixing a Meyer's-singleton guard-branch regression) |
| C6 sim backend's `advanced_io()` failure is deterministic across seeded runs | Two-tier | yes | **CORRECT** | 100/100 runs identical |
| C7 wire bytes identical, vectored send vs. two sequential sends | Two-tier | yes | **CORRECT** | 0 byte mismatches, 1000 frames |
| C8 io_uring construction fails fast (std::expected) when the kernel rejects `io_uring_setup`, never silently degrades per-op | Two-tier | yes | **CORRECT** | seccomp-denied `io_uring_setup` → immediate `std::unexpected`, both compilers |
| C9 registered-buffer lifetime/quiescence contract (member-owned pool, `expected`-returning unregister, in-flight counter rejects premature unregister) | Two-tier | yes (fatal gap conceded and fixed: original sketch registered a constructor-local buffer that dangled the instant construction returned, and `unregister_buffers` returned `void` in violation of the RFC's own std::expected ground rule) | **CORRECT** | 0 ASan/UBSan/LSan reports over 100 full lifecycle reps; premature unregister correctly rejected with `resource_busy`, 0 kernel deregister calls issued until drained |

## Decision

**Two-tier PAL I/O surface (mandatory baseline + one named `advanced_io()`
capability object) wins.**

Applying the ranking in order:

**1. Safety gate.** No design has a claim of kind *safe* or *correct* come
back `WRONG`. Each design's single disproven claim is a *fast*-kind
(performance) claim: Uniform's C5 (native `WRITE_FIXED` speedup),
Capability-gated's F1r (registered-recv amortization direction), and
Two-tier's C4b (free-list contention scaling). None of these gate a design
out — they narrow what can be claimed about registered-buffer performance,
not whether the surface is correct or safe. All three designs' *safe* and
*correct* claims that survived cross-examination were proven `CORRECT`
(Two-tier and Uniform both also caught and fixed genuine fatal safety
defects in their own reference sketches — bounds checks, lifetime/UAF
hazards, off-by-one indexing, cross-connection buffer aliasing — via this
debate process, which is exactly the kind of scrutiny this exercise exists
to apply).

**2. Proven beats claimed.** Counting only claims that survived
cross-examination *and* were proven `CORRECT` by executed evidence:
Two-tier has **9** (C1r, C2, C3, C4a, C5, C6, C7, C8, C9), Capability-gated
has **8** (F2, S1, S2, S3, C1r, C2, C3, C6), Uniform has **6** (C1, C2, C3,
C4, C6, C7). Two-tier has both the highest count and the fewest
unresolved-inconclusive claims: Capability-gated is carrying two
`INCONCLUSIVE` claims (C4r, C5r) that "carry no weight" per the ranking
rule but also represent unresolved architectural dependencies (an
unlanded prerequisite refactor extracting per-backend `Socket`/`IoContext`
namespaces — confirmed absent from the current tree by grep — and an
unverifiable kernel-floor claim). Two-tier and Uniform both cleared their
claims to a definite verdict (proven or disproven), which is a stronger
evidentiary position than "inconclusive."

**3. Measured hot-path numbers among safe survivors.** This is where
Two-tier separates decisively from the other two. Its **C3 result — a
~35–42% p50 latency reduction and an exact halving of SQEs-per-frame
(1.00 vs 2.00) for a realistic header+body actor-frame send** — is a
clean, reproducible, directly-relevant win on the very feature this RFC is
about (vectored I/O), on both compilers. By contrast, the other two
designs' central *registered-buffer* performance theses were **not**
substantiated for the workload this engine actually serves: Uniform's C5
found `WRITE_FIXED` flat-to-worse than plain `WRITEV` at the claim's own
spec payload (40KiB, ≥100k IOPS never even reached), and Capability-gated's
F1r found registered buffers *lose* at small/high-IOPS sizes (64B/4KiB,
−2.6% to −7%) and only pay off at bulk sizes (1MiB, +11–13.7%) — the
opposite of what either design predicted, and a size regime (megabyte-scale
transfers) that is atypical for actor-to-actor message frames. Two-tier's
own registered-buffer contention weakness (C4b) is real, but it sits behind
a graceful, already-proven-fast fallback (the same `submit_send_v` vectored
path from C3) rather than blocking the send — so under contention, Two-tier
degrades to its strongest proven number rather than to a slow or unsafe
path.

**4. Core-invariant integrity.** None of the three designs bend a locked
invariant outright (no heap allocation or locks were added to any hot path;
capability signals are compile-time in all three, confirmed by objdump/
static_assert evidence). Two-tier's residual weakness (C4b) is a
*performance* ceiling under heavy shard-thread contention on the
registered-buffer fast path specifically — not a correctness break (C4a's
TSan-clean race-freedom holds unconditionally) and not a reintroduction of
heap/locks/dynamic dispatch. Capability-gated's clean, invariant is
undermined by a real deployability gap: its structural concepts require a
`Pal` aggregator and per-backend namespace split that **do not exist in
this codebase today** (confirmed by grep — zero hits for `class Socket`,
`epoll_backend`, `io_uring_backend`, etc.) — meaning this design cannot be
adopted without first landing a separate, unreviewed breaking refactor,
a real cost the design's "additive-only" framing understated. Two-tier and
Uniform both work against the codebase largely as it exists today (modulo
the same missing IoContext-completion-queue scaffolding all three designs
had to build fresh for testing).

**Why not Uniform.** The "registration is free bookkeeping on epoll/kqueue"
insight is genuinely sound and well-proven (C1–C4, C6, C7 all correct, with
two fatal reference-sketch bugs — the missing bounds check and the
region-id off-by-one — caught and fixed in the process, which is a
meaningful contribution). But its headline performance case for *why*
registered buffers matter (C5) is disproven at the claim's own spec
parameters, and being uniform means every backend, including ones that
gain nothing (epoll/kqueue/sim), carries the full `RegisteredSlice`
bounds-checked submission path on every op — a broader "pay for it
everywhere" surface than Two-tier's single cached gateway, for a benefit
(C5) that did not measure out.

**Why not Capability-gated.** The structural-concept approach is elegant
C++23 and its safety story (S1 hard compile errors, S2/S3 race and
double-release protection) is fully proven. But it required fixing a fatal,
ordinary-case data-corruption bug (the shared `thread_local` iovec buffer
aliasing two different connections' vectored recvs on the same loop
thread — not an edge case, the normal multi-connection case this IoContext
exists to serve) and, on the exact second half of this debate's own
question (the support-matrix floor), it **initially fabricated a kernel
floor** (5.19, gated on a probe for multishot recv that the code never
uses) that was wrong by a wide margin (the real floor for the syscalls
actually used is 5.1) — a serious miss on precisely the topic under review,
even though it was caught and corrected during cross-examination. Layered
with the two `INCONCLUSIVE` claims (an unverifiable kernel-floor test and
an admittedly-unmet prerequisite refactor), this design is the least ready
to promote past Draft of the three.

## Residual risks

- **Registered buffers do not pay off for typical actor message sizes.**
  Both Uniform's C5 and Capability-gated's F1r (and, by the same
  mechanism, Two-tier's design implicitly) found that io_uring fixed
  buffers help only at large (≈1MiB-class) transfers, and are flat-to-worse
  at small/medium sizes typical of actor-to-actor frames over loopback TCP.
  Two-tier's `submit_send_fixed`/`submit_recv_fixed` should be documented
  as a niche optimization for bulk/streaming payloads (e.g. large
  snapshot/state transfer, 012's persistence path), **not** a general
  send-path default — the proven win for the general case is vectored I/O
  (C3), not registered buffers.
- **`FixedBufferFreeList` collapses under exactly its stated target
  workload.** C4b showed 4 concurrent shard-thread claimers producing
  *below* single-thread aggregate throughput on both 64- and 256-slot
  pools, because the entire `free_mask_` bitmask fits inside one cache
  line regardless of scan order — thread-hashing the scan start did not
  fix it. A real fix needs per-word cache-line padding (or a different
  primitive, e.g. a per-shard-affine sub-pool with no cross-thread
  contention at all, mirroring the "workers are lanes not owners"
  discipline). Until fixed, registered-buffer send-slot acquisition under
  contention should be expected to throttle to the vectored fallback, not
  to scale.
- **Windows RIO (Registered I/O)** — the actual Windows analogue of
  io_uring fixed buffers — was out of scope for all three designs and
  remains unimplemented/unbenchmarked. `iocp_backend`'s registered-buffer
  capability should stay documented as **unavailable** until a dedicated
  RIO-backed tier is designed and proven separately; do not infer Windows
  parity from any of this ADR's Linux-only measurements.
- **kqueue (macOS) has zero executed evidence across all three designs** —
  every proof in this debate ran on Linux (io_uring/epoll) or the sim
  backend. The kqueue capability entries in any resulting spec table are
  structural/paper-only and must be flagged as such until a macOS host is
  available to prove them.
- **The kernel-floor claim (C8/C4r-class) is proven only qualitatively.**
  Two-tier's C8 proves fail-fast construction under a seccomp-denied
  `io_uring_setup` on this box's 6.14 kernel; no design executed a real
  differently-versioned kernel (pre-5.1, or 5.1–5.18) to confirm the exact
  documented floor end-to-end. The 5.1 floor (io_uring_setup,
  `IORING_REGISTER_BUFFERS`, `READ_FIXED`/`WRITE_FIXED`, `READV`/`WRITEV`)
  is derived from public kernel changelogs and syscalls actually exercised
  in the proven code, not from a direct low-end-kernel experiment in this
  session.
- **The registered-buffer lifetime/quiescence contract (C9) is new and
  unreviewed outside this debate.** It closes a real dangling-memory and
  premature-unregister hazard, but it is a new invariant (per-region
  in-flight atomic counter, `unregister_buffers` returns
  `std::expected<void, std::error_code>` and rejects with
  `resource_busy` while ops are outstanding) that must be written into
  019 explicitly, not left implicit in an implementation.

## Spec recommendations for `019-Platform-Abstraction-Layer.md`

1. **New §Advanced I/O tier** — Document the two-tier shape as the
   accepted resolution to the open question: Tier 1 (`submit_recv`/
   `submit_send` over a single `std::span`) is mandatory, identical, and
   trivially emulatable on every backend including `SimEngine`. Tier 2 is
   exactly one named extension point, `IoContext::advanced_io() ->
   std::expected<AdvancedIo, std::error_code>`, queried once (at
   connection/session setup, never per-op) and cached by the caller as
   `std::optional<AdvancedIo>`. Specify that `AdvancedIo` declares the
   *same* public member signatures on every backend (capable backends give
   them real bodies plus a non-owning pointer back into `IoContext` state;
   non-capable backends give the same signatures trivial
   `std::unexpected(not_supported)` bodies and zero data members, staying
   an empty, sizeof==1 type) — this is required, not optional, to keep
   caller source byte-identical across backends (proven necessary by the
   C1r fatal-gap fix).
2. **§Registered buffers — scope the recommendation.** State explicitly,
   backed by this ADR's measured numbers, that `submit_*_fixed` is a
   niche optimization for bulk/streaming transfers (documented threshold:
   materially benefits only in the hundreds-of-KB-to-MB range) and must
   **not** be the default or recommended path for ordinary actor message
   frames, where the proven win is vectored I/O (`submit_send_v`/
   `submit_recv_v`), not registered buffers.
3. **New §Registered-buffer lifetime contract** — Add C9's contract as a
   normative rule: registered memory must be owned by the caller (e.g. a
   shard's `std::pmr` allocation) for at least
   `[register_buffers .. unregister_buffers-after-drain]`;
   `unregister_buffers` returns `std::expected<void, std::error_code>`
   (never `void`, per 008's error-model rule that applies to every
   fallible PAL call without exception) and must reject with
   `std::errc::device_or_resource_busy` — without issuing the underlying
   kernel deregister call — while any submitted op against that region has
   not yet completed. A per-region in-flight counter, decremented in the
   same completion handler that would otherwise release a slot, is the
   reference mechanism.
4. **§Support matrix** — Publish the corrected floor table (fixing the
   fabricated-and-conceded 5.19 figure from the cross-examination round):
   Linux io_uring backend, floor kernel 5.1 (covers `io_uring_setup`,
   `IORING_REGISTER_BUFFERS`, `IORING_OP_READ_FIXED`/`WRITE_FIXED`,
   `READV`/`WRITEV` — the exact syscalls this ADR's proven code uses); epoll
   fallback, universal, selected as a **build-time** option per 019's
   existing rule; macOS kqueue, capability entries currently
   **structural/unproven** (flag pending a macOS host); Windows IOCP,
   vectored I/O available via `WSASend`/`WSARecv` (baseline Windows),
   registered buffers (RIO) explicitly **out of scope / unavailable**
   pending a dedicated future design. Document the fallback policy per
   019's existing rule: kernel/backend selection is resolved at CMake
   configure time via a compile probe, never `uname()`/runtime detection;
   construction (`IoContext::create()`/equivalent) must fail fast with
   `std::unexpected` if the compiled-in backend's minimum requirement is
   not met at runtime (proven: C8), never silently degrade per-op.
5. **§Concurrency note** — Record the proven `FixedBufferFreeList`
   contention ceiling as a documented limitation, not silently shipped:
   the reference bitmask free-list is safe (TSan-clean, C4a) but **does
   not scale** past ~1 uncontended thread once more than a handful of
   shard threads contend for registered-buffer slots (measured: 4-thread
   aggregate throughput below 1-thread throughput). Document the required
   fallback behavior (pool-exhausted → vectored `submit_send_v`, itself
   proven fast, per C3) as the *load-bearing* mitigation, and mark a
   cache-line-padded or per-shard-affine free-list redesign as follow-up
   work before recommending registered buffers under multi-shard
   contention.
6. **§Two-tier caller pattern** — Add the proven caller idiom as the
   canonical example: query `advanced_io()` once at connection setup,
   branch on the cached `std::optional<AdvancedIo>` at the top of the
   send/recv hot function body (proven zero measurable overhead vs. a
   Tier-2-stripped baseline, C2), and always keep the Tier-1 baseline call
   reachable as the correctness fallback on every backend, including
   `SimEngine` (proven deterministic per seed, C6).

## Note on scope not covered by this ADR

kqueue/macOS and Windows IOCP+RIO capability entries in the resulting
support-matrix table should be marked **unverified in this debate** (no
macOS or Windows host was available) and not represented as proven
alongside the Linux io_uring/epoll/sim results that this ADR's evidence
actually covers.

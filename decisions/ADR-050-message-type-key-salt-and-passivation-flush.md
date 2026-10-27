# ADR-050 — Message type-key salt; passivation-flush failure policy (#18, #24a)

Status: Accepted. Evidence: `tests/type_key_collision_test.cpp` (compile-time + runtime), existing
`tests/engine_passivate_persistence_test.cpp`.

## #18 — same-layout message types shared one key (verified, not just read)
`fingerprint_v<T>` hashes tags + field layout only, so `Deposit{u64,u64}` and `Withdraw{u64,u64}`
had one `TypeKey`, and a user event shaped like `WatermarkRecord` was folded into the dedup
watermark by `recover()`. Local dispatch is by C++ type (Protocol slot) and was not affected; the
wire key (`register_remote`) and the durable record key were.

Decision:
1. `static constexpr std::uint64_t quark_type_salt` on a Described type is folded into its
   fingerprint (016). Types without it keep their existing key (no wire/durable change for them).
2. `WatermarkRecord`, `OutboxRecord`, `ReplyRecord` carry reserved salts, so no user type of the same
   layout can alias them. **Durable break:** logs written before this ADR with these internal records
   do not decode; acceptable pre-1.0, noted in README.
3. `fold_protocol` `static_assert`s that the Described messages of one actor Protocol have distinct
   keys, so a collision is a compile error telling the author to add a salt (the startup scan now
   covers message types, not only actor types).

## #24a — failed snapshot on passivation
Reviewed `Activation::close_out_retire`. A flush failure is already surfaced (`deactivate_flush_failures`
counter, asserted in `engine_passivate_persistence_test`). Keeping the actor Running on failure was
rejected: it sits on the proven Dekker close-out path (ADR-028) and a persistent store failure would
make the activation unevictable and spin. Policy kept: retire, count, never block eviction. The
retry/keep-active variant is deferred to the #24b memory-reclaim work, where eviction accounting lives.

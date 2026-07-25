**[Home](Home)**

**Using Quark**
- [How To: Write Your First Actor](How-To-Write-Your-First-Actor)
- [Samples](Samples)
- [Architecture Overview](ActorEngineSpecification)
- [Persistence Adapters](PersistenceAdapters)
- [Performance](PERFORMANCE)

**Contributing to Quark**
- [Contributor Guide](Contributing)
- [Conventions](CONVENTIONS)
- [Project Status](Project-Status)
- [Verification](VERIFICATION)
- [Benchmarks](Benchmarks)
- [Open Questions](OpenQuestions)

<details>
<summary><strong>Specifications (RFC) — 27 docs</strong></summary>

- [001 Actor Execution Model](001-Actor-Execution-Model)
- [002 Scheduler](002-Scheduler)
- [003 Memory](003-Memory)
- [004 Resources](004-Resources)
- [005 Developer Model](005-Developer-Model)
- [006 Messaging and Addressing](006-Messaging-and-Addressing)
- [007 Failure and Supervision](007-Failure-and-Supervision)
- [008 Metadata and Startup](008-Metadata-and-Startup)
- [009 Observability](009-Observability)
- [010 Distribution](010-Distribution)
- [011 Timers and Scheduled Work](011-Timers-and-Scheduled-Work)
- [012 Persistence](012-Persistence)
- [013 Configuration](013-Configuration)
- [014 Testing Model](014-Testing-Model)
- [015 Reentrancy and Quiescence](015-Reentrancy-and-Quiescence)
- [016 Serialization](016-Serialization)
- [017 Delivery Guarantees](017-Delivery-Guarantees)
- [018 Clocks and Deadlines](018-Clocks-and-Deadlines)
- [019 Platform Abstraction Layer](019-Platform-Abstraction-Layer)
- [020 Security](020-Security)
- [021 Cluster Formation and Lifecycle](021-Cluster-Formation-and-Lifecycle)
- [022 Resource Governance and Overload Control](022-Resource-Governance-and-Overload-Control)
- [023 Performance Targets and Budgets](023-Performance-Targets-and-Budgets)
- [024 Streaming and Inbound Streams](024-Streaming-and-Inbound-Streams)
- [025 Placement Policies and Stateless Workers](025-Placement-Policies-and-Stateless-Workers)
- [026 Large Scale Cluster Topology](026-Large-Scale-Cluster-Topology)
- [027 Reminders](027-Reminders)

</details>

<details>
<summary><strong>Decision records (ADRs) — 19 docs</strong></summary>

- [ADR-001 Mailbox MPSC hot path](ADR-001-mailbox-mpsc-hot-path)
- [ADR-002 Mailbox MPSC r2](ADR-002-mailbox-mpsc-hot-path-r2)
- [ADR-003 Mailbox MPSC r3](ADR-003-mailbox-mpsc-hot-path-r3)
- [ADR-004 Mailbox MPSC r4](ADR-004-mailbox-mpsc-hot-path-r4)
- [ADR-005 Inbound stream ingestion](ADR-005-inbound-stream-ingestion-hot-path)
- [ADR-006 Large-scale cluster topology](ADR-006-large-scale-cluster-topology)
- [ADR-007 Actor authoring & dispatch](ADR-007-actor-authoring-and-handler-dispatch-api)
- [ADR-008 Config & activation lifecycle](ADR-008-engine-actor-configuration-and-activation-lifecycle-policy)
- [ADR-009 Failure & supervision](ADR-009-failure-supervision-and-recovery-policy-model)
- [ADR-010 Priority & fairness scheduling](ADR-010-priority-and-fairness-scheduling-policy)
- [ADR-011 Cluster relay & placement gate](ADR-011-cluster-relay-and-placement-gate-verification)
- [ADR-012 Weighted-HRW re-gate](ADR-012-weighted-hrw-distribution-regate)
- [ADR-013 Weighted-HRW re-gate 2](ADR-013-weighted-hrw-distribution-regate-2)
- [ADR-014 Streaming async-suspend gate](ADR-014-streaming-async-suspend-real-scheduler-gate)
- [ADR-015 Execution vehicle](ADR-015-actor-execution-vehicle-passive-stackless-vs-fibers)
- [ADR-016 Serialization wire fast path](ADR-016-serialization-wire-fast-path-encode-gate)
- [ADR-017 Durable reminder scale gate](ADR-017-durable-reminder-mass-due-scale-gate)
- [ADR-018 Outbound streaming replies](ADR-018-outbound-streaming-replies)
- [ADR-019 Broadcast / publish primitive](ADR-019-best-effort-broadcast-publish-primitive)

</details>

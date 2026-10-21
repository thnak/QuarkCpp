import type { Lang } from '../context/LangContext'

export interface Translation {
  // Nav
  nav: {
    features: string
    proof: string
    performance: string
    compare: string
    start: string
    docs: string
  }
  // Hero
  hero: {
    pill: string
    titleLead: string
    titleGrad: string
    titleTail: string
    lede: string
    ctaFirstActor: string
    ctaDocs: string
  }
  // Stats
  stats: {
    testsLabel: string
    testsNote: string
    docsLabel: string
    docsNote: string
    p50Label: string
    p50Note: string
    throughputLabel: string
    throughputNote: string
  }
  // Features
  features: {
    sectionTitle: string
    items: Array<{ title: string; body: string }>
  }
  // Proof Loop
  proof: {
    sectionTitle: string
    steps: Array<{ step: string; title: string; body: string }>
  }
  // Performance
  perf: {
    sectionTitle: string
    featureCol: string
    specCol: string
    metricCol: string
    measuredCol: string
    budgetCol: string
    statusCol: string
    verdictGoal: string
    verdictFree: string
    invariantGatesTitle: string
  }
  // Comparison
  compare: {
    sectionTitle: string
    dimensionCol: string
    winnerCol: string
    marginCol: string
    chooseQuarkTitle: string
    chooseCAFTitle: string
  }
  // Posture
  posture: {
    sectionTitle: string
    subsystemCol: string
    specCol: string
    stdCol: string
    adapterCol: string
  }
  // Code Showcase
  code: {
    sectionTitle: string
    tabs: Array<{ label: string; caption: string }>
  }
  // QuickStart
  quickstart: {
    sectionTitle: string
    caption: string
  }
  // Docs Teaser
  docsTeaser: {
    titleLead: string
    titleGrad: string
    description: string
    ctaButton: string
  }
  // Footer
  footer: {
    brandDesc: string
    docsCol: string
    recordsCol: string
    projectCol: string
    firstActor: string
    samples: string
    specs: string
    adrs: string
    verification: string
    performance: string
    openQuestions: string
    conventions: string
    repository: string
    ciStatus: string
    license: string
    contributing: string
    bottomNote: string
  }
  // Docs UI
  docsUI: {
    searchPlaceholder: string
    searchEmpty: string
    documentsToggle: string
    allDocs: string
    categories: {
      guide: string
      reference: string
      spec: string
      adr: string
    }
    readTime: string
    onThisPage: string
    prevPage: string
    nextPage: string
    missingTitle: string
    missingText: string
    backToDocs: string
    browseTitleLead: string
    browseTitleGrad: string
    browseDesc: string
    browseFilterAll: string
  }
}

export const TRANSLATIONS: Record<Lang, Translation> = {
  en: {
    nav: {
      features: 'Features',
      proof: 'Proof',
      performance: 'Performance',
      compare: 'Compare',
      start: 'Get started',
      docs: 'Wiki & docs',
    },
    hero: {
      pill: '11 rounds of red-teaming — the mailbox still wins',
      titleLead: 'The actor engine that ',
      titleGrad: 'proves',
      titleTail: ' its hot path.',
      lede: 'Quark is a header-first C++23 actor engine — work-stealing scheduler, hybrid sync/async handlers, cluster distribution to 10³–10⁴ nodes, durable persistence and supervision. The runtime owns optimization; you express only intent.',
      ctaFirstActor: 'Write your first actor',
      ctaDocs: 'Browse the wiki',
    },
    stats: {
      testsLabel: 'correctness tests',
      testsNote: 'ASan / UBSan / TSan clean',
      docsLabel: 'design documents',
      docsNote: '28 RFC specs + ADRs + reference',
      p50Label: 'mailbox p50',
      p50Note: 'enqueue → dequeue, budget ≤ 100 ns',
      throughputLabel: 'messages / second',
      throughputNote: 'full tell lifecycle, one core',
    },
    features: {
      sectionTitle: 'Core capabilities',
      items: [
        {
          title: 'Header-first, std-only C++23',
          body: 'std::expected results, coroutine handlers, std::stop_token cancellation, std::pmr shard allocators, concepts and deducing-this. No RTTI or reflection on the hot path.',
        },
        {
          title: 'Hybrid handler execution',
          body: 'Synchronous by default and drained inline at zero cost. An actor opts into quark::task<> coroutine handlers per message type when it needs async I/O.',
        },
        {
          title: 'Zero-cost intent declaration',
          body: 'CRTP policy types — Sequential, Priority<P>, Placement<…>, DrainBudget<N> — are template parameters resolved to metadata at startup. No attributes, no runtime config.',
        },
        {
          title: 'Work-stealing scheduler',
          body: 'Workers borrow activations from shards across K priority bands. Per-actor mailbox FIFO is inviolable, and UniformFIFO objdumps byte-identical to a single MPSC.',
        },
        {
          title: 'Point-to-point and fan-out messaging',
          body: 'tell / ask, credit-controlled streaming replies (ask_stream), best-effort at-most-once broadcast (Topic<M>), and ordered reliable N-subscriber fan-out (FanOut<M, Policy>).',
        },
        {
          title: 'Inbound stream ingestion',
          body: 'A pre-allocated per-stream SPSC credit-ring with derived credit — no shared counter, zero-copy, and backpressure instead of shedding. 140.8 M frames/s sustained.',
        },
        {
          title: 'Cluster distribution at scale',
          body: 'HRW / VirtualBins O(1) placement, SWIM membership, bounded partial-view plus DHT-relay for 10³–10⁴-node topologies, IPv4 and IPv6 endpoints.',
        },
        {
          title: 'Durable persistence and reminders',
          body: 'Snapshot and event-sourced durability, plus at-least-once wall-clock wake-ups that flatten a 10⁶-at-9 PM mass-due wave to peak == fire_rate.',
        },
        {
          title: 'Failure supervision',
          body: 'A zero-cost guarded handler core with restart / resume / stop / escalate policies, escalation-storm guards, and a poison ask that returns an error instead of hanging.',
        },
        {
          title: 'Resource governance',
          body: 'Per-node token-bucket rate limiting, deadline-aware load shedding, circuit breaking and bounded queues — with cross-node backpressure signalling on the transport.',
        },
        {
          title: 'Node security',
          body: 'mTLS node-to-node transport with live certificate rotation and revocation enforced against already-open sessions; wire-arrived principals propagate into handler context.',
        },
        {
          title: 'Deterministic simulation testing',
          body: 'Fault injection without real time or threads. BMC-DPOR interleaving exploration finds a planted bug 100/100 where random search hits 66–72%.',
        },
      ],
    },
    proof: {
      sectionTitle: 'The 4-step proof workflow',
      steps: [
        {
          step: 'Design',
          title: 'Design',
          body: 'An architect proposes a concrete design against the locked RFC decisions and states falsifiable fast / safe / correct claims — each paired with an experiment that would disprove it.',
        },
        {
          step: 'Red-team',
          title: 'Red-team',
          body: 'An adversary steelmans the design, then attacks it hardest where it claims to be fast or safe: data races, UB, ABA, torn state, lost messages, hidden allocations.',
        },
        {
          step: 'Prove',
          title: 'Prove',
          body: 'Every design is implemented in real C++23, compiled under GCC and Clang, run under ASan/UBSan/TSan and benchmarked. Claims come back CORRECT, WRONG or INCONCLUSIVE.',
        },
        {
          step: 'Judge',
          title: 'Judge',
          body: 'A judge weighs which claims survived red-teaming AND were proven by executed code, picks a winner, and writes the durable ADR. Evidence outranks elegance.',
        },
      ],
    },
    perf: {
      sectionTitle: 'Measured performance budgets',
      featureCol: 'Feature',
      specCol: 'Spec',
      metricCol: 'Metric',
      measuredCol: 'Measured',
      budgetCol: 'Budget',
      statusCol: 'Status',
      verdictGoal: 'Goal met',
      verdictFree: 'Zero cost',
      invariantGatesTitle: 'Invariant gates enforced on CI',
    },
    compare: {
      sectionTitle: 'Comparison vs CAF (C++ Actor Framework)',
      dimensionCol: 'Dimension',
      winnerCol: 'Winner',
      marginCol: 'Margin',
      chooseQuarkTitle: 'Choose Quark when…',
      chooseCAFTitle: 'Choose CAF when…',
    },
    posture: {
      sectionTitle: 'System posture & seams',
      subsystemCol: 'Subsystem',
      specCol: 'Spec',
      stdCol: 'Standard default',
      adapterCol: 'Pluggable adapter seam',
    },
    code: {
      sectionTitle: 'Code examples',
      tabs: [
        {
          label: 'Hello counter',
          caption: 'One actor, driven by tell (fire-and-forget) and ask (request/reply) over the real engine.',
        },
        {
          label: 'Fault containment',
          caption: 'A throwing handler is contained at the boundary. The lane survives, and a poison ask resolves as an error rather than hanging the caller.',
        },
        {
          label: 'Durable reminders',
          caption: 'Wall-clock, persisted, at-least-once wake-ups. 10,000 reminders due at one instant flatten to a peak of fire_rate per second — never a 10,000-wide spike.',
        },
      ],
    },
    quickstart: {
      sectionTitle: 'Get started in 30 seconds',
      caption: 'Building, testing, and running samples with CMake.',
    },
    docsTeaser: {
      titleLead: 'The complete ',
      titleGrad: 'design record',
      description: '86 documents — RFC specifications, executed decision records, guides, and verification reports — rendered directly from the repository.',
      ctaButton: 'Explore 86 documents',
    },
    footer: {
      brandDesc: 'A high-performance C++23 actor engine. The runtime owns optimization; developers express only intent.',
      docsCol: 'Docs',
      recordsCol: 'Records',
      projectCol: 'Project',
      firstActor: 'First actor',
      samples: 'Samples',
      specs: 'Specifications',
      adrs: 'Decision records',
      verification: 'Verification',
      performance: 'Performance',
      openQuestions: 'Open questions',
      conventions: 'Conventions',
      repository: 'Repository',
      ciStatus: 'CI status',
      license: 'MIT license',
      contributing: 'Contributing',
      bottomNote: 'MIT licensed. Built from the repository’s own specs, ADRs and measured results.',
    },
    docsUI: {
      searchPlaceholder: 'Search documents…',
      searchEmpty: 'No document matches',
      documentsToggle: 'Documents',
      allDocs: 'All documents',
      categories: {
        guide: 'Guide',
        reference: 'Reference',
        spec: 'Specification',
        adr: 'Decision record',
      },
      readTime: 'min read',
      onThisPage: 'On this page',
      prevPage: '← Previous',
      nextPage: 'Next →',
      missingTitle: 'Document not found',
      missingText: 'No document is published at this address.',
      backToDocs: 'Back to all documents',
      browseTitleLead: 'The complete ',
      browseTitleGrad: 'design record',
      browseDesc: 'documents — RFC specifications, executed decision records, guides and verification history — rendered from the repository.',
      browseFilterAll: 'All',
    },
  },
  vi: {
    nav: {
      features: 'Tính năng',
      proof: 'Chứng minh',
      performance: 'Hiệu năng',
      compare: 'So sánh',
      start: 'Bắt đầu',
      docs: 'Wiki & Tài liệu',
    },
    hero: {
      pill: '11 vòng phản biện — mailbox vẫn giành chiến thắng',
      titleLead: 'Engine actor chứng minh được ',
      titleGrad: 'hot path',
      titleTail: ' của mình.',
      lede: 'Quark là engine actor C++23 header-first — scheduler work-stealing, handler lai đồng bộ/bất đồng bộ, phân tán cluster 10³–10⁴ node, persistence bền vững và giám sát lỗi. Runtime chịu trách nhiệm tối ưu; bạn chỉ cần thể hiện ý định.',
      ctaFirstActor: 'Viết actor đầu tiên của bạn',
      ctaDocs: 'Duyệt tài liệu wiki',
    },
    stats: {
      testsLabel: 'kiểm thử tính đúng đắn',
      testsNote: 'Đã qua kiểm thử ASan / UBSan / TSan',
      docsLabel: 'tài liệu thiết kế',
      docsNote: '28 đặc tả RFC + ADR + tài liệu tham chiếu',
      p50Label: 'mailbox latency p50',
      p50Note: 'enqueue → dequeue, ngân sách ≤ 100 ns',
      throughputLabel: 'bản tin / giây',
      throughputNote: 'vòng đời tell đầy đủ, 1 core',
    },
    features: {
      sectionTitle: 'Tính năng cốt lõi',
      items: [
        {
          title: 'C++23 chuẩn, Header-first',
          body: 'kết quả std::expected, handler coroutine, std::stop_token cho việc hủy, bộ cấp phát shard std::pmr, concepts và deducing-this. Không dùng RTTI hoặc reflection trên hot path.',
        },
        {
          title: 'Xử lý handler lai',
          body: 'Mặc định đồng bộ và thu dọn inline với chi phí bằng 0. Actor chọn handler coroutine quark::task<> cho từng loại bản tin khi cần I/O bất đồng bộ.',
        },
        {
          title: 'Khai báo ý định chi phí bằng 0',
          body: 'Các kiểu policy CRTP — Sequential, Priority<P>, Placement<…>, DrainBudget<N> — là tham số template được giải quyết thành metadata khi khởi động.',
        },
        {
          title: 'Bộ lập lịch Work-stealing',
          body: 'Các worker mượn activation từ các shard trên K dải độ ưu tiên. FIFO mailbox của từng actor luôn được đảm bảo tuyệt đối, và UniformFIFO xuất mã máy trùng khớp MPSC.',
        },
        {
          title: 'Truyền tin point-to-point và fan-out',
          body: 'tell / ask, phản hồi streaming kiểm soát tín dụng (ask_stream), broadcast best-effort at-most-once (Topic<M>), và fan-out N người nhận tin cậy (FanOut<M, Policy>).',
        },
        {
          title: 'Nạp luồng dữ liệu vào (Inbound Stream)',
          body: 'Vòng tín dụng SPSC tiền cấp phát trên mỗi stream — không dùng biến đếm chia sẻ, zero-copy, và kiểm soát áp lực ngược thay vì drop gói. 140.8 M frame/s.',
        },
        {
          title: 'Phân tán cluster quy mô lớn',
          body: 'Định vị HRW / VirtualBins O(1), thành viên SWIM, partial-view có giới hạn kết hợp DHT-relay cho cấu trúc 10³–10⁴ node, hỗ trợ IPv4 và IPv6.',
        },
        {
          title: 'Persistence bền vững và nhắc giờ (Reminder)',
          body: 'Độ bền vững snapshot và event-sourcing, cùng các thức dậy thời gian thực at-least-once làm phẳng đợt sóng 10⁶ nhắc giờ đồng thời thành peak == fire_rate.',
        },
        {
          title: 'Giám sát lỗi (Supervision)',
          body: 'Lõi handler bảo vệ chi phí bằng 0 với chính sách restart / resume / stop / escalate, chống bão leo thang lỗi, và poison ask trả về lỗi thay vì treo.',
        },
        {
          title: 'Quản trị tài nguyên (Governance)',
          body: 'Giới hạn tốc độ token-bucket mỗi node, xả tải nhận biết deadline, circuit breaking và hàng chờ có giới hạn — với tín hiệu áp lực ngược liên node.',
        },
        {
          title: 'Bảo mật node',
          body: 'Truyền tải node-to-node mTLS với xoay vòng certificate và thu hồi trực tiếp áp dụng cho session đã mở; thông tin định danh lan truyền vào context handler.',
        },
        {
          title: 'Kiểm thử mô phỏng định tính',
          body: 'Bơm lỗi không cần thời gian thực hay thread thực. Tìm kiếm đan xen BMC-DPOR phát hiện lỗi được gài 100/100 lần so với 66–72% của tìm kiếm ngẫu nhiên.',
        },
      ],
    },
    proof: {
      sectionTitle: 'Quy trình chứng minh 4 bước',
      steps: [
        {
          step: 'Thiết kế',
          title: 'Design',
          body: 'Kiến trúc sư đề xuất thiết kế cụ thể dựa trên các quyết định RFC và đưa ra các khẳng định về tốc độ / an toàn / tính đúng đắn — mỗi khẳng định kèm thử nghiệm kiểm chứng.',
        },
        {
          step: 'Phản biện',
          title: 'Red-team',
          body: 'Đội phản biện phân tích thiết kế và tấn công mạnh nhất vào những điểm tuyên bố nhanh/an toàn: data race, UB, ABA, torn state, mất bản tin, cấp phát ẩn.',
        },
        {
          step: 'Thực chứng',
          title: 'Prove',
          body: 'Mọi thiết kế được cài đặt bằng C++23 thực tế, biên dịch qua GCC và Clang, chạy dưới ASan/UBSan/TSan và benchmark. Kết quả trả về CORRECT, WRONG hoặc INCONCLUSIVE.',
        },
        {
          step: 'Đánh giá',
          title: 'Judge',
          body: 'Trọng tài xem xét các khẳng định đã vượt qua phản biện VÀ được chứng minh bằng code chạy thực tế, chọn phương án chiến thắng và viết ADR bền vững.',
        },
      ],
    },
    perf: {
      sectionTitle: 'Ngân sách hiệu năng thực tế',
      featureCol: 'Tính năng',
      specCol: 'Spec',
      metricCol: 'Chỉ số',
      measuredCol: 'Thực tế',
      budgetCol: 'Ngân sách',
      statusCol: 'Trạng thái',
      verdictGoal: 'Đạt mục tiêu',
      verdictFree: 'Chi phí bằng 0',
      invariantGatesTitle: 'Cổng bất biến bắt buộc trên CI',
    },
    compare: {
      sectionTitle: 'So sánh với C++ Actor Framework (CAF)',
      dimensionCol: 'Tiêu chí',
      winnerCol: 'Thắng',
      marginCol: 'Mức chênh lệch',
      chooseQuarkTitle: 'Khi nào chọn Quark…',
      chooseCAFTitle: 'Khi nào chọn CAF…',
    },
    posture: {
      sectionTitle: 'Triết lý thiết kế & điểm mở rộng',
      subsystemCol: 'Phân hệ',
      specCol: 'Spec',
      stdCol: 'Mặc định std',
      adapterCol: 'Adapter mở rộng',
    },
    code: {
      sectionTitle: 'Ví dụ mã nguồn',
      tabs: [
        {
          label: 'Hello counter',
          caption: 'Một actor chạy bằng tell (không chờ) và ask (yêu cầu/phản hồi) trên engine thực tế.',
        },
        {
          label: 'Khoanh vùng lỗi',
          caption: 'Handler ném ngoại lệ được khoanh vùng ngay tại ranh giới. Lane tiếp tục chạy, và poison ask trả về lỗi thay vì làm treo bên gọi.',
        },
        {
          label: 'Reminder bền vững',
          caption: 'Thức dậy theo thời gian thực, lưu trữ bền vững, at-least-once. 10.000 reminder hết hạn cùng lúc được làm phẳng tối đa fire_rate mỗi giây.',
        },
      ],
    },
    quickstart: {
      sectionTitle: 'Bắt đầu nhanh trong 30 giây',
      caption: 'Biên dịch, kiểm thử và chạy mẫu bằng CMake.',
    },
    docsTeaser: {
      titleLead: 'Toàn bộ ',
      titleGrad: 'hồ sơ thiết kế',
      description: '86 tài liệu — đặc tả RFC, bản ghi quyết định ADR, hướng dẫn và báo cáo kiểm thử — được render trực tiếp từ kho mã nguồn.',
      ctaButton: 'Khám phá 86 tài liệu',
    },
    footer: {
      brandDesc: 'Engine actor C++23 hiệu năng cao. Runtime chịu trách nhiệm tối ưu; lập trình viên chỉ thể hiện ý định.',
      docsCol: 'Tài liệu',
      recordsCol: 'Hồ sơ',
      projectCol: 'Dự án',
      firstActor: 'Actor đầu tiên',
      samples: 'Mẫu mã nguồn',
      specs: 'Đặc tả RFC',
      adrs: 'Bản ghi ADR',
      verification: 'Báo cáo kiểm thử',
      performance: 'Báo cáo hiệu năng',
      openQuestions: 'Câu hỏi mở',
      conventions: 'Quy ước lập trình',
      repository: 'Kho mã nguồn',
      ciStatus: 'Trạng thái CI',
      license: 'Giấy phép MIT',
      contributing: 'Đóng góp',
      bottomNote: 'Giấy phép MIT. Xây dựng trực tiếp từ spec, ADR và số liệu đo đạc thực tế của repository.',
    },
    docsUI: {
      searchPlaceholder: 'Tìm kiếm tài liệu…',
      searchEmpty: 'Không tìm thấy tài liệu phù hợp với',
      documentsToggle: 'Tài liệu',
      allDocs: 'Tất cả tài liệu',
      categories: {
        guide: 'Hướng dẫn',
        reference: 'Tham chiếu',
        spec: 'Đặc tả RFC',
        adr: 'Bản ghi ADR',
      },
      readTime: 'phút đọc',
      onThisPage: 'Trọng tâm bài viết',
      prevPage: '← Trước',
      nextPage: 'Tiếp →',
      missingTitle: 'Không tìm thấy tài liệu',
      missingText: 'Không có tài liệu nào được xuất bản tại địa chỉ này.',
      backToDocs: 'Quay lại tất cả tài liệu',
      browseTitleLead: 'Toàn bộ ',
      browseTitleGrad: 'hồ sơ thiết kế',
      browseDesc: 'tài liệu — đặc tả RFC, bản ghi quyết định ADR, hướng dẫn và lịch sử kiểm thử — được render từ kho mã nguồn.',
      browseFilterAll: 'Tất cả',
    },
  },
}

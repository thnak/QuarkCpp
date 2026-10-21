# Báo cáo Báo chí & Benchmarks của Quark (023)

Bộ kiểm thử microbenchmark biến các tuyên bố hot-path của RFC (*zero-cost*, *O(1)*, *không cấp phát bộ nhớ trên hot-path*) thành **kết quả kiểm chứng thực tế** đối chiếu với ngân sách [023 Performance Targets and Budgets](023-Performance-Targets-and-Budgets).

---

## 📊 Biểu đồ Hiệu năng & So sánh với CAF

<div class="bench-chart-container" style="margin: 2rem 0; padding: 1.5rem; background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.1); border-radius: 12px;">
  <h3 style="margin-top: 0; color: #38bdf8;">1. Latency & Băng thông: Quark vs CAF</h3>
  <p style="color: #94a3b8; font-size: 0.9rem; margin-bottom: 1.5rem;">Càng thấp càng tốt đối với độ trễ (latency) / thời gian. Đo đạc trên tải đa luồng 12 workers.</p>
  
  <div style="display: flex; flex-direction: column; gap: 1.25rem;">
    <!-- Metric 1: Ask Latency (p50, 12 workers) -->
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.9rem; font-weight: 600; margin-bottom: 0.35rem;">
        <span>Độ trễ Ask p50 (12 workers)</span>
        <span style="color: #38bdf8;">Quark nhanh hơn ~11.0×</span>
      </div>
      <div style="display: flex; flex-direction: column; gap: 0.4rem;">
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #38bdf8; font-weight: 600;">Quark</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 9%; height: 100%; background: linear-gradient(90deg, #0284c7, #38bdf8); border-radius: 4px; display: flex; align-items: center; padding-left: 8px; font-size: 0.75rem; font-weight: bold; color: white;">226 ns</div>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #94a3b8;">CAF</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 100%; height: 100%; background: rgba(244, 63, 94, 0.75); border-radius: 4px; display: flex; align-items: center; justify-content: flex-end; padding-right: 8px; font-size: 0.75rem; font-weight: bold; color: white;">2,480 ns</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Metric 2: Ask Latency (p50, 1 worker) -->
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.9rem; font-weight: 600; margin-bottom: 0.35rem;">
        <span>Độ trễ Ask p50 (1 worker)</span>
        <span style="color: #38bdf8;">Quark nhanh hơn 3.3×</span>
      </div>
      <div style="display: flex; flex-direction: column; gap: 0.4rem;">
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #38bdf8; font-weight: 600;">Quark</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 30%; height: 100%; background: linear-gradient(90deg, #0284c7, #38bdf8); border-radius: 4px; display: flex; align-items: center; padding-left: 8px; font-size: 0.75rem; font-weight: bold; color: white;">147 ns</div>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #94a3b8;">CAF</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 100%; height: 100%; background: rgba(244, 63, 94, 0.75); border-radius: 4px; display: flex; align-items: center; justify-content: flex-end; padding-right: 8px; font-size: 0.75rem; font-weight: bold; color: white;">485 ns</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Metric 3: Spawn 10k Actors (12 workers) -->
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.9rem; font-weight: 600; margin-bottom: 0.35rem;">
        <span>Thời gian Spawn 10,000 Actors (12 workers)</span>
        <span style="color: #38bdf8;">Quark nhanh hơn 5.0×</span>
      </div>
      <div style="display: flex; flex-direction: column; gap: 0.4rem;">
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #38bdf8; font-weight: 600;">Quark</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 20%; height: 100%; background: linear-gradient(90deg, #0284c7, #38bdf8); border-radius: 4px; display: flex; align-items: center; padding-left: 8px; font-size: 0.75rem; font-weight: bold; color: white;">1.2 ms</div>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #94a3b8;">CAF</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 100%; height: 100%; background: rgba(244, 63, 94, 0.75); border-radius: 4px; display: flex; align-items: center; justify-content: flex-end; padding-right: 8px; font-size: 0.75rem; font-weight: bold; color: white;">6.0 ms</div>
          </div>
        </div>
      </div>
    </div>
  </div>
</div>

<div class="bench-chart-container" style="margin: 2rem 0; padding: 1.5rem; background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.1); border-radius: 12px;">
  <h3 style="margin-top: 0; color: #10b981;">2. Ngân sách Hiệu năng Quark Hot-Path (Đặc tả 023)</h3>
  <p style="color: #94a3b8; font-size: 0.9rem; margin-bottom: 1.5rem;">Khoảng an toàn thực tế so với ngân sách 023 (Thanh ngắn hơn = tối ưu hiệu quả hơn).</p>

  <div style="display: flex; flex-direction: column; gap: 1rem;">
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>Mailbox p50 (59 ns thực tế / ≤ 100 ns ngân sách)</span>
        <span style="color: #10b981; font-weight: bold;">59% ngân sách</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 59%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>

    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>Overhead Ask p50 (147 ns thực tế / ≤ 1,000 ns ngân sách)</span>
        <span style="color: #10b981; font-weight: bold;">15% ngân sách</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 15%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>

    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>Tra cứu VirtualBins Placement p99 (12.5 ns thực tế / ≤ 20 ns ngân sách)</span>
        <span style="color: #10b981; font-weight: bold;">62% ngân sách</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 62%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>

    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>Mã hóa Tagless Wire Encode p99 (50 ns thực tế / ≤ 200 ns ngân sách)</span>
        <span style="color: #10b981; font-weight: bold;">25% ngân sách</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 25%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>
  </div>
</div>

---

## Các chỉ số hiệu năng chính

| Phân hệ | Chỉ số đo đạc | Kết quả thực tế | Ngân sách cam kết | Trạng thái |
| :--- | :--- | :--- | :--- | :--- |
| **Mailbox tell** | Latency p50 enqueue → dequeue | **59 ns** | ≤ 100 ns | Đạt mục tiêu |
| **Scheduler throughput** | Băng thông đầy đủ 1 core | **11.0 M/s** | ≥ 10 M/s | Đạt mục tiêu |
| **Ask request/reply** | Overhead engine p50 / p99 | **147 / 226 ns** | p50 ≤ 1 µs | Đạt mục tiêu |
| **Inbound streaming** | Tốc độ nạp duy trì | **140.8 M frames/s** | ≥ 10 M/s | Đạt mục tiêu |
| **VirtualBins Placement** | Tra cứu O(1) độc lập N | **12.5 ns** | ≤ 20 ns | Đạt mục tiêu |

---

## So sánh trực tiếp với CAF (C++ Actor Framework)

- **Ask Latency (p50)**: Quark nhanh hơn CAF **3.3×** (1 worker) và **~11×** (12 workers).
- **Khởi tạo Spawn (10k actors)**: Quark nhanh hơn **1.55× đến 5.0×**.
- **Độ tin cậy hot path**: MPSC mailbox của Quark loại bỏ 100% allocation ẩn trên hot-path.

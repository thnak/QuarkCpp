# Hướng dẫn viết Actor đầu tiên với Quark C++23

Chào mừng bạn đến với **Quark**, engine actor C++23 header-first với hiệu năng cực cao. Bài viết này sẽ hướng dẫn bạn viết actor đầu tiên sử dụng hai cơ chế truyền tin cơ bản: **`tell`** (gửi không chờ response) và **`ask`** (yêu cầu & phản hồi).

---

## 1. Định nghĩa Actor và Giao thức (Protocol)

Trong Quark, một actor kế thừa từ `quark::Actor` bằng kỹ thuật CRTP. Các policy như `Sequential`, `Priority<P>`, hay `DrainBudget<N>` được xác định ngay tại thời điểm biên dịch (chi phí bằng 0 ở runtime).

```cpp
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

// Định nghĩa các loại tin nhắn
struct Add { int amount; };
struct GetTotal {};

// Actor Counter duy trì trạng thái tổng tích lũy
struct Counter : Actor<Counter, Sequential, Priority<0>, DrainBudget<16>> {
    using protocol = Protocol<Add, Ask<GetTotal, int>>;

    // Handler cho tin nhắn Add (tell)
    void handle(const Add& a) noexcept { 
        total_ += a.amount; 
    }
    
    // Handler cho tin nhắn Ask (request / reply)
    void handle(const Ask<GetTotal, int>& m) noexcept { 
        m.respond(total_); 
    }

private:
    int total_ = 0;
};
```

---

## 2. Khởi tạo Engine và Đăng ký Actor

```cpp
int main() {
    // 1. Tạo bể chứa tin nhắn (MessagePool)
    detail::MessagePool pool(1024);
    Counter counter;
    auto activation = std::make_unique<Activation>(&counter, Counter::dispatch_table(), pool.sink());

    // 2. Khởi tạo Engine với 1 worker thread và 1 shard
    Engine<PriorityBands<2>> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    register_actor<Counter>(eng, /*key*/ 42, *activation);

    // 3. Router để gửi bản tin
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Counter> counter_ref = router.get<Counter>(42);
    eng.start();

    // 4. Gửi 100 tin nhắn 'tell' liên tiếp
    for (int i = 1; i <= 100; ++i) {
        counter_ref.tell(Add{i});
    }

    // 5. Gửi bản tin 'ask' nhận kết quả
    result<int> total = block_on(counter_ref.ask<int>(GetTotal{}));
    std::printf("Tổng nhận được: %d\n", total.value()); // Kết quả: 5050

    eng.stop();
}
```

---

## 3. Điểm nổi bật của Quark

- **Zero-allocation hot path**: Mailbox MPSC cực nhẹ với latency p50 chỉ ~59 ns.
- **Biên dịch an toàn**: Giao thức `Protocol<...>` kiểm tra kiểu bản tin ngay lúc compile.
- **Không RTTI/Reflection**: Tối ưu mã máy trực tiếp cho vi xử lý hiện đại.

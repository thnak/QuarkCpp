# Các bài mẫu minh họa (Samples) trong Quark

Quark đi kèm với 22 mẫu minh họa runnable thực tế nằm trong thư mục `samples/`. Bạn có thể biên dịch toàn bộ bằng cách bật cờ `-DQUARK_BUILD_SAMPLES=ON` khi chạy CMake.

---

## Danh sách mẫu tiêu biểu

### 1. `01_hello_counter`
- **Chủ đề**: Khởi tạo actor cơ bản, truyền tin `tell` và `ask`.
- **Nội dung**: Minh họa một actor đếm số nhận 100 tin nhắn `tell` đồng bộ và một tin nhắn `ask` để đọc giá trị cuối cùng.

### 2. `02_supervised_worker`
- **Chủ đề**: Giám sát lỗi (Supervision) và khoanh vùng ngoại lệ.
- **Nội dung**: Minh họa cách lõi handler làm sụp nạp ngoại lệ `poison tell` hoặc `poison ask` mà không làm crash worker thread hay hỏng trạng thái engine.

### 3. `14_durable_reminders`
- **Chủ đề**: Hẹn giờ nhắc nhở bền vững theo thời gian thực.
- **Nội dung**: Minh họa dịch vụ `ReminderService` làm phẳng đợt sóng hàng nghìn reminder hết hạn cùng lúc thành luồng ổn định có tốc độ kiểm soát `fire_rate`.

---

## Cách biên dịch và chạy

```bash
# Tạo thư mục build và bật biên dịch samples
cmake -S . -B build -DQUARK_BUILD_SAMPLES=ON

# Biên dịch sample
cmake --build build --target 01_hello_counter -j4

# Chạy sample
./build/samples/01_hello_counter
```

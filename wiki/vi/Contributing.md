# Hướng dẫn Đóng góp (Contributor Guide)

Cảm ơn bạn đã quan tâm đóng góp cho **Quark**! Quark tuân thủ quy trình phát triển dựa trên bằng chứng và kiểm thử nghiêm ngặt.

---

## Quy trình 4 bước phát triển (4-step proof workflow)

1. **Design**: Viết thiết kế cụ thể kèm các tuyên bố có thể kiểm chứng bằng thực nghiệm.
2. **Red-team**: Phản biện thiết kế, tìm kiếm data race, undefined behavior, ABA hay lost messages.
3. **Prove**: Cài đặt bằng C++23 thực tế, kiểm tra bằng ASan/UBSan/TSan và đo đạc benchmark.
4. **Judge**: Đưa ra quyết định ADR dựa trên bằng chứng mã chạy thực tế.

---

## Lệnh kiểm thử nhanh trước khi gửi PR

```bash
# 1. Kiểm tra tính đúng đắn trên Release build
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build -j4 --output-on-failure

# 2. Kiểm tra với Sanitizer (Address & Undefined)
cmake -S . -B build-asan -DQUARK_SANITIZE="address;undefined"
cmake --build build-asan -j4
ctest --test-dir build-asan -j4 --output-on-failure

# 3. Định dạng mã nguồn theo standard .clang-format
clang-format -i include/quark/**/*.hpp src/**/*.cpp
```

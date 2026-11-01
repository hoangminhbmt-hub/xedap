# XeDap Guard — báo mất xe đạp qua Bluetooth

Hệ thống gồm hai phần:
- **Firmware** (`firmware/XeDap/XeDap.ino`): chạy trên ESP32-C3 gắn trên xe. Chỉ phát Bluetooth LE và gửi trạng thái (pin, tín hiệu) mỗi giây.
- **Giao diện** (`xedap.html`): mở trên điện thoại Android (Chrome) hoặc PC. Theo dõi xe và báo động khi mất kết nối hoặc tín hiệu yếu.

## Phần cứng
- ESP32-C3 SuperMini, pin 18650.
- Cầu chia đo pin: Pin(+) — 220k — GPIO3 — 220k — GND. Xem `docs/so-do-noi-day.svg`.

## Nạp firmware lần đầu
1. Cài Arduino IDE, thêm board ESP32 (core từ 3.0 trở lên).
2. Cài thư viện **NimBLE-Arduino** (từ 2.1 trở lên).
3. Chọn board **ESP32C3 Dev Module**, bật **USB CDC On Boot: Enabled**.
4. Mở `firmware/XeDap/XeDap.ino` và nạp.
5. Mở Serial Monitor (115200). Dòng `PIN ghep doi: ...` là mã ghép đôi của xe. **Ghi lại mã này.**

Mã ghép đôi được sinh ngẫu nhiên, không có trong mã nguồn. Có thể đổi mã trong giao diện bằng nút **Đổi PIN**.

## Quy tắc khi làm việc với kho này
- **Không** đưa mã PIN thật, mật khẩu Wi-Fi, khóa, hay bất kỳ thông tin cá nhân nào vào kho.
- Mọi thay đổi định dạng dữ liệu giữa firmware và giao diện phải cập nhật cả hai bên và ghi lại trong `CLAUDE.md`.
- Trước khi phát hành, kiểm thử trên xe thật, không chỉ trên bàn.

# XeDap Guard — báo mất xe đạp qua Bluetooth

Hệ thống gồm hai phần:
- **Firmware** (`firmware/XeDap/XeDap.ino`): chạy trên ESP32-C3 gắn trên xe. Đo pin, đọc cảm biến rung, phát Bluetooth.
- **Giao diện** (`xedap.html`): mở trên điện thoại Android (Chrome) hoặc PC. Theo dõi xe, báo động, cập nhật firmware.

## Phần cứng
- ESP32-C3 SuperMini.
- Cảm biến rung MPU6050: SDA GPIO4, SCL GPIO5, INT GPIO0.
- Pin: 3 viên 18650 song song, 3,7V, sạc ngoài.
- Cầu chia đo pin: Pin(+) — 220k — GPIO3 — 220k — GND. Xem `docs/so-do-noi-day-v1.2.pdf` để in.

## Nạp firmware lần đầu (qua USB)
1. Cài Arduino IDE, thêm board ESP32 (core từ 3.0 trở lên).
2. Cài thư viện **NimBLE-Arduino** (từ 2.1 trở lên).
3. Chọn board **ESP32C3 Dev Module**, bật **USB CDC On Boot: Enabled**.
4. Chọn **Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA)**. Bắt buộc để cập nhật qua Bluetooth sau này.
5. Mở `firmware/XeDap/XeDap.ino` và nạp.
6. Mở Serial Monitor (115200). Dòng `PIN ghep doi: ...` là mã ghép đôi. Ghi lại mã này.

Mã ghép đôi được sinh ngẫu nhiên, không có trong mã nguồn.

## Cập nhật firmware qua điện thoại (không cần cáp)
1. Biên dịch trong Arduino IDE, chọn **Sketch → Export Compiled Binary**, lấy file `.bin`.
2. Mở `xedap.html` trên điện thoại, kết nối xe, mục **Cập nhật firmware**, chọn file `.bin`, bấm **Cập nhật xe**.
3. Giữ điện thoại gần xe, xe đang thức, đến khi báo xong. Xe tự khởi động lại, bấm **Kết nối lại**.

## Mở khóa khi xe bị khóa
Xe khóa sau 3 lần bị rung đánh thức mà không có điện thoại. Để mở khóa, rung xe đúng mẫu đã cài (mặc định 3 cú trong 5 giây). Các hằng số `UNLOCK_TAPS` và `UNLOCK_WINDOW_MS` nằm đầu `XeDap.ino`.

## Quy tắc khi làm việc với kho này
- **Không** đưa mã PIN thật, mật khẩu Wi-Fi, khóa, hay thông tin cá nhân vào kho.
- Mọi thay đổi định dạng dữ liệu giữa firmware và giao diện phải cập nhật cả hai bên, ghi trong `CLAUDE.md`.
- Mọi thay đổi đấu nối phải cập nhật bản vẽ A4 trong `docs/`.
- Trước khi phát hành, kiểm thử trên xe thật, không chỉ trên bàn.

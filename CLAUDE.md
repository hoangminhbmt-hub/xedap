# CLAUDE.md — XeDap Guard

## Mục đích
Báo mất xe đạp qua Bluetooth LE. Firmware ESP32-C3 phát trạng thái, giao diện điện thoại báo động.

## Cấu trúc
- `firmware/XeDap/XeDap.ino` — firmware ESP32-C3 (NimBLE-Arduino).
- `xedap.html` — giao diện Web Bluetooth (Chrome Android/PC). Tệp đơn, không có bước build.
- `manifest.json`, `icon-192.png`, `icon-512.png` — cài đặt như ứng dụng.
- `docs/so-do-noi-day.svg` — sơ đồ đấu nối.

## Định dạng dữ liệu (phải khớp giữa hai bên)
- UUID dịch vụ: `6e7a1001-3c2b-4b8e-9a1f-58c5e1a0b001`
  - Trạng thái (notify): `6e7a1002-...`
  - Lệnh (write): `6e7a1003-...`
- Gói trạng thái: `"<pin %>,<pin mV>,<rssi>,<cờ>"`, ví dụ `"87,4050,-62,-"`. Cờ: `-` bình thường, `L` pin yếu, `S` pin cạn.
- Lệnh: `FIND`, `PIN:<6 số>`, `UNPAIR`.

Nếu đổi bất kỳ điều nào ở trên, phải sửa cả firmware lẫn `xedap.html` trong cùng một lần commit.

## Quy tắc làm việc
- Tóm tắt phương án và chờ chủ xe đồng ý trước khi sửa mã.
- Không đưa mã PIN thật, mật khẩu Wi-Fi, khóa hay token vào kho. Nếu cần giá trị bí mật, dùng tệp `secrets.h` (đã nằm trong `.gitignore`).
- Firmware không có mã PIN cố định. Lần chạy đầu tự sinh mã ngẫu nhiên và lưu trong bộ nhớ xe.
- Mọi thay đổi về báo động phải được kiểm thử trên xe thật trước khi coi là xong.
- Giao tiếp với người dùng bằng tiếng Việt, ngắn gọn, tránh viết tắt không cần thiết.

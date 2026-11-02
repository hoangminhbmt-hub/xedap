# CLAUDE.md — XeDap Guard

## Mục đích
Báo mất xe đạp qua Bluetooth LE. Firmware ESP32-C3 phát trạng thái, giao diện điện thoại báo động.

## Cấu trúc
- `firmware/XeDap/XeDap.ino` — firmware ESP32-C3 (NimBLE-Arduino).
- `xedap.html` — giao diện Web Bluetooth (Chrome Android/PC). Tệp đơn, không có bước build.
- `manifest.json`, `icon-192.png`, `icon-512.png` — cài đặt như ứng dụng.
- `docs/so-do-noi-day-v1.2.svg` và `.pdf` — sơ đồ đấu nối A4 (bản để in). Mọi thay đổi đấu nối phải cập nhật bản vẽ này.

## Định dạng dữ liệu (phải khớp giữa hai bên)
- UUID dịch vụ: `6e7a1001-3c2b-4b8e-9a1f-58c5e1a0b001`
  - Trạng thái (notify): `6e7a1002-...`
  - Lệnh (write): `6e7a1003-...`
- Gói trạng thái: `"<pin %>,<pin mV>,<rssi>,<cờ>,<số lần rung>"`, ví dụ `"87,4050,-62,-,0"`.
  - Cờ: `-` bình thường, `L` pin yếu, `S` pin cạn, `T` có rung khi đang khóa.
  - Số lần rung: đếm trong phiên thức hiện tại, không lưu qua lần ngủ.
- Lệnh: `FIND`, `PIN:<6 số>`, `UNPAIR`, `ARM`, `DISARM`, `SLEEP`.
  - `ARM`/`DISARM`: đồng bộ với nút "Canh xe" trên giao diện. Khi `DISARM`, xe xoá cờ `T` và số lần rung.
  - `SLEEP`: xe ngắt kết nối và ngủ sâu. Giao diện phải đặt `manualDisconnect` trước khi gửi để không báo "mất kết nối".

## Hành vi nguồn điện và rung
- Bật máy hoặc bị rung khi đang ngủ: xe thức, bật Bluetooth.
- Không có điện thoại kết nối trong `SLEEP_AFTER_MS` (60 giây): xe tự ngủ lại.
- Đang có điện thoại kết nối: xe giữ thức cho đến khi nhận `SLEEP`.
- Cảm biến rung MPU6050 nối I2C (SDA GPIO4, SCL GPIO5) và chân ngắt INT GPIO0. INT đánh thức chip khi ngủ.
- Khi pin cạn (dưới 3500 mV, giữ liên tục 10 giây), xe ngủ sâu không có nguồn đánh thức. Chỉ chạy lại khi thay pin.
- Nguồn: 3 viên 18650 song song, cùng hãng, sạc ngoài (tháo pin ra). Không có mạch bảo vệ pin và không có mạch sạc trên xe. Pin nối chân 5V của bo.
- Xe không có công tắc nguồn cứng. Bật bằng rung khi đang ngủ, tắt bằng lệnh SLEEP hoặc tự ngủ sau 60 giây không có điện thoại.
- Trạng thái `armed` và cờ `T` không được lưu sau khi ngủ. Khi thức do rung, xe mặc định là đang khóa.

Nếu đổi bất kỳ điều nào ở trên, phải sửa cả firmware lẫn `xedap.html` trong cùng một lần commit.

## Quy tắc làm việc
- Tóm tắt phương án và chờ chủ xe đồng ý trước khi sửa mã.
- Không đưa mã PIN thật, mật khẩu Wi-Fi, khóa hay token vào kho. Nếu cần giá trị bí mật, dùng tệp `secrets.h` (đã nằm trong `.gitignore`).
- Firmware không có mã PIN cố định. Lần chạy đầu tự sinh mã ngẫu nhiên và lưu trong bộ nhớ xe.
- Mọi thay đổi về báo động phải được kiểm thử trên xe thật trước khi coi là xong.
- Giao tiếp với người dùng bằng tiếng Việt, ngắn gọn, tránh viết tắt không cần thiết.

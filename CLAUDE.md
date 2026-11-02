# CLAUDE.md — XeDap Guard

## Mục đích
Báo mất xe đạp qua Bluetooth LE. Firmware ESP32-C3 phát trạng thái, giao diện điện thoại báo động. Xe không có công tắc nguồn cứng.

## Cấu trúc
- `firmware/XeDap/XeDap.ino` — firmware ESP32-C3 (NimBLE-Arduino, Update cho OTA).
- `xedap.html` — giao diện Web Bluetooth (Chrome Android/PC). Tệp đơn, không có bước build.
- `manifest.json`, `icon-192.png`, `icon-512.png` — cài đặt như ứng dụng.
- `docs/so-do-noi-day-v1.2.svg` và `.pdf` — sơ đồ đấu nối A4 (bản để in). Mọi thay đổi đấu nối phải cập nhật bản vẽ này.

## Định dạng dữ liệu (phải khớp giữa hai bên)
- UUID dịch vụ: `6e7a1001-3c2b-4b8e-9a1f-58c5e1a0b001`
  - Trạng thái (notify): `6e7a1002-...`
  - Lệnh (write): `6e7a1003-...`
  - Điều khiển OTA (write, notify): `6e7a1004-...`
  - Dữ liệu OTA (write): `6e7a1005-...`
- Gói trạng thái: `"<pin %>,<pin mV>,<rssi>,<cờ>,<số lần rung>"`, ví dụ `"87,4050,-62,-,0"`.
  - Cờ: `-` bình thường, `L` pin yếu, `S` pin cạn, `T` có rung khi đang canh.
  - Số lần rung: đếm trong phiên thức hiện tại, không lưu qua lần ngủ.
- Lệnh: `FIND`, `PIN:<6 số>`, `UNPAIR`, `ARM`, `DISARM`, `SLEEP`.
  - `ARM`/`DISARM`: đồng bộ với nút "Canh xe" trên giao diện. Khi `DISARM`, xe xoá cờ `T` và số lần rung.
  - `SLEEP`: xe ngắt kết nối và ngủ sâu. Giao diện phải đặt `manualDisconnect` trước khi gửi để không báo "mất kết nối".
- OTA:
  - `OTA_CTRL` nhận `START:<size>:<crc hex>`, `END`, `ABORT`. Xe trả `OK` hoặc `ERR:<lý do>`.
  - `OTA_DATA` nhận từng đoạn 100 byte, theo đúng thứ tự.
  - CRC32 cùng thuật toán ở cả hai bên (hàm `crc32Update`).

## Hành vi nguồn điện, rung và khóa
- Bị rung khi đang ngủ: xe thức, bật Bluetooth, tăng bộ đếm `rtcWakes`.
- `rtcWakes` đặt lại về 0 khi có điện thoại kết nối.
- Đánh thức không có điện thoại đủ `LOCK_WAKES` (3) lần: xe KHÓA. Trạng thái khóa lưu trong bộ nhớ flash (`locked`).
- Khi khóa: mỗi lần rung chỉ thức `UNLOCK_WINDOW_MS` (5 giây) để đếm cú rung. Đủ `UNLOCK_TAPS` (3) cú thì mở khóa và giữ thức. Không đủ thì ngủ lại ngay.
- Cấp điện lại (cắm pin) khi đang khóa: xe ngủ lại ngay, không mở khóa.
- Không có điện thoại kết nối: xe ngủ lại sau `SLEEP_AFTER_MS` (60 giây), hoặc `SLEEP_AFTER_REPEAT_MS` (10 giây) từ lần đánh thức thứ hai.
- Đang có điện thoại kết nối: xe giữ thức cho đến khi nhận `SLEEP`.
- Cảm biến rung MPU6050 nối I2C (SDA GPIO4, SCL GPIO5) và chân ngắt INT GPIO0. INT đánh thức chip khi ngủ.
- Khi pin cạn (dưới 3500 mV, giữ liên tục 10 giây), xe ngủ sâu không có nguồn đánh thức. Chỉ chạy lại khi thay pin.
- Nguồn: 3 viên 18650 song song, cùng hãng, sạc ngoài (tháo pin ra). Không có mạch bảo vệ pin và không có mạch sạc trên xe. Pin nối chân 5V của bo.
- Xe không có công tắc nguồn cứng. Bật bằng rung khi đang ngủ, tắt bằng lệnh `SLEEP` hoặc tự ngủ sau thời gian chờ.
- Trạng thái `armed` và cờ `T` không được lưu sau khi ngủ. Khi thức do rung, xe mặc định là đang canh.

## Cập nhật firmware (OTA)
- Lần đầu nạp qua USB, trong Arduino IDE chọn Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA)".
- Các lần sau: mở `xedap.html` trên điện thoại, mục "Cập nhật firmware", chọn file `.bin`, bấm "Cập nhật xe". Giữ điện thoại gần xe, xe đang thức.
- Firmware OTA chỉ nhận khi đã ghép đôi và kết nối được mã hóa.
- Cập nhật xong, xe khởi động lại. Bấm "Kết nối lại" trên điện thoại.

## Quy tắc làm việc
- Tóm tắt phương án và chờ chủ xe đồng ý trước khi sửa mã.
- Không đưa mã PIN thật, mật khẩu Wi-Fi, khóa hay token vào kho. Nếu cần giá trị bí mật, dùng tệp `secrets.h` (đã nằm trong `.gitignore`).
- Firmware không có mã PIN cố định. Lần chạy đầu tự sinh mã ngẫu nhiên và lưu trong bộ nhớ xe.
- Các hằng số khóa, mở khóa, ngưỡng rung nằm ở đầu `XeDap.ino` để chủ xe tự chỉnh.
- Mọi thay đổi về báo động phải được kiểm thử trên xe thật trước khi coi là xong.
- Giao tiếp với người dùng bằng tiếng Việt, ngắn gọn, tránh viết tắt không cần thiết.

Nếu đổi bất kỳ điều nào ở trên, phải sửa cả firmware lẫn `xedap.html` trong cùng một lần commit.

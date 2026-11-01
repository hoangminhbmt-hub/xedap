/*
 * XeDap Guard - Báo mất xe đạp qua Bluetooth LE (ESP32-C3)
 *
 * CHƯA KIỂM THỬ TRÊN XE THẬT. Ngưỡng rung và các thanh ghi MPU6050 cần chỉnh khi thử.
 *
 * Phần cứng : ESP32-C3 SuperMini + pin 18650 (nối chân 5V/GND)
 *             Cầu chia đo pin: Pin(+) ─ 220k ─ GPIO3 ─ 220k ─ GND
 *             Cảm biến rung MPU6050: SDA GPIO4, SCL GPIO5, INT GPIO0
 * Board     : ESP32C3 Dev Module, Arduino-ESP32 core >= 3.0, USB CDC On Boot: Enabled
 * Thư viện  : NimBLE-Arduino >= 2.1 (h2zero)
 *
 * Hoạt động:
 *  - Bật máy, hoặc bị rung khi đang ngủ: xe thức dậy và bật Bluetooth.
 *  - Không có điện thoại kết nối trong SLEEP_AFTER_MS: xe tự ngủ lại.
 *  - Có điện thoại kết nối: xe hoạt động đến khi nhận lệnh SLEEP.
 *  - Khi đang khóa (ARM), mỗi lần rung được đếm và cờ 'T' được gửi kèm.
 *
 * Gói trạng thái (notify, 1 giây/lần): "<pin %>,<pin mV>,<rssi>,<cờ>,<số lần rung>"
 *   vd "87,4050,-62,-,0"   pin % = -1 khi không lắp cầu chia
 *   cờ: '-' bình thường, 'L' pin yếu, 'S' pin cạn, 'T' có rung khi đang khóa
 * Lệnh (write): "FIND", "PIN:654321", "UNPAIR", "ARM", "DISARM", "SLEEP"
 */
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "driver/rtc_io.h"

// ---------- Cấu hình ----------
constexpr int PIN_LED = 8;   // LED onboard, sáng ở mức LOW
constexpr int PIN_VBAT = 3;  // giữa cầu chia 220k / 220k
constexpr float VBAT_RATIO = 2.0f;

// MPU6050
constexpr int PIN_SDA = 4;
constexpr int PIN_SCL = 5;
constexpr gpio_num_t PIN_MPU_INT = GPIO_NUM_0;  // chân ngắt, dùng để đánh thức
constexpr uint8_t MPU_ADDR = 0x68;
constexpr uint8_t MOT_THRESHOLD = 20;   // đơn vị 2 mg -> khoảng 40 mg, chỉnh khi thử
constexpr uint8_t MOT_DURATION = 1;     // ms
constexpr uint8_t LP_WAKE_5HZ = 1;      // LP_WAKE_CTRL: 0=1.25Hz, 1=5Hz, 2=20Hz, 3=40Hz

// Thời gian chờ điện thoại kết nối sau khi thức dậy (hoặc sau khi mất kết nối)
constexpr uint32_t SLEEP_AFTER_MS = 60000;

// Không đặt mã cố định trong mã nguồn. Lần chạy đầu tiên sẽ tự sinh mã
// ngẫu nhiên, lưu vào bộ nhớ của xe và in ra cổng Serial.

constexpr uint16_t VBAT_NONE_MV = 2500;   // thấp hơn = không lắp cầu chia
constexpr uint16_t VBAT_LOW_MV = 3600;    // ~20% -> báo pin yếu
constexpr uint16_t VBAT_EMPTY_MV = 3300;  // -> ngủ sâu để bảo vệ pin

constexpr char DEVICE_NAME[] = "XeDap";

#define SVC_UUID "6e7a1001-3c2b-4b8e-9a1f-58c5e1a0b001"
#define ST_UUID  "6e7a1002-3c2b-4b8e-9a1f-58c5e1a0b001"  // trạng thái (notify)
#define CMD_UUID "6e7a1003-3c2b-4b8e-9a1f-58c5e1a0b001"  // lệnh (write)

// ---------- Biến toàn cục ----------
Preferences prefs;
uint32_t pairPin = 0;

NimBLEServer* server = nullptr;
NimBLECharacteristic* chStatus = nullptr;
volatile bool bleAuthed = false;
volatile bool evAuthed = false;
volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
volatile uint32_t connectedAt = 0;

struct Cmd { char s[24]; };
QueueHandle_t cmdQueue;

uint16_t vbatMv = 0;
int8_t rssiNow = 0;
uint8_t emptyCount = 0;
uint32_t findUntil = 0;

bool armed = true;          // đang khóa: rung mới được tính
bool theftFlag = false;     // đã có rung khi đang khóa
uint16_t theftCount = 0;    // số lần rung trong phiên thức này
uint32_t lastLinkMs = 0;    // lần cuối còn điện thoại kết nối
bool sleepRequested = false;

// =====================================================================
//  MPU6050 (cảm biến rung)
// =====================================================================
void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t mpuRead(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

// Cấu hình phát hiện chuyển động ở chế độ chu kỳ (tiết kiệm pin)
bool initMotionSensor() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  mpuWrite(0x6B, 0x00);            // đánh thức chip, dùng xung nội
  delay(10);
  if (mpuRead(0x75) != 0x68) return false;  // WHO_AM_I: không thấy MPU6050

  mpuWrite(0x1A, 0x01);            // CONFIG: DLPF
  mpuWrite(0x1C, 0x01);            // ACCEL_CONFIG: ±2g, lọc trọng lực (HPF 5 Hz)
  mpuWrite(0x1F, MOT_THRESHOLD);   // MOT_THR
  mpuWrite(0x20, MOT_DURATION);    // MOT_DUR
  mpuWrite(0x69, 0x15);            // MOT_DETECT_CTRL
  mpuWrite(0x37, 0x20);            // INT_PIN_CFG: giữ mức INT đến khi đọc INT_STATUS
  mpuWrite(0x38, 0x40);            // INT_ENABLE: báo chuyển động
  mpuWrite(0x6C, (LP_WAKE_5HZ << 6) | 0x07);  // tắt gyro, giữ accel
  mpuWrite(0x6B, 0x20 | 0x08);     // CYCLE bật, TEMP_DIS
  mpuRead(0x3A);                   // xoá cờ cũ
  return true;
}

// Nếu có rung, đọc INT_STATUS để xoá cờ, rồi đếm nếu đang khóa
void checkVibration() {
  if (digitalRead(PIN_MPU_INT) != HIGH) return;
  uint8_t st = mpuRead(0x3A);
  if ((st & 0x40) && armed) {
    if (theftCount < 65535) theftCount++;
    theftFlag = true;
    Serial.printf("Rung khi dang khoa, lan %u\n", theftCount);
  }
}

// =====================================================================
//  Pin
// =====================================================================
uint16_t readVbatMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_VBAT);  // trở kháng cao -> lấy trung bình
  return (uint16_t)(sum / 16 * VBAT_RATIO);
}

int batteryPercent(uint16_t mv) {
  if (mv < VBAT_NONE_MV) return -1;
  static const uint16_t V[] = {3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
  static const uint8_t P[] = {0, 5, 10, 20, 35, 50, 65, 80, 90, 100};
  if (mv <= V[0]) return 0;
  for (int i = 1; i < 10; i++) {
    if (mv <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (mv - V[i - 1]) / (V[i] - V[i - 1]);
  }
  return 100;
}

// Ngủ sâu không có nguồn đánh thức: chỉ chạy lại khi tháo/lắp pin
void deepSleepForever() {
  Serial.println("Pin can -> ngu sau. Thay pin da sac de chay lai.");
  Serial.flush();
  digitalWrite(PIN_LED, HIGH);
  esp_deep_sleep_start();
}

// Ngủ sâu, rung cảm biến sẽ đánh thức
[[noreturn]] void enterDeepSleep() {
  Serial.println("Ngu sau. Rung xe de danh thuc.");
  Serial.flush();
  NimBLEDevice::getAdvertising()->stop();
  if (connHandle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(connHandle);
  delay(200);                      // chờ gói ngắt kết nối đi hết
  mpuRead(0x3A);                   // xoá cờ rung đang chờ, tránh đánh thức ngay
  digitalWrite(PIN_LED, HIGH);
  rtc_gpio_pullup_dis(PIN_MPU_INT);
  rtc_gpio_pulldown_en(PIN_MPU_INT);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_MPU_INT, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

// =====================================================================
//  BLE
// =====================================================================
void sendStatus(char flag) {
  if (!bleAuthed || !chStatus) return;
  char c = theftFlag ? 'T' : flag;
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%d,%u,%d,%c,%u", batteryPercent(vbatMv), vbatMv,
                   rssiNow, c, theftCount);
  chStatus->setValue((uint8_t*)buf, n);
  chStatus->notify();
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    connHandle = ci.getConnHandle();
    bleAuthed = false;
    connectedAt = millis();
    NimBLEDevice::startSecurity(ci.getConnHandle());  // yêu cầu ghép đôi ngay
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& ci, int reason) override {
    bleAuthed = false;
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    NimBLEDevice::startAdvertising();
  }
  uint32_t onPassKeyDisplay() override { return pairPin; }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    if (ci.isEncrypted() && ci.isAuthenticated()) {
      bleAuthed = true;
      evAuthed = true;
    } else {
      NimBLEDevice::getServer()->disconnect(ci.getConnHandle());
    }
  }
};

class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (!ci.isEncrypted()) return;
    NimBLEAttValue v = c->getValue();
    Cmd cmd{};
    memcpy(cmd.s, v.data(), min((size_t)v.length(), sizeof(cmd.s) - 1));
    xQueueSend(cmdQueue, &cmd, 0);  // xử lý trong loop()
  }
};

void setupBle() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(9);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding + MITM + secure connections
  NimBLEDevice::setSecurityPasskey(pairPin);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  NimBLEService* svc = server->createService(SVC_UUID);
  chStatus = svc->createCharacteristic(
      ST_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                   NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* chCmd = svc->createCharacteristic(
      CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
                    NIMBLE_PROPERTY::WRITE_AUTHEN);
  chCmd->setCallbacks(new CmdCB());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(SVC_UUID);
  adv->setMinInterval(800);   // 500 ms
  adv->setMaxInterval(1600);  // 1 s - tiết kiệm pin
  adv->start();
}

void handleCommand(char* c) {
  for (char* p = c; *p; p++) *p = toupper(*p);
  Serial.printf("[CMD] %s\n", c);

  if (!strcmp(c, "FIND")) {
    findUntil = millis() + 10000;
  } else if (!strncmp(c, "PIN:", 4)) {
    long v = atol(c + 4);
    if (v >= 100000 && v <= 999999) {
      pairPin = v;
      prefs.begin("xedap", false);
      prefs.putUInt("pin", pairPin);
      prefs.end();
      NimBLEDevice::setSecurityPasskey(pairPin);
    }
  } else if (!strcmp(c, "UNPAIR")) {
    // Xoá mọi thiết bị đã ghép, điện thoại hiện tại cũng phải ghép lại
    NimBLEDevice::deleteAllBonds();
    if (connHandle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(connHandle);
  } else if (!strcmp(c, "ARM")) {
    armed = true;
  } else if (!strcmp(c, "DISARM")) {
    // Đang đi xe: không tính rung, xoá cờ báo động cũ
    armed = false;
    theftFlag = false;
    theftCount = 0;
  } else if (!strcmp(c, "SLEEP")) {
    sleepRequested = true;
  }
}

// =====================================================================
//  Tác vụ 1 giây/lần
// =====================================================================
void slowTasks() {
  uint32_t now = millis();

  vbatMv = readVbatMv();
  bool wired = vbatMv >= VBAT_NONE_MV;
  char flag = (wired && vbatMv < VBAT_LOW_MV) ? 'L' : '-';

  // Pin dưới 3.3V liên tục 10 giây -> báo điện thoại rồi ngủ sâu
  if (wired && vbatMv < VBAT_EMPTY_MV) {
    if (++emptyCount >= 10) {
      sendStatus('S');
      delay(1000);
      deepSleepForever();
    }
  } else {
    emptyCount = 0;
  }

  if (bleAuthed && connHandle != BLE_HS_CONN_HANDLE_NONE) {
    int8_t r;
    if (ble_gap_conn_rssi(connHandle, &r) == 0) rssiNow = r;
  } else {
    rssiNow = 0;
  }

  // Ngắt kết nối lạ không ghép đôi sau 60 giây (đủ thời gian nhập PIN)
  if (!bleAuthed && connHandle != BLE_HS_CONN_HANDLE_NONE && now - connectedAt > 60000) {
    server->disconnect(connHandle);
  }

  sendStatus(flag);
}

void updateLed() {
  uint32_t t = millis();
  bool on;
  if ((int32_t)(findUntil - t) > 0) on = (t % 200) < 100;  // tìm xe: nháy nhanh
  else on = (t % 5000) < 20;                               // bình thường: chớp rất ngắn
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);  // giảm dòng tiêu thụ

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  analogReadResolution(12);

  // Không khởi động khi pin đã cạn, tránh xả pin quá mức
  vbatMv = readVbatMv();
  if (vbatMv >= VBAT_NONE_MV && vbatMv < VBAT_EMPTY_MV) deepSleepForever();

  bool byVibration = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;

  prefs.begin("xedap", false);
  pairPin = prefs.getUInt("pin", 0);
  if (pairPin < 100000 || pairPin > 999999) {  // chưa có mã hợp lệ: sinh mã mới
    pairPin = 100000 + esp_random() % 900000;
    prefs.putUInt("pin", pairPin);
  }
  prefs.end();

  pinMode(PIN_MPU_INT, INPUT_PULLDOWN);
  if (!initMotionSensor()) Serial.println("Khong tim thay MPU6050 - kiem tra day SDA/SCL");

  if (byVibration && armed) {  // thức dậy vì rung khi đang khóa
    theftFlag = true;
    theftCount = 1;
    Serial.println("Thuc day do rung");
  }

  cmdQueue = xQueueCreate(4, sizeof(Cmd));
  setupBle();
  lastLinkMs = millis();

  Serial.printf("XeDap san sang. Pin %u mV (%d%%), PIN ghep doi: %06lu\n", vbatMv,
                batteryPercent(vbatMv), (unsigned long)pairPin);
}

void loop() {
  static uint32_t tSlow = 0;

  Cmd cmd;
  while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) handleCommand(cmd.s);

  if (evAuthed) {
    evAuthed = false;
    Serial.println("Dien thoai da ket noi");
    // Khoảng kết nối 200-400 ms, mất tín hiệu 4 s thì coi là rớt kết nối
    server->updateConnParams(connHandle, 160, 320, 0, 400);
    sendStatus('-');
  }

  checkVibration();

  if (millis() - tSlow >= 1000) {
    tSlow = millis();
    slowTasks();
  }

  // Còn điện thoại kết nối thì giữ thức. Không có điện thoại đủ lâu thì ngủ lại.
  if (connHandle != BLE_HS_CONN_HANDLE_NONE) lastLinkMs = millis();
  if (sleepRequested || millis() - lastLinkMs > SLEEP_AFTER_MS) enterDeepSleep();

  updateLed();
  delay(10);
}

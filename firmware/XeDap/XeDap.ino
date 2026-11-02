/*
 * XeDap Guard - Báo mất xe đạp qua Bluetooth LE (ESP32-C3)
 *
 * CHƯA KIỂM THỬ TRÊN XE THẬT. Ngưỡng rung và thời gian cần chỉnh khi thử.
 *
 * Phần cứng : ESP32-C3 SuperMini
 *             Pin: 3 viên 18650 song song, 3,7V, nối chân 5V và GND. Không mạch bảo vệ.
 *             Cầu chia đo pin: Pin(+) ─ 220k ─ GPIO3 ─ 220k ─ GND
 *             Cảm biến rung MPU6050: SDA GPIO4, SCL GPIO5, INT GPIO0
 * Board     : ESP32C3 Dev Module, Arduino-ESP32 core >= 3.0, USB CDC On Boot: Enabled
 *             Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA)"
 * Thư viện  : NimBLE-Arduino >= 2.1 (h2zero)
 *
 * Hoạt động:
 *  - Xe ngủ, bị rung: thức dậy, bật Bluetooth, chờ điện thoại.
 *  - Bị đánh thức LOCK_WAKES lần mà không có điện thoại: xe KHÓA.
 *  - Khi khóa: mỗi lần rung chỉ thức UNLOCK_WINDOW_MS để đếm cú rung.
 *    Đủ UNLOCK_TAPS cú rung trong khoảng đó thì mở khóa và giữ thức.
 *  - Không có điện thoại kết nối trong thời gian chờ: xe ngủ lại.
 *  - Có điện thoại kết nối: xe giữ thức đến khi nhận lệnh SLEEP.
 *  - Canh xe (ARM): khi đang thức, mỗi lần rung được đếm và báo cờ T.
 *  - Cập nhật firmware qua Bluetooth (OTA_CTRL, OTA_DATA). Lần đầu phải nạp qua USB.
 *
 * Gói trạng thái (notify, 1 giây/lần): "<pin %>,<pin mV>,<rssi>,<cờ>,<số lần rung>"
 *   vd "87,4050,-62,-,0"   pin % = -1 khi không lắp cầu chia
 *   cờ: '-' bình thường, 'L' pin yếu, 'S' pin cạn, 'T' có rung khi đang canh
 * Lệnh (write, đã mã hóa): "FIND", "PIN:654321", "UNPAIR", "ARM", "DISARM", "SLEEP"
 * OTA_CTRL (write/notify): "START:<size>:<crc hex>", "END", "ABORT"; phản hồi "OK" hoặc "ERR:..."
 * OTA_DATA (write): từng đoạn dữ liệu firmware, theo thứ tự
 */
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Update.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "driver/rtc_io.h"

// ---------- Cấu hình (chỉnh tại đây) ----------
constexpr int PIN_LED = 8;   // LED onboard, sáng ở mức LOW
constexpr int PIN_VBAT = 3;  // giữa cầu chia 220k / 220k
constexpr float VBAT_RATIO = 2.0f;

// MPU6050
constexpr int PIN_SDA = 4;
constexpr int PIN_SCL = 5;
constexpr gpio_num_t PIN_MPU_INT = GPIO_NUM_0;
constexpr uint8_t MPU_ADDR = 0x68;
constexpr uint8_t MOT_THRESHOLD = 20;   // đơn vị 2 mg, chỉnh khi thử
constexpr uint8_t MOT_DURATION = 1;     // ms, chỉnh khi thử
constexpr uint8_t LP_WAKE_5HZ = 1;      // LP_WAKE_CTRL: 0=1.25Hz, 1=5Hz, 2=20Hz, 3=40Hz

// Thời gian chờ điện thoại (ms)
constexpr uint32_t SLEEP_AFTER_MS = 60000;          // khi mới đánh thức
constexpr uint32_t SLEEP_AFTER_REPEAT_MS = 10000;   // từ lần đánh thức thứ 2 trở đi

// Khóa và mở khóa bằng mẫu rung
constexpr uint8_t LOCK_WAKES = 3;            // số lần đánh thức không có điện thoại thì khóa
constexpr uint8_t UNLOCK_TAPS = 3;           // số cú rung để mở khóa
constexpr uint32_t UNLOCK_WINDOW_MS = 5000;  // khoảng thời gian đếm cú rung
constexpr uint32_t TAP_DEBOUNCE_MS = 150;    // bỏ qua rung dư của cùng một cú

constexpr uint16_t VBAT_NONE_MV = 2500;   // thấp hơn = không lắp cầu chia
constexpr uint16_t VBAT_LOW_MV = 3600;    // ~20% -> báo pin yếu
constexpr uint16_t VBAT_EMPTY_MV = 3500;  // dưới mức này liên tục 10 giây -> ngủ sâu

constexpr char DEVICE_NAME[] = "XeDap";
constexpr uint32_t OTA_MAX_SIZE = 0x1F0000;  // khoảng 1,9 MB, theo bảng phân vùng OTA

#define SVC_UUID      "6e7a1001-3c2b-4b8e-9a1f-58c5e1a0b001"
#define ST_UUID       "6e7a1002-3c2b-4b8e-9a1f-58c5e1a0b001"  // trạng thái (notify)
#define CMD_UUID      "6e7a1003-3c2b-4b8e-9a1f-58c5e1a0b001"  // lệnh (write)
#define OTA_CTRL_UUID "6e7a1004-3c2b-4b8e-9a1f-58c5e1a0b001"  // điều khiển OTA (write, notify)
#define OTA_DATA_UUID "6e7a1005-3c2b-4b8e-9a1f-58c5e1a0b001"  // dữ liệu OTA (write)

// ---------- Biến toàn cục ----------
RTC_DATA_ATTR uint8_t rtcWakes = 0;  // số lần bị rung đánh thức khi không có điện thoại (giữ qua ngủ sâu)

Preferences prefs;
uint32_t pairPin = 0;
bool locked = false;

NimBLEServer* server = nullptr;
NimBLECharacteristic* chStatus = nullptr;
NimBLECharacteristic* chOtaCtrl = nullptr;
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
uint32_t lastLinkMs = 0;
bool sleepRequested = false;

bool armed = true;          // đang canh: rung được đếm
bool theftFlag = false;     // đã có rung khi đang canh
uint16_t theftCount = 0;    // số lần rung trong phiên thức này

// OTA
volatile bool otaActive = false;
volatile bool otaRestart = false;
uint32_t otaSize = 0;
uint32_t otaGot = 0;
uint32_t otaCrc = 0;
uint32_t otaCrcExpected = 0;

// =====================================================================
//  CRC32 (cùng thuật toán với giao diện điện thoại)
// =====================================================================
uint32_t crc32Update(uint32_t crc, const uint8_t* d, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *d++;
    for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
  }
  return ~crc;
}

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

bool initMotionSensor() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  mpuWrite(0x6B, 0x00);            // đánh thức chip
  delay(10);
  if (mpuRead(0x75) != 0x68) return false;  // WHO_AM_I: không thấy MPU6050

  mpuWrite(0x1A, 0x01);            // CONFIG: DLPF
  mpuWrite(0x1C, 0x01);            // ACCEL_CONFIG: ±2g, lọc trọng lực
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

// Khi đang thức và đang canh: đếm cú rung cho cờ T
void checkVibration() {
  if (digitalRead(PIN_MPU_INT) != HIGH) return;
  uint8_t st = mpuRead(0x3A);
  if ((st & 0x40) && armed) {
    if (theftCount < 65535) theftCount++;
    theftFlag = true;
    Serial.printf("Rung khi dang canh, lan %u\n", theftCount);
  }
}

// Khi đang khóa: thức trong UNLOCK_WINDOW_MS để đếm cú rung. Trả về true nếu đủ mẫu.
bool countUnlockTaps(uint8_t taps) {
  uint32_t start = millis();
  while (taps < UNLOCK_TAPS && millis() - start < UNLOCK_WINDOW_MS) {
    if (digitalRead(PIN_MPU_INT) == HIGH) {
      mpuRead(0x3A);  // xoá cờ
      taps++;
      Serial.printf("Cu rung %u\n", taps);
      delay(TAP_DEBOUNCE_MS);
    }
    delay(5);
  }
  return taps >= UNLOCK_TAPS;
}

// =====================================================================
//  Pin và ngủ
// =====================================================================
uint16_t readVbatMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_VBAT);
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

// Ngủ sâu, rung cảm biến đánh thức. Không tắt BLE ở đây.
void sleepNow() {
  Serial.println("Ngu sau. Rung xe de danh thuc.");
  Serial.flush();
  digitalWrite(PIN_LED, HIGH);
  mpuRead(0x3A);
  rtc_gpio_pullup_dis(PIN_MPU_INT);
  rtc_gpio_pulldown_en(PIN_MPU_INT);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_MPU_INT, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

// Ngắt BLE rồi ngủ
void enterDeepSleep() {
  NimBLEDevice::getAdvertising()->stop();
  if (connHandle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(connHandle);
  delay(200);
  sleepNow();
}

// Pin cạn: ngủ sâu không có nguồn đánh thức, chỉ chạy lại khi thay pin
void deepSleepForever() {
  Serial.println("Pin can -> ngu sau. Thay pin da sac de chay lai.");
  Serial.flush();
  digitalWrite(PIN_LED, HIGH);
  esp_deep_sleep_start();
}

// =====================================================================
//  BLE
// =====================================================================
void notifyOta(const char* s) {
  if (!chOtaCtrl || !bleAuthed) return;
  chOtaCtrl->setValue((uint8_t*)s, strlen(s));
  chOtaCtrl->notify();
}

void otaFail(const char* msg) {
  Serial.printf("OTA loi: %s\n", msg);
  if (otaActive) Update.abort();
  otaActive = false;
  char buf[48];
  snprintf(buf, sizeof(buf), "ERR:%s", msg);
  notifyOta(buf);
}

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

// Lệnh điều khiển OTA: START:<size>:<crc hex>, END, ABORT
class OtaCtrlCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (!ci.isEncrypted()) return;
    NimBLEAttValue v = c->getValue();
    char s[48] = {0};
    memcpy(s, v.data(), min((size_t)v.length(), sizeof(s) - 1));

    if (!strncmp(s, "START:", 6)) {
      char* p = s + 6;
      otaSize = strtoul(p, &p, 10);
      if (*p != ':') return otaFail("sai dinh dang");
      otaCrcExpected = strtoul(p + 1, nullptr, 16);
      if (otaSize == 0 || otaSize > OTA_MAX_SIZE) return otaFail("kich thuoc khong hop le");
      if (!Update.begin(otaSize)) return otaFail("khong mo duoc bo nho");
      otaGot = 0;
      otaCrc = 0;
      otaActive = true;
      Serial.printf("OTA bat dau, %lu byte\n", (unsigned long)otaSize);
      notifyOta("OK");
    } else if (!strcmp(s, "END")) {
      if (!otaActive) return otaFail("chua bat dau");
      if (otaGot != otaSize) return otaFail("thieu du lieu");
      if (otaCrc != otaCrcExpected) return otaFail("sai ma kiem tra");
      if (!Update.end(true)) return otaFail("ghi that bai");
      otaActive = false;
      Serial.println("OTA xong, khoi dong lai");
      notifyOta("OK");
      otaRestart = true;
    } else if (!strcmp(s, "ABORT")) {
      otaFail("da huy");
    }
  }
};

// Dữ liệu firmware
class OtaDataCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (!ci.isEncrypted() || !otaActive) return;
    NimBLEAttValue v = c->getValue();
    const uint8_t* d = v.data();
    size_t n = v.length();
    if (otaGot + n > otaSize) return otaFail("du lieu thua");
    if (Update.write((uint8_t*)d, n) != n) return otaFail("loi ghi bo nho");
    otaCrc = crc32Update(otaCrc, d, n);
    otaGot += n;
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

  chOtaCtrl = svc->createCharacteristic(
      OTA_CTRL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
                         NIMBLE_PROPERTY::WRITE_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
  chOtaCtrl->setCallbacks(new OtaCtrlCB());
  NimBLECharacteristic* chOtaData = svc->createCharacteristic(
      OTA_DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
                         NIMBLE_PROPERTY::WRITE_AUTHEN);
  chOtaData->setCallbacks(new OtaDataCB());
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
    NimBLEDevice::deleteAllBonds();
    if (connHandle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(connHandle);
  } else if (!strcmp(c, "ARM")) {
    armed = true;
  } else if (!strcmp(c, "DISARM")) {
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
  if ((int32_t)(findUntil - t) > 0) on = (t % 200) < 100;
  else on = (t % 5000) < 20;
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  analogReadResolution(12);

  bool byVibration = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;

  prefs.begin("xedap", false);
  pairPin = prefs.getUInt("pin", 0);
  if (pairPin < 100000 || pairPin > 999999) {
    pairPin = 100000 + esp_random() % 900000;
    prefs.putUInt("pin", pairPin);
  }
  locked = prefs.getBool("locked", false);

  // Không khởi động khi pin đã cạn
  vbatMv = readVbatMv();
  if (vbatMv >= VBAT_NONE_MV && vbatMv < VBAT_EMPTY_MV) {
    prefs.end();
    deepSleepForever();
  }

  pinMode(PIN_MPU_INT, INPUT_PULLDOWN);
  if (!initMotionSensor()) Serial.println("Khong tim thay MPU6050 - kiem tra day SDA/SCL");

  if (locked) {
    // Đang khóa: chỉ mở khi đủ mẫu rung, còn lại ngủ lại ngay
    if (byVibration && countUnlockTaps(1)) {
      locked = false;
      prefs.putBool("locked", false);
      rtcWakes = 0;
      Serial.println("Mo khoa bang mau rung");
    } else {
      prefs.end();
      sleepNow();
    }
  } else if (byVibration) {
    // Đánh thức khi không khóa
    if (rtcWakes < 255) rtcWakes++;
    if (rtcWakes >= LOCK_WAKES) {
      locked = true;
      prefs.putBool("locked", true);
      Serial.println("Khoa xe: nhan mau rung de mo");
      prefs.end();
      sleepNow();
    }
    theftFlag = armed;
    theftCount = armed ? 1 : 0;
    Serial.println("Thuc day do rung");
  }
  prefs.end();

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
    rtcWakes = 0;  // có điện thoại: đặt lại bộ đếm đánh thức
    Serial.println("Dien thoai da ket noi");
    server->updateConnParams(connHandle, 160, 320, 0, 400);
    sendStatus('-');
  }

  if (!otaActive) checkVibration();

  if (millis() - tSlow >= 1000) {
    tSlow = millis();
    slowTasks();
  }

  if (otaRestart) {
    delay(300);
    ESP.restart();
  }

  if (connHandle != BLE_HS_CONN_HANDLE_NONE) lastLinkMs = millis();
  uint32_t limit = (rtcWakes >= 2) ? SLEEP_AFTER_REPEAT_MS : SLEEP_AFTER_MS;
  if (!otaActive && (sleepRequested || millis() - lastLinkMs > limit)) enterDeepSleep();

  updateLed();
  delay(10);
}

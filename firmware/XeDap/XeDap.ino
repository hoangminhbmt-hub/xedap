/*
 * XeDap Guard v3.2 - Báo mất xe đạp qua Bluetooth LE (ESP32-C3) - bản tiết kiệm pin
 *
 * Phần cứng (mọi dây nằm ở MỘT hàng chân của ESP32-C3 SuperMini: 5V GND 3V3 4 3 2 1 0)
 *   4 viên pin sạc NiMH 1.2V : (+) → 5V, (−) → GND
 *   Cầu chia đo pin 1/3       : Pin(+) ─ 220k ─ 220k ─ GPIO0 ─ 220k ─ GND
 *   MPU-6050 (GY-521/SY-151)  : VCC→3V3, GND→GND, SCL→GPIO4, SDA→GPIO3, INT→GPIO1
 *                               XDA, XCL, AD0 bỏ trống. GPIO2 bỏ trống.
 *                               (lỡ đảo SDA/SCL hoặc AD0 lên 3V3 thì firmware vẫn tự nhận ra)
 *   Không đèn, không còi: mọi báo động nằm trên điện thoại (app/xedap.html).
 * Board    : ESP32C3 Dev Module, Arduino-ESP32 core >= 3.0, USB CDC On Boot: Enabled
 * Thư viện  : NimBLE-Arduino >= 2.1 (h2zero)
 *
 * Ngủ (Bluetooth tắt hẳn):
 *   - Lệnh SLEEP từ app, hoặc tự ngủ sau 3 phút không ai kết nối và không canh.
 *   - Xe đứng yên lúc đi ngủ -> lắc xe ~2 giây để đánh thức.
 *   - Xe đang rung lúc đi ngủ, hoặc vừa bị lắc dậy mà không ai kết nối (đang đạp xe)
 *     -> KHÔNG nhận lắc; cứ 20 giây tự dậy ~0.5 giây kiểm tra (không bật Bluetooth),
 *     xe đứng yên mới bật lại chế độ lắc. Dựng xe xong chờ tối đa ~20 giây là lắc dậy được.
 *
 * Gói trạng thái (notify, 1 giây/lần): "<pin %>,<pin mV>,<rssi>,<cờ>,<rung>,<canh>"
 *   vd "87,5400,-62,-,12,A"   pin % = -1 khi không lắp cầu chia
 *   cờ: '-' bình thường, 'L' pin yếu, 'S' pin cạn -> ngủ hẳn, 'Z' sắp ngủ,
 *       'N' không thấy cảm biến rung -> không ngủ được
 *   rung: độ rung mạnh nhất trong giây vừa qua, đơn vị 10 mg (0-99), 0 = xe đứng yên
 *   canh: 'D' không canh, 'A' đang canh
 * Lệnh (write): "ARM", "DISARM", "SLEEP", "VIB:5", "PIN:654321", "UNPAIR"
 */
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>

// ---------- Chân (cùng một hàng) ----------
constexpr int PIN_VBAT = 0;  // giữa cầu chia (220k+220k) / 220k  (ADC1)
constexpr int PIN_INT = 1;   // MPU-6050 INT - chân đánh thức khi ngủ sâu (GPIO0-5)
constexpr int PIN_SDA = 3;   // MPU-6050 SDA
constexpr int PIN_SCL = 4;   // MPU-6050 SCL
// GPIO2 bỏ trống (chân khởi động, tránh bị kéo xuống lúc bật nguồn)
constexpr int PIN_LED = 8;   // LED trên mạch: không dùng, để tắt hẳn

// ---------- Cấu hình ----------
constexpr float VBAT_RATIO = 3.0f;
constexpr uint32_t DEFAULT_PIN = 123456;  // mã PIN ghép đôi Bluetooth (6 số)

constexpr uint16_t VBAT_NONE_MV = 3000;   // thấp hơn = không lắp cầu chia
constexpr uint16_t VBAT_LOW_MV = 4600;    // 1.15V/viên (~20%) -> báo pin yếu
constexpr uint16_t VBAT_EMPTY_MV = 4200;  // 1.05V/viên -> ngủ hẳn, tránh xả hỏng pin NiMH

constexpr uint32_t IDLE_SLEEP_MS = 3UL * 60 * 1000;  // không kết nối & không canh -> tự ngủ
constexpr uint32_t LOCK_CHECK_MS = 20UL * 1000;      // ngủ khoá lắc: 20 giây dậy kiểm tra 1 lần
constexpr uint32_t MOVE_RECENT_MS = 10000;           // rung trong 10 giây cuối = xe đang di chuyển
constexpr uint8_t MOVE_LEVEL = 5;                    // mức rung coi là "đang di chuyển" (50 mg)

constexpr float VIB_MG_PER_LEVEL = 10.0f;  // 1 mức rung = 10 mg
constexpr float WAKE_MG = 80.0f;           // lúc vừa thức: rung > 80 mg mới tính
constexpr int WAKE_BINS = 8;               // cần 8 khoảng 100 ms có rung trong 3 giây
constexpr float STILL_MG = 50.0f;          // kiểm tra đứng yên: rung > 50 mg là còn chạy
constexpr uint8_t MOT_THR_6050 = 20;       // ngưỡng đánh thức MPU-6050 (~40 mg, 2 mg/đơn vị)
constexpr uint8_t MOT_THR_6500 = 10;       // ngưỡng đánh thức MPU-6500 (~40 mg, 4 mg/đơn vị)

constexpr char DEVICE_NAME[] = "XeDap";

#define SVC_UUID "6e7a1001-3c2b-4b8e-9a1f-58c5e1a0b001"
#define ST_UUID  "6e7a1002-3c2b-4b8e-9a1f-58c5e1a0b001"  // trạng thái (notify)
#define CMD_UUID "6e7a1003-3c2b-4b8e-9a1f-58c5e1a0b001"  // lệnh (write)

// ---------- Biến toàn cục ----------
Preferences prefs;
uint32_t pairPin = DEFAULT_PIN;

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

// Cảm biến
bool mpuOk = false;
bool mpu6500 = false;    // chip họ MPU-6500 (hay bị bán thay MPU-6050): thanh ghi ngủ khác
uint8_t mpuAddr = 0x68;
uint8_t mpuWho = 0;
bool i2cSwapped = false; // dây SDA/SCL bị đảo
float refX = 0, refY = 0, refZ = 0;  // gia tốc trung bình (mg), để tính độ rung
bool refInit = false;
float vibPeak = 0;                   // rung mạnh nhất trong giây hiện tại (mg)
uint8_t vibLevel = 0;
uint32_t lastMoveAt = 0;             // lần cuối rung >= MOVE_LEVEL

bool armed = false;         // đang canh (lưu NVS)
uint8_t vibTh = 5;          // ngưỡng rung do app đặt (chỉ lưu lại, app tự báo động)
bool wokeByShake = false;
bool everConnected = false;
uint32_t idleSince = 0;

// =====================================================================
//  MPU-6050 / MPU-6500 (I2C, không cần thư viện)
// =====================================================================
bool mpuWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

uint8_t mpuRead8(uint8_t reg) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(mpuAddr, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

// Gia tốc 3 trục, đơn vị mg (thang ±2g)
bool mpuAccel(float& x, float& y, float& z) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(mpuAddr, (uint8_t)6) != 6) return false;
  int16_t v[3];
  for (int i = 0; i < 3; i++) {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    v[i] = (int16_t)((hi << 8) | lo);
  }
  x = v[0] / 16.384f;
  y = v[1] / 16.384f;
  z = v[2] / 16.384f;
  return true;
}

// Dò cảm biến: thử 2 địa chỉ (AD0 thấp/cao) và cả trường hợp đảo SDA/SCL
bool mpuFind() {
  const int pins[2][2] = {{PIN_SDA, PIN_SCL}, {PIN_SCL, PIN_SDA}};
  for (int p = 0; p < 2; p++) {
    Wire.end();
    Wire.begin(pins[p][0], pins[p][1], 100000);  // 100 kHz: chắc ăn với dây dupont dài
    for (uint8_t a = 0x68; a <= 0x69; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        mpuAddr = a;
        i2cSwapped = (p == 1);
        return true;
      }
    }
  }
  Wire.end();
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  return false;
}

const char* mpuName() {
  if (!mpuOk) return "KHONG THAY";
  return mpu6500 ? "MPU-6500" : "MPU-6050";
}

// Chế độ thức: chỉ bật gia tốc kế, tắt con quay để đỡ tốn điện
bool mpuStartActive() {
  if (!mpuWrite(0x6B, 0x80)) return false;  // reset
  delay(100);
  mpuWho = mpuRead8(0x75);  // WHO_AM_I: 0x68 = MPU-6050, 0x70/0x71/0x73... = họ MPU-6500
  mpu6500 = (mpuWho >= 0x70 && mpuWho <= 0x7F);
  mpuWrite(0x6B, mpu6500 ? 0x01 : 0x08);  // thức (6050: tắt cảm biến nhiệt)
  mpuWrite(0x6C, 0x07);  // con quay 3 trục: tắt
  mpuWrite(0x1A, 0x03);  // lọc 44 Hz
  if (mpu6500) mpuWrite(0x1D, 0x03);  // 6500: lọc riêng cho gia tốc 44 Hz
  mpuWrite(0x19, 19);    // 50 mẫu/giây
  mpuWrite(0x1C, 0x00);  // ±2g
  mpuWrite(0x38, 0x00);  // không ngắt khi đang thức
  mpuWrite(0x37, 0x20);  // INT mức cao, đẩy kéo, giữ mức tới khi đọc INT_STATUS
  delay(20);
  return true;
}

// Ngủ chờ lắc: đo vài lần/giây, rung quá ngưỡng -> chân INT lên cao đánh thức ESP32
void mpuStartMotionWake() {
  mpuWrite(0x38, 0x00);
  mpuWrite(0x37, 0x20);  // INT mức cao, giữ mức
  if (mpu6500) {
    mpuWrite(0x6B, 0x01);
    mpuWrite(0x6C, 0x07);  // gia tốc bật, con quay tắt
    mpuWrite(0x1D, 0x09);  // lọc gia tốc 184 Hz
    mpuWrite(0x38, 0x40);  // bật ngắt "wake on motion"
    mpuWrite(0x69, 0xC0);  // so mẫu hiện tại với mẫu trước
    mpuWrite(0x1F, MOT_THR_6500);
    mpuWrite(0x1E, 4);     // đo ~4 lần/giây
    mpuWrite(0x6B, 0x21);  // chế độ chu kỳ tiết kiệm
  } else {
    mpuWrite(0x6B, 0x08);
    mpuWrite(0x6C, 0x07);
    mpuWrite(0x1C, 0x00);  // bộ lọc thông cao: reset
    mpuWrite(0x1F, MOT_THR_6050);
    mpuWrite(0x20, 1);     // chỉ cần 1 mẫu vượt ngưỡng
    mpuWrite(0x38, 0x40);  // bật ngắt phát hiện chuyển động
    delay(50);
    mpuWrite(0x1C, 0x07);  // giữ vị trí hiện tại làm mốc so sánh
    mpuWrite(0x6C, 0x47);  // đo 5 lần/giây, con quay tắt
    mpuWrite(0x6B, 0x28);  // chế độ chu kỳ tiết kiệm (~0.02 mA)
  }
  delay(300);
  mpuRead8(0x3A);  // xoá ngắt cũ
}

// Gọi mỗi 20 ms khi đang thức
void sampleVibration() {
  static uint8_t fails = 0;
  float x, y, z;
  if (!mpuOk) return;
  if (!mpuAccel(x, y, z)) {
    // Đọc lỗi liên tục ~1 giây (50 lần) -> coi như mất cảm biến (lỏng dây), slowTasks sẽ dò lại
    if (++fails >= 50) {
      fails = 0;
      mpuOk = false;
      Serial.println("Mat ket noi cam bien rung (long day?) -> se tu do lai moi 5 giay");
    }
    return;
  }
  fails = 0;
  if (!refInit) {
    refX = x; refY = y; refZ = z;
    refInit = true;
  }
  float dx = x - refX, dy = y - refY, dz = z - refZ;
  float d = sqrtf(dx * dx + dy * dy + dz * dz);
  if (d > vibPeak) vibPeak = d;
  // Mốc trôi chậm (~1 giây) theo tư thế xe: nghiêng hẳn vẫn báo rung vài giây
  refX += dx * 0.02f;
  refY += dy * 0.02f;
  refZ += dz * 0.02f;
}

// Đếm số khoảng 100 ms có rung > mg trong 'bins' khoảng; dừng sớm khi đạt 'need'
int countShakeBins(int bins, float mg, int need) {
  int n = 0;
  for (int b = 0; b < bins; b++) {
    float peak = 0;
    for (int i = 0; i < 5; i++) {
      vibPeak = 0;
      sampleVibration();
      if (vibPeak > peak) peak = vibPeak;
      delay(20);
    }
    if (peak > mg && ++n >= need) break;
  }
  return n;
}

// =====================================================================
//  Pin
// =====================================================================
uint16_t readVbatMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) sum += analogReadMilliVolts(PIN_VBAT);  // trở kháng cao -> lấy trung bình
  return (uint16_t)(sum / 32 * VBAT_RATIO);
}

int batteryPercent(uint16_t mv) {
  if (mv < VBAT_NONE_MV) return -1;
  // Đường xả NiMH (4 viên): 1.05V/viên = 0% ... 1.40V/viên = 100%
  static const uint16_t V[] = {4200, 4400, 4560, 4680, 4800, 4880, 4960, 5080, 5240, 5600};
  static const uint8_t P[] = {0, 5, 10, 20, 35, 50, 65, 80, 90, 100};
  if (mv <= V[0]) return 0;
  for (int i = 1; i < 10; i++) {
    if (mv <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (mv - V[i - 1]) / (V[i] - V[i - 1]);
  }
  return 100;
}

// =====================================================================
//  BLE
// =====================================================================
void sendStatus(char flag) {
  if (!bleAuthed || !chStatus) return;
  char buf[24];
  int n = snprintf(buf, sizeof(buf), "%d,%u,%d,%c,%u,%c", batteryPercent(vbatMv), vbatMv, rssiNow,
                   flag, vibLevel, armed ? 'A' : 'D');
  chStatus->setValue((uint8_t*)buf, n);
  chStatus->notify();
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    connHandle = ci.getConnHandle();
    bleAuthed = false;
    connectedAt = millis();
    // Không tự gọi startSecurity: điện thoại sẽ tự hỏi PIN khi đọc đặc tính đã mã hoá.
    // Gọi ở đây làm Android báo "GATT operation already in progress".
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

// =====================================================================
//  Ngủ
// =====================================================================
void saveArmed(bool a) {
  armed = a;
  prefs.begin("xedap", false);
  prefs.putBool("armed", armed);
  prefs.end();
}

// Pin cạn: tắt hẳn, chỉ chạy lại khi thay pin đã sạc
void deepSleepForever() {
  Serial.println("Pin can -> ngu han. Thay pin da sac de chay lai.");
  Serial.flush();
  if (mpuOk) mpuWrite(0x6B, 0x40);  // MPU ngủ hẳn
  esp_deep_sleep_start();
}

// lockLac = true: không nhận lắc (xe đang chạy), 20 giây dậy kiểm tra 1 lần
void goSleep(const char* why, bool lockLac) {
  Serial.printf("Ngu (%s). %s\n", why,
                lockLac ? "Xe dang chay -> 20 giay kiem tra 1 lan, dung yen moi lac duoc."
                        : "Lac xe ~2 giay de danh thuc.");
  if (bleAuthed) {
    sendStatus('Z');  // báo điện thoại: sắp ngủ, đừng báo động mất kết nối
    delay(500);
  }
  if (server && connHandle != BLE_HS_CONN_HANDLE_NONE) {
    server->disconnect(connHandle);
    delay(300);
  }
  if (server) NimBLEDevice::getAdvertising()->stop();
  if (armed) saveArmed(false);  // ngủ = không canh

  if (lockLac) {
    mpuWrite(0x6B, 0x40);  // MPU ngủ hẳn, ESP32 tự dậy theo giờ
    esp_sleep_enable_timer_wakeup((uint64_t)LOCK_CHECK_MS * 1000ULL);
  } else {
    mpuStartMotionWake();
    // INT đang ở mức cao (vừa có rung) thì xoá rồi chờ, tránh vừa ngủ đã thức
    for (int i = 0; i < 10 && digitalRead(PIN_INT); i++) {
      mpuRead8(0x3A);
      delay(200);
    }
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_INT, ESP_GPIO_WAKEUP_GPIO_HIGH);
  }
  Serial.flush();
  esp_deep_sleep_start();
}

// Tự quyết kiểu ngủ: xe vừa rung trong 10 giây cuối -> khoá lắc
void goSleepAuto(const char* why) {
  bool moving = lastMoveAt && millis() - lastMoveAt < MOVE_RECENT_MS;
  goSleep(why, moving);
}

// =====================================================================
//  Lệnh từ điện thoại
// =====================================================================
void handleCommand(char* c) {
  for (char* p = c; *p; p++) *p = toupper(*p);
  Serial.printf("[CMD] %s\n", c);

  if (!strcmp(c, "ARM")) {
    saveArmed(true);
    sendStatus('-');
  } else if (!strcmp(c, "DISARM")) {
    saveArmed(false);
    sendStatus('-');
  } else if (!strcmp(c, "SLEEP")) {
    // Không có cảm biến thì không có gì đánh thức được -> không cho ngủ, báo app
    if (mpuOk) {
      goSleepAuto("lenh tu dien thoai");
    } else {
      Serial.println("Khong thay MPU-6050 -> bo qua lenh ngu. Kiem tra day VCC GND SCL SDA.");
      sendStatus('N');
    }
  } else if (!strncmp(c, "VIB:", 4)) {
    vibTh = constrain(atoi(c + 4), 1, 30);
    prefs.begin("xedap", false);
    prefs.putUChar("vibTh", vibTh);
    prefs.end();
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
  }
  // TEST, SILENCE, FIND của bản có còi: bỏ qua
}

// =====================================================================
//  Tác vụ 1 giây/lần
// =====================================================================
void printStatusLine(uint32_t now, bool connected) {
  long left = (long)(IDLE_SLEEP_MS - (now - idleSince)) / 1000;
  Serial.printf("[TT] Pin %u mV (%d%%) | cam bien: %s (0x%02X, WHO=0x%02X%s) | canh: %s | "
                "ket noi: %s | rung: %u",
                vbatMv, batteryPercent(vbatMv), mpuName(), mpuAddr, mpuWho,
                i2cSwapped ? ", SDA/SCL bi dao" : "", armed ? "BAT" : "tat",
                connected ? (bleAuthed ? "co" : "chua ghep doi") : "khong", vibLevel);
  if (!mpuOk) Serial.print(" | KHONG NGU DUOC: kiem tra day MPU");
  else if (connected || armed) Serial.print(" | khong tu ngu");
  else Serial.printf(" | tu ngu sau %ld s", max(left, 0L));
  Serial.println();
}

void slowTasks() {
  uint32_t now = millis();

  vibLevel = (uint8_t)min(vibPeak / VIB_MG_PER_LEVEL, 99.0f);
  vibPeak = 0;
  if (vibLevel >= MOVE_LEVEL) lastMoveAt = now;
  if (vibLevel) Serial.printf("[RUNG] %u\n", vibLevel);

  // Chưa thấy cảm biến (dây lỏng lúc khởi động / vừa tuột) -> 5 giây dò lại 1 lần
  static uint32_t tRetry = 0;
  if (!mpuOk && now - tRetry >= 5000) {
    tRetry = now;
    mpuOk = mpuFind() && mpuStartActive();
    if (mpuOk) {
      refInit = false;  // lấy lại mốc tư thế, tránh báo rung giả
      Serial.printf("Da tim lai cam bien: %s (0x%02X, WHO=0x%02X%s)\n", mpuName(), mpuAddr, mpuWho,
                    i2cSwapped ? ", SDA/SCL bi dao" : "");
    }
  }

  vbatMv = readVbatMv();
  bool wired = vbatMv >= VBAT_NONE_MV;
  char flag = (wired && vbatMv < VBAT_LOW_MV) ? 'L' : (mpuOk ? '-' : 'N');

  // Pin dưới 4.2V liên tục 10 giây -> báo điện thoại rồi ngủ hẳn
  if (wired && vbatMv < VBAT_EMPTY_MV) {
    if (++emptyCount >= 10) {
      sendStatus('S');
      delay(1000);
      deepSleepForever();
    }
  } else {
    emptyCount = 0;
  }

  bool connected = connHandle != BLE_HS_CONN_HANDLE_NONE;
  if (bleAuthed && connected) {
    int8_t r;
    if (ble_gap_conn_rssi(connHandle, &r) == 0) rssiNow = r;
  } else {
    rssiNow = 0;
  }

  // Ngắt kết nối lạ không ghép đôi sau 60 giây (đủ thời gian nhập PIN)
  if (!bleAuthed && connected && now - connectedAt > 60000) {
    server->disconnect(connHandle);
  }

  static uint32_t tPrint = 0;
  if (now - tPrint >= 5000) {
    tPrint = now;
    printStatusLine(now, connected);
  }

  // Tự ngủ: không ai kết nối và không canh
  if (connected || armed) idleSince = now;
  if (mpuOk && now - idleSince > IDLE_SLEEP_MS) {
    // Vừa bị lắc dậy mà không ai dùng -> nhiều khả năng đang đạp xe -> khoá lắc
    if (wokeByShake && !everConnected) goSleep("lac day nhung khong ai ket noi", true);
    else goSleepAuto("khong ai ket noi");
  }

  sendStatus(flag);
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);  // giảm dòng tiêu thụ

  // LED xanh trên mạch (GPIO8, sáng ở mức LOW): giữ cứng mức HIGH cho tắt hẳn.
  // Để chân thả nổi (INPUT) thì nhiễu lúc phát Bluetooth làm LED nháy lấp loé.
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_INT, INPUT_PULLDOWN);  // chưa lắp MPU thì chân đứng ở mức thấp
  analogReadResolution(12);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool fromSleep = cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_TIMER;
  if (!fromSleep) {
    // Mới cắm điện: chờ tối đa 2 giây cho Serial Monitor kịp kết nối, khỏi mất chữ
    uint32_t t = millis();
    while (!Serial && millis() - t < 2000) delay(10);
  }

  mpuOk = mpuFind() && mpuStartActive();

  // Không khởi động khi pin đã cạn, tránh xả pin quá mức
  vbatMv = readVbatMv();
  if (vbatMv >= VBAT_NONE_MV && vbatMv < VBAT_EMPTY_MV) deepSleepForever();

  if (mpuOk && cause == ESP_SLEEP_WAKEUP_TIMER) {
    // Đang khoá lắc: kiểm tra nhanh 0.5 giây, có rung là xe còn chạy (chưa bật Bluetooth)
    if (countShakeBins(5, STILL_MG, 1) >= 1) goSleep("xe van dang chay", true);
    else goSleep("xe da dung yen", false);
  }
  if (mpuOk && cause == ESP_SLEEP_WAKEUP_GPIO) {
    if (countShakeBins(30, WAKE_MG, WAKE_BINS) < WAKE_BINS) goSleep("chi la va cham le", false);
    wokeByShake = true;
  }

  prefs.begin("xedap", true);
  pairPin = prefs.getUInt("pin", DEFAULT_PIN);
  armed = prefs.getBool("armed", false);  // khôi phục trạng thái canh sau khi mất điện
  vibTh = prefs.getUChar("vibTh", 5);
  prefs.end();

  cmdQueue = xQueueCreate(4, sizeof(Cmd));
  setupBle();
  idleSince = millis();

  Serial.printf("XeDap v3.2 san sang%s. Pin %u mV (%d%%), cam bien: %s, PIN ghep doi: %06lu\n",
                wokeByShake ? " (vua lac day)" : "", vbatMv, batteryPercent(vbatMv), mpuName(),
                (unsigned long)pairPin);
  if (mpuOk) {
    Serial.printf("  dia chi I2C 0x%02X, WHO_AM_I 0x%02X%s\n", mpuAddr, mpuWho,
                  i2cSwapped ? " - SDA/SCL dang bi DAO (van chay duoc)" : "");
  } else {
    Serial.println("  KHONG THAY cam bien rung: kiem tra VCC->3V3, GND, SCL->IO4, SDA->IO3, moi han.");
  }
}

void loop() {
  static uint32_t tSlow = 0, tVib = 0;

  Cmd cmd;
  while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) handleCommand(cmd.s);

  if (evAuthed) {
    evAuthed = false;
    everConnected = true;
    Serial.println("Dien thoai da ket noi");
    // Khoảng kết nối 200-400 ms, mất tín hiệu 4 s thì coi là rớt kết nối
    server->updateConnParams(connHandle, 160, 320, 0, 400);
    sendStatus(mpuOk ? '-' : 'N');
  }

  if (millis() - tVib >= 20) {
    tVib = millis();
    sampleVibration();
  }

  if (millis() - tSlow >= 1000) {
    tSlow = millis();
    slowTasks();
  }

  delay(5);
}

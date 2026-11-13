/*
 * XeGuard - Hệ thống cảnh báo chống trộm xe qua Bluetooth LE cho ESP32-C3
 *
 * Board : ESP32C3 Dev Module (ESP32-C3 SuperMini), Arduino-ESP32 core >= 3.0
 *         Tools > USB CDC On Boot: Enabled (để xem Serial qua cổng USB)
 * Thư viện: NimBLE-Arduino >= 2.1 (h2zero)
 *
 * Tính năng:
 *  - Phát hiện rung/di chuyển (MPU6050) theo 2 cấp: cảnh báo nhẹ -> báo động
 *  - Phát hiện xe bị nghiêng/dựng chân chống/nhấc lên xe tải (so với tư thế lúc khoá)
 *  - Dự phòng cảm biến rung SW-420 nếu không có MPU6050
 *  - BLE có ghép đôi bằng mã PIN (passkey) + mã hoá, chỉ điện thoại đã ghép mới điều khiển được
 *  - Tự bật canh xe khi điện thoại rời xa / mất kết nối (chỉ khi xe đứng yên)
 *  - Tuỳ chọn tự tắt canh khi chủ xe quay lại (smart key)
 *  - Theo dõi điện áp ắc quy: phát hiện cắt nguồn, nổ máy trái phép, ắc quy yếu
 *  - Relay khoá máy (tuỳ chọn), nút ẩn để tắt khi mất điện thoại
 *  - Ghi nhớ trạng thái canh xe khi bị mất điện / khởi động lại
 */
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

// ---------- Chân kết nối (ESP32-C3 SuperMini) ----------
constexpr int PIN_SDA = 5;
constexpr int PIN_SCL = 6;
constexpr int PIN_BUZZER = 4;  // còi 12V qua transistor/MOSFET
constexpr int PIN_RELAY = 7;   // relay khoá máy (tuỳ chọn)
constexpr int PIN_LED = 8;     // LED onboard, sáng ở mức LOW
constexpr int PIN_BTN = 9;     // nút BOOT / nút ẩn: giữ 3s để bật/tắt canh xe
constexpr int PIN_VIB = 10;    // SW-420 (chỉ dùng khi không có MPU6050)
constexpr int PIN_VBAT = 3;    // đo ắc quy qua cầu chia 100k / 22k
constexpr float VBAT_RATIO = (100.0f + 22.0f) / 22.0f;

constexpr uint8_t MPU_ADDR = 0x68;
constexpr char DEVICE_NAME[] = "XeGuard";
constexpr char FW_VERSION[] = "1.0";

// ---------- BLE UUID ----------
#define SVC_UUID "6e7a0001-3c2b-4b8e-9a1f-58c5e1a0b001"
#define ST_UUID  "6e7a0002-3c2b-4b8e-9a1f-58c5e1a0b001"  // trạng thái (notify)
#define CMD_UUID "6e7a0003-3c2b-4b8e-9a1f-58c5e1a0b001"  // lệnh (write)
#define CFG_UUID "6e7a0004-3c2b-4b8e-9a1f-58c5e1a0b001"  // cấu hình (read, JSON)

// ---------- Cấu hình lưu trong NVS ----------
struct Config {
  uint16_t sensMg = 60;      // ngưỡng rung (mg), càng nhỏ càng nhạy
  uint8_t tiltDeg = 12;      // góc nghiêng tối đa so với lúc khoá
  uint16_t autoArmSec = 30;  // tự canh sau N giây mất kết nối, 0 = tắt
  bool autoDisarm = false;   // tự tắt canh khi điện thoại chủ kết nối lại
  uint8_t sirenSec = 30;     // thời gian hú còi
  bool immo = false;         // dùng relay khoá máy khi đang canh
  int8_t rssiArm = 0;        // tự canh khi RSSI < ngưỡng (vd -90), 0 = tắt
  uint32_t pin = 123456;     // mã ghép đôi BLE
} cfg;

Preferences prefs;

enum State : uint8_t { DISARMED, ARMED, ALARM };
State state = DISARMED;

// Mã sự kiện gửi lên điện thoại
//  '-' không có   'W' rung nhẹ (cảnh báo)   'M' di chuyển -> báo động
//  'T' nghiêng    'P' mất nguồn ắc quy     'E' nổ máy trái phép
//  'B' ắc quy yếu 'L' tự canh do mất kết nối  'R' tự canh do ở xa
//  'O' khởi động lại khi đang canh
char pendingEvent = '-';

// ---------- Còi không chặn (non-blocking) ----------
struct Beeper {
  int toggles = 0;
  uint16_t onMs = 0, offMs = 0;
  uint32_t next = 0;
  bool on = false;

  void start(int count, uint16_t on_, uint16_t off_) {
    toggles = count * 2;
    onMs = on_;
    offMs = off_;
    on = false;
    next = millis();
  }
  void stop() {
    toggles = 0;
    on = false;
    digitalWrite(PIN_BUZZER, LOW);
  }
  bool busy() const { return toggles > 0; }
  void update() {
    if (!toggles) return;
    uint32_t now = millis();
    if ((int32_t)(now - next) < 0) return;
    on = !on;
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
    next = now + (on ? onMs : offMs);
    if (--toggles == 0) stop();
  }
} beeper;

// ---------- Cảm biến ----------
bool mpuOk = false;
float lp[3] = {0, 0, 1};   // gia tốc đã lọc (g)
float ref[3] = {0, 0, 1};  // tư thế lúc khoá
bool refValid = false;
float motionScore = 0;
float tiltNow = 0;
volatile uint32_t vibPulses = 0;

uint32_t settleUntil = 0;  // bỏ qua cảm biến tới thời điểm này
uint32_t stillSince = 0;   // xe đứng yên từ lúc nào
uint32_t warnAt = 0;
bool warnActive = false;
uint32_t alarmUntil = 0;

float vbat = 0;
float vbatMinArmed = 99;
bool powerAlarmed = false, engineAlarmed = false, lowBatSent = false;
uint32_t lowBatSince = 0;

// ---------- BLE ----------
NimBLEServer* server = nullptr;
NimBLECharacteristic* chStatus = nullptr;
NimBLECharacteristic* chCfg = nullptr;
volatile bool bleConnected = false;
volatile bool bleAuthed = false;
volatile bool evAuthed = false;
volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
volatile uint32_t connectedAt = 0, disconnectedAt = 0;
int8_t rssiNow = 0;
uint8_t farCount = 0;

struct Cmd { char s[32]; };
QueueHandle_t cmdQueue;

// =====================================================================
//  MPU6050 (đọc trực tiếp qua I2C, không cần thư viện)
// =====================================================================
bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool mpuInit() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (!mpuWrite(0x6B, 0x00)) return false;  // đánh thức
  mpuWrite(0x1A, 0x03);                     // lọc thông thấp ~44 Hz
  mpuWrite(0x1C, 0x00);                     // ±2g
  return true;
}

bool mpuRead(float a[3]) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6) return false;
  for (int i = 0; i < 3; i++) {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    a[i] = (int16_t)((hi << 8) | lo) / 16384.0f;
  }
  return true;
}

void IRAM_ATTR vibIsr() { vibPulses++; }

float angleDeg(const float a[3], const float b[3]) {
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  if (na < 0.1f || nb < 0.1f) return 0;
  return degrees(acosf(constrain(dot / (na * nb), -1.0f, 1.0f)));
}

// =====================================================================
//  Cấu hình
// =====================================================================
void updateCfgChar() {
  if (!chCfg) return;
  char buf[200];
  int n = snprintf(buf, sizeof(buf),
                   "{\"sens\":%u,\"tilt\":%u,\"autoArm\":%u,\"autoDisarm\":%d,"
                   "\"siren\":%u,\"immo\":%d,\"rssi\":%d,\"mpu\":%d,\"fw\":\"%s\"}",
                   cfg.sensMg, cfg.tiltDeg, cfg.autoArmSec, cfg.autoDisarm,
                   cfg.sirenSec, cfg.immo, cfg.rssiArm, mpuOk, FW_VERSION);
  chCfg->setValue((uint8_t*)buf, n);
}

void loadCfg() {
  prefs.begin("xeguard", true);
  cfg.sensMg = prefs.getUShort("sens", cfg.sensMg);
  cfg.tiltDeg = prefs.getUChar("tilt", cfg.tiltDeg);
  cfg.autoArmSec = prefs.getUShort("autoArm", cfg.autoArmSec);
  cfg.autoDisarm = prefs.getBool("autoDis", cfg.autoDisarm);
  cfg.sirenSec = prefs.getUChar("siren", cfg.sirenSec);
  cfg.immo = prefs.getBool("immo", cfg.immo);
  cfg.rssiArm = prefs.getChar("rssi", cfg.rssiArm);
  cfg.pin = prefs.getUInt("pin", cfg.pin);
  prefs.end();
}

void saveCfg() {
  prefs.begin("xeguard", false);
  prefs.putUShort("sens", cfg.sensMg);
  prefs.putUChar("tilt", cfg.tiltDeg);
  prefs.putUShort("autoArm", cfg.autoArmSec);
  prefs.putBool("autoDis", cfg.autoDisarm);
  prefs.putUChar("siren", cfg.sirenSec);
  prefs.putBool("immo", cfg.immo);
  prefs.putChar("rssi", cfg.rssiArm);
  prefs.putUInt("pin", cfg.pin);
  prefs.end();
  updateCfgChar();
}

void persistArmed(bool armed) {
  prefs.begin("xeguard", false);
  prefs.putBool("armed", armed);
  prefs.end();
}

// =====================================================================
//  Trạng thái
// =====================================================================
// Gói trạng thái gọn <= 20 byte để vừa MTU mặc định:
//   "<state>,<event>,<motion>,<tilt>,<vbat x10>,<rssi>"   vd: "A,W,7,3,126,-64"
void sendStatus() {
  if (!bleAuthed || !chStatus) return;
  static const char ST_CH[] = {'D', 'A', 'X'};
  char buf[24];
  int n = snprintf(buf, sizeof(buf), "%c,%c,%d,%d,%d,%d", ST_CH[state], pendingEvent,
                   (int)min(motionScore * 4, 99.0f), (int)min(tiltNow, 180.0f),
                   (int)(vbat * 10), rssiNow);
  chStatus->setValue((uint8_t*)buf, n);
  chStatus->notify();
  pendingEvent = '-';
}

void raiseEvent(char ev) {
  pendingEvent = ev;
  Serial.printf("[EVT] %c  state=%d\n", ev, state);
  sendStatus();
}

void applyRelay() {
  digitalWrite(PIN_RELAY, (cfg.immo && state != DISARMED) ? HIGH : LOW);
}

void arm(char ev = '-') {
  if (state != DISARMED) return;
  state = ARMED;
  settleUntil = millis() + 3000;  // chờ xe ổn định rồi chụp tư thế tham chiếu
  refValid = false;
  warnActive = false;
  motionScore = 0;
  vbatMinArmed = 99;
  powerAlarmed = engineAlarmed = false;
  persistArmed(true);
  applyRelay();
  beeper.start(1, 80, 80);
  raiseEvent(ev);
}

void disarm() {
  state = DISARMED;
  beeper.stop();
  beeper.start(2, 60, 90);
  persistArmed(false);
  applyRelay();
  disconnectedAt = millis();  // đếm lại thời gian tự canh
  raiseEvent('-');
}

void triggerAlarm(char ev) {
  if (state == DISARMED) return;
  state = ALARM;
  alarmUntil = millis() + cfg.sirenSec * 1000UL;
  beeper.start(cfg.sirenSec * 1000 / 500, 250, 250);
  raiseEvent(ev);
}

void endAlarm() {
  beeper.stop();
  state = ARMED;
  settleUntil = millis() + 2000;
  refValid = false;  // chụp lại tư thế để xe bị đổ không hú mãi
  warnActive = false;
  motionScore = 0;
  raiseEvent('-');
}

// =====================================================================
//  Xử lý cảm biến (50 Hz)
// =====================================================================
void sampleSensors() {
  if (mpuOk) {
    float a[3];
    if (!mpuRead(a)) return;
    float d = sqrtf(sq(a[0] - lp[0]) + sq(a[1] - lp[1]) + sq(a[2] - lp[2]));
    for (int i = 0; i < 3; i++) lp[i] += 0.1f * (a[i] - lp[i]);
    motionScore = motionScore * 0.96f + (d * 1000.0f > cfg.sensMg ? 1.0f : 0.0f);
    tiltNow = refValid ? angleDeg(ref, lp) : 0;
  } else {
    uint32_t p = vibPulses;
    vibPulses = 0;
    motionScore = motionScore * 0.96f + (p > 0 ? 1.0f : 0.0f);
  }
  if (motionScore > 1.5f) stillSince = millis();
}

void evaluateSensors() {
  uint32_t now = millis();
  if (state != ARMED) return;
  if ((int32_t)(now - settleUntil) < 0) {
    motionScore = 0;
    return;
  }
  if (mpuOk && !refValid) {
    memcpy(ref, lp, sizeof(ref));
    refValid = true;
    return;
  }
  if (mpuOk && tiltNow > cfg.tiltDeg) {
    triggerAlarm('T');
    return;
  }
  if (warnActive && now - warnAt > 15000) warnActive = false;
  if (motionScore > 4.0f) {
    motionScore = 0;
    if (warnActive) {
      triggerAlarm('M');  // rung lần 2 trong 15s -> báo động
    } else {
      warnActive = true;  // rung lần 1 -> chỉ kêu nhẹ, tránh báo động giả
      warnAt = now;
      beeper.start(2, 40, 60);
      settleUntil = now + 800;  // bỏ qua rung do chính còi gây ra
      raiseEvent('W');
    }
  }
}

// =====================================================================
//  BLE
// =====================================================================
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    connHandle = ci.getConnHandle();
    bleConnected = true;
    bleAuthed = false;
    connectedAt = millis();
    NimBLEDevice::startSecurity(ci.getConnHandle());  // yêu cầu ghép đôi ngay
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& ci, int reason) override {
    bleConnected = false;
    bleAuthed = false;
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    disconnectedAt = millis();
    NimBLEDevice::startAdvertising();
  }
  uint32_t onPassKeyDisplay() override { return cfg.pin; }
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
    size_t n = min((size_t)v.length(), sizeof(cmd.s) - 1);
    memcpy(cmd.s, v.data(), n);
    xQueueSend(cmdQueue, &cmd, 0);  // xử lý trong loop(), không chặn task BLE
  }
};

void setupBle() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(9);
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding + MITM + secure connections
  NimBLEDevice::setSecurityPasskey(cfg.pin);
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
  chCfg = svc->createCharacteristic(
      CFG_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                    NIMBLE_PROPERTY::READ_AUTHEN);
  updateCfgChar();
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(SVC_UUID);
  adv->setMinInterval(800);   // 500 ms - tiết kiệm điện
  adv->setMaxInterval(1600);  // 1 s
  adv->start();
}

// =====================================================================
//  Lệnh từ điện thoại
// =====================================================================
void handleCommand(char* c) {
  for (char* p = c; *p; p++) *p = toupper(*p);
  char* arg = strchr(c, ':');
  if (arg) *arg++ = 0;
  long v = arg ? atol(arg) : 0;
  Serial.printf("[CMD] %s %s\n", c, arg ? arg : "");

  if (!strcmp(c, "ARM")) arm('-');
  else if (!strcmp(c, "DISARM")) disarm();
  else if (!strcmp(c, "FIND")) { if (state == DISARMED) beeper.start(6, 120, 120); }
  else if (!strcmp(c, "SILENCE")) { if (state == ALARM) endAlarm(); }
  else if (!strcmp(c, "SENS")) { cfg.sensMg = constrain(v, 15, 1000); saveCfg(); }
  else if (!strcmp(c, "TILT")) { cfg.tiltDeg = constrain(v, 3, 90); saveCfg(); }
  else if (!strcmp(c, "AUTOARM")) { cfg.autoArmSec = constrain(v, 0, 3600); saveCfg(); }
  else if (!strcmp(c, "AUTODISARM")) { cfg.autoDisarm = v != 0; saveCfg(); }
  else if (!strcmp(c, "SIREN")) { cfg.sirenSec = constrain(v, 5, 180); saveCfg(); }
  else if (!strcmp(c, "IMMO")) { cfg.immo = v != 0; saveCfg(); applyRelay(); }
  else if (!strcmp(c, "RSSI")) { cfg.rssiArm = constrain(v, -110, 0); saveCfg(); }
  else if (!strcmp(c, "PIN")) {
    if (v >= 100000 && v <= 999999) {
      cfg.pin = v;
      saveCfg();
      NimBLEDevice::setSecurityPasskey(cfg.pin);
    }
  } else if (!strcmp(c, "UNPAIR")) {
    // Xoá mọi thiết bị đã ghép; điện thoại hiện tại cũng phải ghép lại
    NimBLEDevice::deleteAllBonds();
    if (connHandle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(connHandle);
    return;
  }
  sendStatus();
}

// =====================================================================
//  Tác vụ chậm (1 Hz)
// =====================================================================
void slowTasks() {
  uint32_t now = millis();

  // --- Điện áp ắc quy ---
  vbat = analogReadMilliVolts(PIN_VBAT) * VBAT_RATIO / 1000.0f;
  bool vbatWired = vbat > 3.0f;
  if (vbatWired && state != DISARMED) {
    vbatMinArmed = min(vbatMinArmed, vbat);
    if (vbat < 9.0f && !powerAlarmed) {  // bị cắt/tháo ắc quy (cần pin dự phòng)
      powerAlarmed = true;
      triggerAlarm('P');
    }
    if (vbat - vbatMinArmed > 0.8f && !engineAlarmed) {  // điện áp tăng = máy phát đang sạc
      engineAlarmed = true;
      triggerAlarm('E');
    }
  }
  if (vbatWired && vbat < 11.8f) {
    if (!lowBatSince) lowBatSince = now;
    if (!lowBatSent && now - lowBatSince > 60000) {
      lowBatSent = true;
      raiseEvent('B');
    }
  } else {
    lowBatSince = 0;
    if (vbat > 12.2f) lowBatSent = false;
  }

  bool still = now - stillSince > 10000;

  // --- RSSI & tự canh khi điện thoại ở xa ---
  if (bleAuthed && connHandle != BLE_HS_CONN_HANDLE_NONE) {
    int8_t r;
    if (ble_gap_conn_rssi(connHandle, &r) == 0) rssiNow = r;
    if (cfg.rssiArm != 0 && state == DISARMED && rssiNow < cfg.rssiArm && still) {
      if (++farCount >= 10) arm('R');
    } else {
      farCount = 0;
    }
  } else {
    rssiNow = 0;
    farCount = 0;
  }

  // --- Tự canh khi mất kết nối (chỉ khi xe đứng yên, tránh khoá khi đang chạy) ---
  if (!bleConnected && state == DISARMED && cfg.autoArmSec && still &&
      now - disconnectedAt > cfg.autoArmSec * 1000UL) {
    arm('L');
  }

  // --- Ngắt kết nối lạ không ghép đôi sau 20s ---
  if (bleConnected && !bleAuthed && now - connectedAt > 20000 &&
      connHandle != BLE_HS_CONN_HANDLE_NONE) {
    server->disconnect(connHandle);
  }

  sendStatus();
}

// =====================================================================
//  LED & nút ẩn
// =====================================================================
void updateLed() {
  uint32_t t = millis();
  bool on;
  switch (state) {
    case DISARMED: on = (t % 3000) < 30; break;
    case ARMED:    on = (t % 2000) < 100; break;
    default:       on = (t % 200) < 100; break;
  }
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

void handleButton() {
  static uint32_t pressAt = 0;
  static bool handled = false;
  if (digitalRead(PIN_BTN) == LOW) {
    if (!pressAt) {
      pressAt = millis();
      handled = false;
    } else if (!handled && millis() - pressAt > 3000) {
      handled = true;
      if (state == DISARMED) arm('-');
      else disarm();
    }
  } else {
    pressAt = 0;
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);  // giảm dòng tiêu thụ

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_BTN, INPUT_PULLUP);
  analogReadResolution(12);

  cmdQueue = xQueueCreate(4, sizeof(Cmd));
  loadCfg();

  mpuOk = mpuInit();
  if (mpuOk) {
    mpuRead(lp);
    Serial.println("MPU6050 OK");
  } else {
    Serial.println("Khong thay MPU6050 -> dung cam bien rung SW-420");
    pinMode(PIN_VIB, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_VIB), vibIsr, RISING);
  }

  setupBle();
  disconnectedAt = millis();
  stillSince = millis();

  prefs.begin("xeguard", true);
  bool wasArmed = prefs.getBool("armed", false);
  prefs.end();
  if (wasArmed) arm('O');  // khôi phục sau mất điện

  Serial.printf("XeGuard %s san sang. PIN ghep doi: %06lu\n", FW_VERSION, (unsigned long)cfg.pin);
}

void loop() {
  static uint32_t tSample = 0, tSlow = 0;
  uint32_t now = millis();

  if (now - tSample >= 20) {
    tSample = now;
    sampleSensors();
    evaluateSensors();
  }

  Cmd cmd;
  while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) handleCommand(cmd.s);

  if (evAuthed) {
    evAuthed = false;
    Serial.println("Dien thoai chu xe da ket noi");
    if (cfg.autoDisarm && state != DISARMED) disarm();
    else sendStatus();
  }

  if (state == ALARM && (int32_t)(now - alarmUntil) >= 0) endAlarm();

  if (now - tSlow >= 1000) {
    tSlow = now;
    slowTasks();
  }

  beeper.update();
  updateLed();
  handleButton();
  delay(2);
}

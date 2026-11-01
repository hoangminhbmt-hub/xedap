/*
 * XeDap Guard - Báo mất xe đạp qua Bluetooth LE (ESP32-C3)
 *
 * Phần cứng : ESP32-C3 SuperMini + pin 18650 (nối chân 5V/GND)
 *             Cầu chia đo pin: Pin(+) ─ 220k ─ GPIO3 ─ 220k ─ GND
 * Board     : ESP32C3 Dev Module, Arduino-ESP32 core >= 3.0, USB CDC On Boot: Enabled
 * Thư viện  : NimBLE-Arduino >= 2.1 (h2zero)
 *
 * Thiết bị chỉ phát Bluetooth và gửi trạng thái mỗi giây. Việc báo động khi
 * mất kết nối / tín hiệu yếu do điện thoại đảm nhận (app/xedap.html).
 *
 * Gói trạng thái (notify, 1 giây/lần): "<pin %>,<pin mV>,<rssi>,<cờ>"
 *   vd "87,4050,-62,-"   pin % = -1 khi không lắp cầu chia
 *   cờ: '-' bình thường, 'L' pin yếu, 'S' pin cạn -> thiết bị sắp ngủ sâu
 * Lệnh (write): "FIND", "PIN:654321", "UNPAIR"
 */
#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

// ---------- Cấu hình ----------
constexpr int PIN_LED = 8;   // LED onboard, sáng ở mức LOW
constexpr int PIN_VBAT = 3;  // giữa cầu chia 220k / 220k
constexpr float VBAT_RATIO = 2.0f;
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

void deepSleepForever() {
  Serial.println("Pin can -> ngu sau. Thay pin da sac de chay lai.");
  Serial.flush();
  digitalWrite(PIN_LED, HIGH);
  esp_deep_sleep_start();  // không đặt nguồn đánh thức: chỉ chạy lại khi tháo/lắp pin
}

// =====================================================================
//  BLE
// =====================================================================
void sendStatus(char flag) {
  if (!bleAuthed || !chStatus) return;
  char buf[24];
  int n = snprintf(buf, sizeof(buf), "%d,%u,%d,%c", batteryPercent(vbatMv), vbatMv, rssiNow, flag);
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

  prefs.begin("xedap", false);
  pairPin = prefs.getUInt("pin", 0);
  if (pairPin < 100000 || pairPin > 999999) {  // chưa có mã hợp lệ: sinh mã mới
    pairPin = 100000 + esp_random() % 900000;
    prefs.putUInt("pin", pairPin);
  }
  prefs.end();

  cmdQueue = xQueueCreate(4, sizeof(Cmd));
  setupBle();

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

  if (millis() - tSlow >= 1000) {
    tSlow = millis();
    slowTasks();
  }

  updateLed();
  delay(10);
}

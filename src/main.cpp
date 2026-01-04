/*
  ESP32 IR Controller (Scoreboard RC5) — FAST UDP BINARY (SPEC COMPLIANT)
  Target: ESP32 DevKit v1 (ESP32)

  Spec: "ESP32-C3 IR Controller over Wi-Fi (UDP, Binary Protocol) v1.0"
  - UDP port: 4210
  - MAGIC: 0xA5
  - VERSION: 0x01
  - Command: [MAGIC][VER][CMD][ID u16 LE][LEN][PAYLOAD...]
  - ACK (7 bytes): [MAGIC][VER][0x7F][ID u16 LE][STATUS][CODE=0]
  - ACK is sent immediately after accept/enqueue, before IR execution
  - Dedup by last ID (resend last ACK, do not re-execute)
  - No Serial / JSON / HTTP / mDNS
  - Wi-Fi sleep disabled
  - Status LED: steady ON on Wi-Fi, burst blink on command/IR activity
  - RC5 toggle flips on EVERY IR press (including macro steps)

  OTA addition (service mode):
  - Enter OTA mode by UDP command CMD_OTA_MODE (0x70), LEN=0
  - OTA mode active for OTA_WINDOW_MS, then auto-exit
  - In OTA mode, only CMD_OTA_MODE is accepted (others are rejected) to avoid accidental actions
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <esp_wifi.h>

// ===== OTA (ArduinoOTA) =====
#include <ArduinoOTA.h>

// ======================
// Wi-Fi (single network)
// ======================
// static const char* WIFI_SSID = "Pestovo_Arena";
// static const char* WIFI_PASS = "99991111";
// static const char* WIFI_SSID = "WirelessNet";
// static const char* WIFI_PASS = "9269849402";
static const char* WIFI_SSID = "IoT";
static const char* WIFI_PASS = "9269849402";

// ======================
// Pins (from your original sketch)
// ======================
static const int IR_LED_PIN     = 26;
static const int STATUS_LED_PIN = 2;
static const int SIREN_PIN      = 25;
static const bool SIREN_ACTIVE_HIGH = true;

// ======================
// UDP
// ======================
static const uint16_t UDP_PORT = 4210;
WiFiUDP udp;

// ======================
// IR
// ======================
IRsend irsend(IR_LED_PIN);

// ======================
// Spec constants
// ======================
static const uint8_t MAGIC = 0xA5;
static const uint8_t VER   = 0x01;
static const uint8_t CMD_ACK = 0x7F;

// Commands (spec)
static const uint8_t CMD_MODE_SWITCH      = 0x40; // exit x3 + 500ms after
static const uint8_t CMD_RESET_SCOREBOARD = 0x41; // pause(9) + reset (8 x3)
static const uint8_t CMD_SIREN            = 0x60; // payload: count + (on16,off16)*count

// Vendor extension: OTA service mode
static const uint8_t CMD_OTA_MODE         = 0x70; // LEN=0

// Timing (spec)
static const uint16_t IR_GAP_MS_DEFAULT   = 100;
static const uint16_t IR_GAP_MS_EXIT3_END = 500;

// ======================
// OTA settings
// ======================
static const char* OTA_HOSTNAME = "scoreboard-esp32";
static const uint16_t OTA_PORT = 3232;                 // default ArduinoOTA port
static const uint32_t OTA_WINDOW_MS = 180000UL;        // 3 minutes

static bool otaEnabled = false;
static bool otaInitialized = false;
static unsigned long otaUntilMs = 0;

static inline bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

static inline void statusLedWrite(bool on) {
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

// ======================
// Status LED burst (same style as old firmware)
// ======================
static const uint16_t ACTIVITY_BLINK_ON_MS  = 70;
static const uint16_t ACTIVITY_BLINK_OFF_MS = 70;
static const uint8_t  ACTIVITY_BLINK_COUNT  = 4;

static bool ledInBurst = false;
static bool ledLevel = false;
static unsigned long ledNextMs = 0;
static uint8_t ledTogglesLeft = 0;

static void statusLedSetBase() {
  statusLedWrite(wifiConnected());
  ledLevel = wifiConnected();
}

static void statusLedActivityBurst() {
  ledInBurst = true;
  ledTogglesLeft = (uint8_t)(ACTIVITY_BLINK_COUNT * 2);
  statusLedWrite(false);
  ledLevel = false;
  ledNextMs = millis() + ACTIVITY_BLINK_OFF_MS;
}

static void statusLedTick() {
  const unsigned long now = millis();

  if (!ledInBurst) {
    statusLedSetBase();
    return;
  }
  if (now < ledNextMs) return;

  statusLedWrite(!ledLevel);
  ledLevel = !ledLevel;

  if (ledTogglesLeft > 0) ledTogglesLeft--;

  if (ledTogglesLeft == 0) {
    ledInBurst = false;
    statusLedSetBase();
    return;
  }

  ledNextMs = now + (ledLevel ? ACTIVITY_BLINK_ON_MS : ACTIVITY_BLINK_OFF_MS);
}

// ======================
// Siren engine (non-blocking)
// ======================
static bool sirenActive = false;
static uint8_t sirenCount = 0;
static uint16_t sirenOnMs[5]  = {0,0,0,0,0};
static uint16_t sirenOffMs[5] = {0,0,0,0,0};

static uint8_t sirenIndex = 0;
static bool sirenPhaseOn = false;
static unsigned long sirenNextMs = 0;

static inline void sirenWrite(bool on) {
  const bool level = SIREN_ACTIVE_HIGH ? on : !on;
  digitalWrite(SIREN_PIN, level ? HIGH : LOW);
}

static void sirenStop() {
  sirenActive = false;
  sirenCount = 0;
  sirenIndex = 0;
  sirenPhaseOn = false;
  sirenNextMs = 0;
  sirenWrite(false);
}

static void sirenStart(uint8_t count, const uint16_t* onArr, const uint16_t* offArr) {
  sirenCount = count;
  for (uint8_t i = 0; i < 5; i++) {
    sirenOnMs[i]  = (i < count) ? onArr[i]  : 0;
    sirenOffMs[i] = (i < count) ? offArr[i] : 0;
  }
  sirenIndex = 0;
  sirenPhaseOn = true;
  sirenActive = true;

  sirenWrite(true);
  sirenNextMs = millis() + sirenOnMs[0];
}

static void sirenTick() {
  if (!sirenActive) return;
  const unsigned long now = millis();
  if (now < sirenNextMs) return;

  if (sirenPhaseOn) {
    sirenWrite(false);
    sirenPhaseOn = false;
    sirenNextMs = now + sirenOffMs[sirenIndex];
    return;
  }

  if (sirenIndex + 1 >= sirenCount) {
    sirenStop();
    return;
  }

  sirenIndex++;
  sirenPhaseOn = true;
  sirenWrite(true);
  sirenNextMs = now + sirenOnMs[sirenIndex];
}

// ======================
// RC5 table (values from your working sketch)
// Spec mapping CMD 0x01..0x1B in exact order
// ======================
static uint8_t rc5_toggle_state = 0;
static const uint64_t RC5_TOGGLE_MASK_12BIT = 0x800ULL;

struct Rc5Entry {
  uint16_t bits;      // 12
  uint64_t v_t0;
  uint64_t v_t1;
};

static const Rc5Entry RC5_TABLE[27] = {
  {12, (0x8CAULL  & ~RC5_TOGGLE_MASK_12BIT), (0x8CAULL  | RC5_TOGGLE_MASK_12BIT)}, // 0: -bright
  {12, (0x0CBULL  & ~RC5_TOGGLE_MASK_12BIT), (0x0CBULL  | RC5_TOGGLE_MASK_12BIT)}, // 1: +bright
  {12, (0x80CULL  & ~RC5_TOGGLE_MASK_12BIT), (0x80CULL  | RC5_TOGGLE_MASK_12BIT)}, // 2: exit
  {12, (0x02FULL  & ~RC5_TOGGLE_MASK_12BIT), (0x02FULL  | RC5_TOGGLE_MASK_12BIT)}, // 3: prev_time
  {12, (0x838ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x838ULL  | RC5_TOGGLE_MASK_12BIT)}, // 4: time
  {12, (0x021ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x021ULL  | RC5_TOGGLE_MASK_12BIT)}, // 5: year
  {12, (0x820ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x820ULL  | RC5_TOGGLE_MASK_12BIT)}, // 6: date
  {12, (0x022ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x022ULL  | RC5_TOGGLE_MASK_12BIT)}, // 7: minus
  {12, (0x0E6ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x0E6ULL  | RC5_TOGGLE_MASK_12BIT)}, // 8: prev_date
  {12, (0x80DULL  & ~RC5_TOGGLE_MASK_12BIT), (0x80DULL  | RC5_TOGGLE_MASK_12BIT)}, // 9: sec
  {12, (0x011ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x011ULL  | RC5_TOGGLE_MASK_12BIT)}, // 10: F
  {12, (0x810ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x810ULL  | RC5_TOGGLE_MASK_12BIT)}, // 11: red
  {12, (0x02BULL  & ~RC5_TOGGLE_MASK_12BIT), (0x02BULL  | RC5_TOGGLE_MASK_12BIT)}, // 12: prev_tmp1
  {12, (0x800ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x800ULL  | RC5_TOGGLE_MASK_12BIT)}, // 13: 0
  {12, (0x801ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x801ULL  | RC5_TOGGLE_MASK_12BIT)}, // 14: 1
  {12, (0x002ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x002ULL  | RC5_TOGGLE_MASK_12BIT)}, // 15: 2
  {12, (0x803ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x803ULL  | RC5_TOGGLE_MASK_12BIT)}, // 16: 3
  {12, (0x02EULL  & ~RC5_TOGGLE_MASK_12BIT), (0x02EULL  | RC5_TOGGLE_MASK_12BIT)}, // 17: prev_hum
  {12, (0x804ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x804ULL  | RC5_TOGGLE_MASK_12BIT)}, // 18: 4
  {12, (0x005ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x005ULL  | RC5_TOGGLE_MASK_12BIT)}, // 19: 5
  {12, (0x806ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x806ULL  | RC5_TOGGLE_MASK_12BIT)}, // 20: 6
  {12, (0x02CULL  & ~RC5_TOGGLE_MASK_12BIT), (0x02CULL  | RC5_TOGGLE_MASK_12BIT)}, // 21: prev_press
  {12, (0x807ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x807ULL  | RC5_TOGGLE_MASK_12BIT)}, // 22: 7
  {12, (0x008ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x008ULL  | RC5_TOGGLE_MASK_12BIT)}, // 23: 8
  {12, (0x809ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x809ULL  | RC5_TOGGLE_MASK_12BIT)}, // 24: 9
  {12, (0x029ULL  & ~RC5_TOGGLE_MASK_12BIT), (0x029ULL  | RC5_TOGGLE_MASK_12BIT)}, // 25: prev_rad
  {12, (0x80FULL & ~RC5_TOGGLE_MASK_12BIT), (0x80FULL | RC5_TOGGLE_MASK_12BIT)}, // 26: prev_tmp2
};

// Useful indices for macros
static const uint8_t IDX_EXIT  = 2;
static const uint8_t IDX_8     = 23;
static const uint8_t IDX_9     = 24;

static inline uint16_t readU16LE(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void writeU16LE(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

static inline void sendRc5Press(uint8_t idx) {
  // Toggle flips on EVERY press (spec)
  rc5_toggle_state ^= 1;
  const uint64_t v = (rc5_toggle_state == 0) ? RC5_TABLE[idx].v_t0 : RC5_TABLE[idx].v_t1;
  irsend.sendRC5(v, RC5_TABLE[idx].bits, 0);
  statusLedActivityBurst();
}

// ======================
// Action queue (keeps UDP path ultra-short)
// ======================
enum class ActionType : uint8_t { Press = 1, Delay = 2 };
struct Action { ActionType type; uint16_t arg; };

static const uint8_t QSIZE = 32;
static Action q[QSIZE];
static uint8_t qHead = 0, qTail = 0, qCount = 0;
static unsigned long delayUntilMs = 0;

static inline bool qPush(Action a) {
  if (qCount >= QSIZE) return false;
  q[qTail] = a;
  qTail = (uint8_t)((qTail + 1) % QSIZE);
  qCount++;
  return true;
}
static inline bool qPop(Action &a) {
  if (qCount == 0) return false;
  a = q[qHead];
  qHead = (uint8_t)((qHead + 1) % QSIZE);
  qCount--;
  return true;
}
static inline void queuePress(uint8_t idx) { qPush({ActionType::Press, idx}); }
static inline void queueDelay(uint16_t ms) { qPush({ActionType::Delay, ms}); }

static void queueMacroModeSwitch() {
  queuePress(IDX_EXIT);
  queueDelay(IR_GAP_MS_DEFAULT);
  queuePress(IDX_EXIT);
  queueDelay(IR_GAP_MS_DEFAULT);
  queuePress(IDX_EXIT);
  queueDelay(IR_GAP_MS_EXIT3_END);
}

static void queueMacroResetScoreboard() {
  // per your addition: reset allowed only from pause => enforce pause first
  queuePress(IDX_9);
  queueDelay(IR_GAP_MS_DEFAULT);

  // reset = 8 triple press
  queuePress(IDX_8);
  queueDelay(IR_GAP_MS_DEFAULT);
  queuePress(IDX_8);
  queueDelay(IR_GAP_MS_DEFAULT);
  queuePress(IDX_8);
}

// ======================
// Dedup + ACK
// ======================
static uint16_t lastId = 0;
static bool lastIdValid = false;
static uint8_t lastAckStatus = 0;

static void sendAck(const IPAddress& ip, uint16_t port, uint16_t id, uint8_t status) {
  // 7 bytes per spec
  uint8_t a[7];
  a[0] = MAGIC;
  a[1] = VER;
  a[2] = CMD_ACK;
  writeU16LE(&a[3], id);
  a[5] = status; // 1 accepted, 0 rejected
  a[6] = 0;      // CODE reserved, always 0

  udp.beginPacket(ip, port);
  udp.write(a, sizeof(a));
  udp.endPacket();
}

// ======================
// OTA helpers (service mode)
// ======================
static void otaInitOnce() {
  if (otaInitialized) return;

  // No Serial output, no mDNS usage in our code.
  // ArduinoOTA itself can work by direct IP upload (PlatformIO) or discovery (if your tooling does it).
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPort(OTA_PORT);

  // No password (as requested)
  // ArduinoOTA.setPassword("...");

  // No callbacks (to avoid Serial/extra overhead). You can add LED patterns here if needed.
  ArduinoOTA.begin();

  otaInitialized = true;
}

static void otaEnterOrExtendWindow() {
  otaInitOnce();
  otaEnabled = true;
  otaUntilMs = millis() + OTA_WINDOW_MS;
  statusLedActivityBurst(); // simple visible feedback
}

static void otaTick() {
  if (!otaEnabled) return;
  if (!wifiConnected()) return;

  const unsigned long now = millis();
  if (now >= otaUntilMs) {
    otaEnabled = false;
    return;
  }

  // Handle OTA packets only during service window
  ArduinoOTA.handle();
}

// ======================
// Command handler (enqueue only)
// ======================
static bool enqueueCommand(uint8_t cmd, const uint8_t* payload, uint8_t payloadLen) {
  // If OTA service window is active: accept only OTA command to avoid accidental actions during update.
  if (otaEnabled) {
    if (cmd == CMD_OTA_MODE) {
      if (payloadLen != 0) return false;
      otaEnterOrExtendWindow();
      return true;
    }
    return false;
  }

  // Enter OTA mode (LEN must be 0)
  if (cmd == CMD_OTA_MODE) {
    if (payloadLen != 0) return false;
    otaEnterOrExtendWindow();
    return true;
  }

  // 0x01..0x1B => single IR press (index = cmd-1)
  if (cmd >= 0x01 && cmd <= 0x1B) {
    const uint8_t idx = (uint8_t)(cmd - 1);
    if (idx >= 27) return false;
    return qPush({ActionType::Press, idx});
  }

  if (cmd == CMD_MODE_SWITCH) {
    if (qCount > (QSIZE - 6)) return false;
    queueMacroModeSwitch();
    return true;
  }

  if (cmd == CMD_RESET_SCOREBOARD) {
    if (qCount > (QSIZE - 6)) return false;
    queueMacroResetScoreboard();
    return true;
  }

  if (cmd == CMD_SIREN) {
    if (payloadLen < 1) return false;
    const uint8_t count = payload[0];
    if (count < 1 || count > 3) return false; // spec: max 3
    const uint8_t need = (uint8_t)(1 + count * 4); // count + (on16,off16)*count
    if (payloadLen < need) return false;

    uint16_t onArr[5]  = {0,0,0,0,0};
    uint16_t offArr[5] = {0,0,0,0,0};
    const uint8_t* p = payload + 1;
    for (uint8_t i = 0; i < count; i++) {
      onArr[i]  = readU16LE(p); p += 2;
      offArr[i] = readU16LE(p); p += 2;
    }
    sirenStart(count, onArr, offArr);
    return true;
  }

  return false;
}

// ======================
// UDP processing (spec parser with LEN)
// ======================
static void processUdp() {
  int packetSize;
  while ((packetSize = udp.parsePacket()) > 0) {
    uint8_t buf[128];
    int len = udp.read((char*)buf, (int)sizeof(buf));
    if (len < 6) continue; // minimum 6 bytes per spec

    const IPAddress remoteIP = udp.remoteIP();
    const uint16_t remotePort = udp.remotePort();

    // Parse header
    if (buf[0] != MAGIC) continue;
    if (buf[1] != VER) continue;

    const uint8_t cmd = buf[2];
    const uint16_t id = readU16LE(&buf[3]);
    const uint8_t plen = buf[5];

    // Validate length strictly: 6 + plen
    if ((int)(6 + plen) != len) {
      // reject, but still ACK with status=0
      lastId = id; lastIdValid = true; lastAckStatus = 0;
      sendAck(remoteIP, remotePort, id, 0);
      continue;
    }

    // Dedup by last ID
    if (lastIdValid && id == lastId) {
      sendAck(remoteIP, remotePort, id, lastAckStatus);
      continue;
    }

    const uint8_t* payload = (plen > 0) ? &buf[6] : nullptr;

    const bool ok = enqueueCommand(cmd, payload, plen);

    // Save dedup state
    lastId = id;
    lastIdValid = true;
    lastAckStatus = ok ? 1 : 0;

    // Optional: blink immediately on accepted command
    if (ok) statusLedActivityBurst();

    // ACK immediately (before IR execution), per spec
    sendAck(remoteIP, remotePort, id, lastAckStatus);
  }
}

// ======================
// Action executor (non-blocking)
// ======================
static void actionTick() {
  const unsigned long now = millis();
  if (delayUntilMs != 0 && now < delayUntilMs) return;
  delayUntilMs = 0;

  if (qCount == 0) return;

  Action a;
  if (!qPop(a)) return;

  if (a.type == ActionType::Delay) {
    delayUntilMs = now + (unsigned long)a.arg;
    return;
  }

  if (a.type == ActionType::Press) {
    const uint8_t idx = (uint8_t)(a.arg & 0xFF);
    if (idx < 27) sendRc5Press(idx);
    return;
  }
}

// ======================
// Wi-Fi reconnect tick (minimal, non-blocking)
// ======================
static unsigned long nextWifiRetryMs = 0;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 3000;

static void wifiTick() {
  const unsigned long now = millis();
  if (wifiConnected()) return;
  if (now < nextWifiRetryMs) return;
  nextWifiRetryMs = now + WIFI_RETRY_INTERVAL_MS;

  WiFi.reconnect();
}

// ======================
// Setup / Loop
// ======================
void setup() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  statusLedWrite(false);

  pinMode(SIREN_PIN, OUTPUT);
  sirenWrite(false);

  irsend.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  // Optional (doesn't use Serial): set hostname for OTA convenience
  WiFi.setHostname(OTA_HOSTNAME);

  // Disable Wi-Fi power save/sleep for minimum latency
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  udp.begin(UDP_PORT);

  // Do NOT start OTA here; OTA is started only on demand via CMD_OTA_MODE.
}

void loop() {
  statusLedTick();

  wifiTick();

  if (wifiConnected()) {
    processUdp();
  }

  // OTA handling is active only during the service window.
  otaTick();

  // Keep your real-time engines running always
  actionTick();
  sirenTick();
}

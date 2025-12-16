// Swatchle – T-Display S3 Touch
// Authors: <N.B.Solangi, E.Sairoglu, A.J.Westcott, K.Tandean, T.Destura>
// Course: <CBL Project 1>
// Year: 2025

#define TOUCH_MODULES_CST_SELF

#include <TFT_eSPI.h>
#include <Wire.h>
#include <math.h>

#include "pin_config.h"
#include <TouchLib.h>
#include <Adafruit_TCS34725.h>

// ---- Connectivity ----
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// --- WiFi + Web control panel ---
#include <FS.h>
using fs::FS;
#include <WebServer.h>

// -------------------- SHARED COLOR STATE --------------------

// Mission color (daily target)
uint8_t missionR = 0x6E;    // default #6E081A
uint8_t missionG = 0x08;
uint8_t missionB = 0x1A;
String  missionHex = "#6E081A";

uint16_t missionColor565 = 0;

// Last scanned / user color
uint8_t userR = 0x80;
uint8_t userG = 0x80;
uint8_t userB = 0x80;
String  userHex = "#808080";

uint16_t userColor565    = 0;

// HTTP JSON + UI server
WebServer server(80);

// ==================== WiFi CONFIG ====================

#undef WIFI_SSID
#undef WIFI_PASS

#define WIFI_SSID "Your WiFi SSID"
#define WIFI_PASS "Your WiFi Password"

// ========== BLE CONFIG ==========
static const char *BLE_DEVICE_NAME  = "Swatchle";
static const char *BLE_SERVICE_UUID = "6e081a00-0000-4c0a-8000-001234567890";
static const char *BLE_CHAR_TX_UUID = "6e081a01-0000-4c0a-8000-001234567890"; // notify
static const char *BLE_CHAR_RX_UUID = "6e081a02-0000-4c0a-8000-001234567890"; // write

// -------------------- TFT + TOUCH --------------------

TFT_eSPI tft = TFT_eSPI();
TouchLib touch(Wire, PIN_IIC_SDA, PIN_IIC_SCL, CTS820_SLAVE_ADDRESS, PIN_TOUCH_RES);

const uint8_t ROT = 0; // portrait

int SCREEN_W = 0;
int SCREEN_H = 0;

// Bottom bar
int BTN_H;
int BTN_Y;

// -------------------- COLOR THEME --------------------

#define COL_BG           TFT_WHITE
#define COL_TOPBAR       TFT_DARKGREY

#define COL_HOME_APP     TFT_NAVY
#define COL_HOME_DRAW    TFT_DARKCYAN
#define COL_HOME_MISSION TFT_DARKGREY

#define COL_BTN_HOME     TFT_NAVY
#define COL_BTN_CLEAR    TFT_MAROON
#define COL_BTN_SCAN     TFT_DARKCYAN
#define COL_BTN_CONNECT  TFT_DARKCYAN

// -------------------- UI STATE --------------------

enum UiScreen {
  UI_HOME,
  UI_APP,
  UI_DRAW,
  UI_DAILY
};

UiScreen currentScreen = UI_HOME;

struct Button {
  int x, y, w, h;
  const char *label;
  uint16_t color;
  UiScreen target;
};

Button homeButtons[3];

// Draw state
bool drawing = false;
int lastX = 0, lastY = 0;
bool wasTouching = false;

// -------------------- COLOR SENSOR --------------------

Adafruit_TCS34725 tcs(
  TCS34725_INTEGRATIONTIME_50MS,
  TCS34725_GAIN_4X
);

bool sensorOk = false;

// -------------------- DAILY MISSION COLORS --------------------

uint16_t user565 = TFT_DARKGREY;
bool haveUserColor = false;
int accuracyPercent = 0;

// -------------------- LED CONTROL FOR SENSOR RING --------------------
// TEMP: this assumes we wired the Gravity TCS34725 LED pin to GPIO 10.

enum LedMode {
  LED_MODE_OFF,
  LED_MODE_DIM
};

LedMode ledMode = LED_MODE_DIM;
#define LED_PIN 10

void applyLedMode() {
  switch (ledMode) {
    case LED_MODE_OFF:
      analogWrite(LED_PIN, 0);    // off
      break;
    case LED_MODE_DIM:
      analogWrite(LED_PIN, 64);   // dim-ish
      break;
  }
}

// -------------------- COLOR CALIBRATION -----------------
// Derived from the white sample: RAW r=153 g=254 b=414 c=822
const float CLEAR_WHITE = 822.0f;
float kR = 1.79f;
float kG = 1.08f;
float kB = 0.66f;

// -------------------- CONNECTIVITY STATE --------------------

bool wifiConnected = false;
bool bleConnected  = false;

String ipString = "";

BLEServer         *pServer           = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;
BLECharacteristic *pRxCharacteristic = nullptr;

// -------------------- BLE CALLBACKS --------------------

class SwatchleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleConnected = true;
    Serial.println("BLE: device connected");
  }
  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    Serial.println("BLE: device disconnected, restarting advertising");
    server->getAdvertising()->start();
  }
};

class SwatchleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String value = pCharacteristic->getValue();
    if (value.length() > 0) {
      Serial.print("BLE RX: ");
      Serial.println(value);
      // parse BLE commands here if needed
    }
  }
};

// -------------------- TOUCH MAPPING (PORTRAIT) --------------------

void mapTouch(int rx, int ry, int &x, int &y) {
  x = rx;
  y = ry;

  if (x < 0) x = 0;
  if (x >= SCREEN_W) x = SCREEN_W - 1;
  if (y < 0) y = 0;
  if (y >= SCREEN_H) y = SCREEN_H - 1;
}

// Convert 8-bit RGB to 16-bit 565 for TFT
uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// To be called any time the mission color changes
void applyMissionColorFromRGB(uint8_t r, uint8_t g, uint8_t b) {
  missionR = r;
  missionG = g;
  missionB = b;
  missionColor565 = rgbTo565(r, g, b);

  char buf[8];
  sprintf(buf, "#%02X%02X%02X", r, g, b);
  missionHex = String(buf);
}

// VERY small hex parser"
bool parseHexColor(const String &hex, uint8_t &r, uint8_t &g, uint8_t &b) {
  String h = hex;
  h.trim();
  if (h.startsWith("#")) h.remove(0, 1);
  if (h.length() != 6) return false;

  auto hex2byte = [](char hi, char lo) -> int {
    auto val = [](char c)->int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    int v1 = val(hi), v2 = val(lo);
    if (v1 < 0 || v2 < 0) return -1;
    return (v1 << 4) | v2;
  };

  int rInt = hex2byte(h[0], h[1]);
  int gInt = hex2byte(h[2], h[3]);
  int bInt = hex2byte(h[4], h[5]);
  if (rInt < 0 || gInt < 0 || bInt < 0) return false;

  r = (uint8_t)rInt;
  g = (uint8_t)gInt;
  b = (uint8_t)bInt;
  return true;
}

// A tiny “distance” score used as color match metric
float colorMatchPercent(uint8_t r1, uint8_t g1, uint8_t b1,
                        uint8_t r2, uint8_t g2, uint8_t b2) {
  float dr = (float)r1 - r2;
  float dg = (float)g1 - g2;
  float db = (float)b1 - b2;
  float dist = sqrtf(dr*dr + dg*dg + db*db);   // max ≈ 441
  float score = 1.0f - dist / 441.0f;
  if (score < 0) score = 0;
  return score * 100.0f;
}

// ==================== CONNECTIVITY HELPERS ====================

// --- WiFi ---

void connectWiFi() {
  Serial.println("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("Swatchle");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  const unsigned long TIMEOUT = 20000; // 20 seconds

  while (WiFi.status() != WL_CONNECTED && millis() - start < TIMEOUT) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    ipString = WiFi.localIP().toString();
    Serial.print("WiFi connected, IP: ");
    Serial.println(ipString);
  } else {
    wifiConnected = false;
    ipString = "";
    Serial.print("WiFi connection failed, status=");
    Serial.println(WiFi.status());
  }
}

// --- BLE ---

void setupBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new SwatchleServerCallbacks());

  BLEService *pService = pServer->createService(BLE_SERVICE_UUID);

  // TX characteristic (notify to phone)
  pTxCharacteristic = pService->createCharacteristic(
      BLE_CHAR_TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  // RX characteristic (write from phone)
  pRxCharacteristic = pService->createCharacteristic(
      BLE_CHAR_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE
  );
  pRxCharacteristic->setCallbacks(new SwatchleRxCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);

  BLEDevice::startAdvertising();
  Serial.println("BLE: advertising started");
}

void bleSendColor(uint8_t r, uint8_t g, uint8_t b) {
  if (!bleConnected || pTxCharacteristic == nullptr) return;

  char buf[16];
  sprintf(buf, "#%02X%02X%02X", r, g, b);
  pTxCharacteristic->setValue((uint8_t*)buf, strlen(buf));
  pTxCharacteristic->notify();
  Serial.print("BLE TX color: ");
  Serial.println(buf);
}

// ==================== HTTP HELPERS (WebServer) ====================

void handleRoot() {
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Swatchle</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Helvetica,Arial,sans-serif;"
      "background:#f5f5f5;margin:0;padding:16px;color:#333;}"
    ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:16px;"
      "box-shadow:0 2px 6px rgba(0,0,0,.08);}"
    "h1{margin:0 0 8px;font-size:22px;}"
    "h2{margin:0 0 8px;font-size:18px;}"
    "label{display:block;font-size:14px;margin-bottom:4px;}"
    "input[type=text]{width:100%;padding:8px 10px;font-size:16px;"
      "border-radius:8px;border:1px solid #ccc;box-sizing:border-box;}"
    "button{margin-top:8px;padding:8px 14px;border-radius:999px;border:none;"
      "background:#007AFF;color:#fff;font-weight:600;font-size:15px;}"
    ".row{display:flex;gap:12px;margin-top:8px;}"
    ".swatch{flex:1;border-radius:999px;height:60px;display:flex;"
      "align-items:center;justify-content:center;color:#fff;font-weight:600;}"
    ".swatch span{background:rgba(0,0,0,.35);padding:4px 8px;border-radius:999px;"
      "font-size:12px;}"
    "</style></head><body>";

  html +=
    "<div class='card'>"
      "<h1>Swatchle – Daily Mission</h1>"
      "<p style='margin:0 0 8px'>Device IP: " + WiFi.localIP().toString() + "</p>"
      "<label>Set mission color (hex, e.g. #27F5B0)</label>"
      "<input id='hex' type='text' value='" + missionHex + "'>"
      "<button onclick='setMission()'>Update mission</button>"
    "</div>"

    "<div class='card'>"
      "<h2>Live status</h2>"
      "<div class='row'>"
        "<div id='missionSwatch' class='swatch'><span>mission</span></div>"
        "<div id='userSwatch' class='swatch'><span>yours</span></div>"
      "</div>"
      "<div id='match'>–</div>"
      "<button onclick='scanOnce()'"
        "style='margin-top:12px;background:#34C759;'>Scan once</button>"
    "</div>"

    "<script>"
    "function setMission(){"
      "const hex=document.getElementById('hex').value;"
      "fetch('/api/set_mission?hex='+encodeURIComponent(hex))"
        ".then(r=>r.text())"
        ".then(msg=>alert(msg))"
        ".catch(err=>console.error(err));"
    "}"
    "function scanOnce(){"
      "fetch('/api/scan_once',{method:'POST'})"
        ".then(_=>updateStatus())"
        ".catch(err=>console.error(err));"
    "}"
    "function updateStatus(){"
      "fetch('/api/status')"
        ".then(r=>r.json())"
        ".then(data=>{"
          "document.getElementById('missionSwatch').style.background=data.mission;"
          "document.getElementById('userSwatch').style.background=data.user;"
          "document.getElementById('match').textContent="
            "'Match: '+data.match.toFixed(1)+'%';"
        "})"
        ".catch(err=>console.error(err));"
    "}"
    "updateStatus();"
    "setInterval(updateStatus,1500);"
    "</script></body></html>";

  server.send(200, "text/html", html);
}

// /api/status  → JSON with both colors + match
void handleStatus() {
  float match = colorMatchPercent(
    missionR, missionG, missionB,
    userR,    userG,    userB
  );

  String json = "{";
  json += "\"mission\":\"" + missionHex + "\",";
  json += "\"user\":\""    + userHex    + "\",";
  json += "\"match\":"     + String(match, 1);
  json += "}";

  server.send(200, "application/json", json);
}

// /api/set_mission?hex=#27F5B0
void handleSetMission() {
  if (!server.hasArg("hex")) {
    server.send(400, "text/plain", "Missing hex parameter");
    return;
  }

  String hex = server.arg("hex");
  uint8_t r, g, b;
  if (!parseHexColor(hex, r, g, b)) {
    server.send(400, "text/plain", "Invalid hex color");
    return;
  }

  // Update mission color used everywhere (UI + website)
  applyMissionColorFromRGB(r, g, b);

  // Reset user color / accuracy for new mission
  haveUserColor   = false;
  accuracyPercent = 0;

  userR = userG = userB = 0x80;
  user565 = rgbTo565(userR, userG, userB);
  userHex = "#808080";

  // If we are on the Mission screen, redrawing it so the left half updates
  if (currentScreen == UI_DAILY) {
    drawDailyMission();
  }

  server.send(200, "text/plain", "Mission color set to " + missionHex);
}

// /api/scan_once –  copy mission→user
void handleScanOnce() {
  userR = missionR;
  userG = missionG;
  userB = missionB;

  userColor565 = rgbTo565(userR, userG, userB);
  user565      = userColor565;

  userHex = missionHex;
  haveUserColor   = true;
  accuracyPercent = 100;

  server.send(200, "text/plain", "Scan simulated (user color = mission)");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void setupWifiAndServer() {
  Serial.println();
  Serial.printf("Connecting to WiFi SSID \"%s\"...\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timeout, continuing offline");
      break;
    }
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected! IP: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
    ipString = WiFi.localIP().toString();
  } else {
    wifiConnected = false;
    ipString = "";
  }

  // initialize cached color values
  applyMissionColorFromRGB(missionR, missionG, missionB);
  userColor565 = rgbTo565(userR, userG, userB);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/set_mission", HTTP_GET, handleSetMission);
  server.on("/api/set_mission", HTTP_POST, handleSetMission);
  server.on("/api/scan_once", HTTP_POST, handleScanOnce);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("WebServer started on port 80");
}

// ==================== UI SCREENS ====================

void drawHomeScreen() {
  tft.fillScreen(COL_BG);

  // TOP BAR
  tft.fillRect(0, 0, SCREEN_W, 40, COL_TOPBAR);

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE, COL_TOPBAR);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Swatchle", SCREEN_W / 2, 22);

  // Subtitle
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_BLACK, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Select an app:", 10, 52);

  int marginX = 10;
  int marginY = 80;
  int gapY    = 8;

  int btnW = SCREEN_W - 2 * marginX;
  int btnH = 48;

  int y0 = marginY;
  int y1 = y0 + btnH + gapY;
  int y2 = y1 + btnH + gapY;

  homeButtons[0] = { marginX, y0, btnW, btnH, "App",     COL_HOME_APP,     UI_APP    };
  homeButtons[1] = { marginX, y1, btnW, btnH, "Draw",    COL_HOME_DRAW,    UI_DRAW   };
  homeButtons[2] = { marginX, y2, btnW, btnH, "Mission", COL_HOME_MISSION, UI_DAILY  };

  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);

  for (int i = 0; i < 3; i++) {
    Button &b = homeButtons[i];

    tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, b.color);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, 8, TFT_WHITE);
    tft.drawString(b.label, b.x + b.w / 2, b.y + btnH / 2 + 1);
  }

  tft.setTextDatum(TL_DATUM);
}

void drawAppScreen() {
  tft.fillScreen(COL_BG);

  // TOP BAR
  tft.fillRect(0, 0, SCREEN_W, 40, COL_TOPBAR);

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE, COL_TOPBAR);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("App", SCREEN_W / 2, 22);

  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_BLACK, COL_BG);
  tft.setTextDatum(TL_DATUM);

  int y = 52;
  tft.drawString("Connectivity", 10, y);
  y += 20;

  String wifiLine = "WiFi: ";
  wifiLine += (wifiConnected ? "Connected" : "Not connected");
  tft.drawString(wifiLine, 10, y);
  y += 18;

  if (wifiConnected && ipString.length() > 0) {
    String ipLine = "IP: " + ipString;
    tft.drawString(ipLine, 10, y);
    y += 18;
  }

  String bleLine = "BLE:  ";
  if (bleConnected) bleLine += "Connected";
  else              bleLine += "Advertising";
  tft.drawString(bleLine, 10, y);
  y += 30;

  tft.drawString("Use CONNECT to", 10, y);
  y += 16;
  tft.drawString("join WiFi + HTTP UI", 10, y);

  // BOTTOM BAR
  tft.fillRect(0, BTN_Y, SCREEN_W, BTN_H, COL_TOPBAR);
  int halfW = SCREEN_W / 2;

  int homeX = 0;
  int homeW = halfW;
  int connX = halfW;
  int connW = halfW;

  tft.fillRect(homeX, BTN_Y, homeW, BTN_H, COL_BTN_HOME);
  tft.fillRect(connX, BTN_Y, connW, BTN_H, COL_BTN_CONNECT);

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("HOME",    homeX + homeW / 2, BTN_Y + BTN_H / 2 + 1);
  tft.drawString("CONNECT", connX + connW / 2, BTN_Y + BTN_H / 2 + 1);

  tft.setTextDatum(TL_DATUM);
}

void drawDrawScreenUI() {
  tft.fillScreen(COL_BG);

  // TOP BAR
  tft.fillRect(0, 0, SCREEN_W, 40, COL_TOPBAR);

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE, COL_TOPBAR);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Draw", SCREEN_W / 2, 22);

  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_BLACK, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Draw with your", 10, 52);
  tft.drawString("finger",          10, 68);

  // BOTTOM BAR
  tft.fillRect(0, BTN_Y, SCREEN_W, BTN_H, COL_TOPBAR);

  int halfW = SCREEN_W / 2;

  int homeX = 0;
  int homeW = halfW;
  int clearX = halfW;
  int clearW = halfW;

  tft.fillRect(homeX, BTN_Y, homeW, BTN_H, COL_BTN_HOME);
  tft.fillRect(clearX, BTN_Y, clearW, BTN_H, COL_BTN_CLEAR);

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("HOME",  homeX + homeW  / 2, BTN_Y + BTN_H / 2 + 1);
  tft.drawString("CLEAR", clearX + clearW / 2, BTN_Y + BTN_H / 2 + 1);

  tft.setTextDatum(TL_DATUM);
}

void drawDailyMission() {
  tft.fillScreen(COL_BG);

  // TITLE
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_BLACK, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Daily mission", SCREEN_W / 2, 26);

  // HEX of mission (centered)
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_BLACK, COL_BG);
  tft.drawString(missionHex, SCREEN_W / 2, 50);

  // LED mode line (centered, with tap hint)
  String ledLine = (ledMode == LED_MODE_OFF)
                   ? "LED: Off (tap)"
                   : "LED: Dim (tap)";
  tft.drawString(ledLine, SCREEN_W / 2, 70);

  tft.setTextDatum(TL_DATUM);

  // SPLIT CIRCLE
  int cx = SCREEN_W / 2;
  int cy = 150;
  int r  = 60;

  // Left half: mission color
  for (int x = -r; x <= 0; x++) {
    int h = (int)sqrtf((float)(r * r - x * x));
    tft.drawFastVLine(cx + x, cy - h, 2 * h, missionColor565);
  }

  // Right half: user or placeholder
  uint16_t rightCol = haveUserColor ? user565 : TFT_LIGHTGREY;
  for (int x = 0; x <= r; x++) {
    int h = (int)sqrtf((float)(r * r - x * x));
    tft.drawFastVLine(cx + x, cy - h, 2 * h, rightCol);
  }

  // Labels
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_BLACK, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Swatchle", cx - r / 2, cy - 18);
  tft.drawString("Yours",    cx + r / 2, cy + 18);

  // Accuracy / prompt
  if (haveUserColor) {
    tft.setFreeFont(&FreeSansBold9pt7b);
    char buf[8];
    sprintf(buf, "%d%%", accuracyPercent);
    tft.drawString(buf, SCREEN_W / 2, 240);

    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextColor(TFT_BLACK, COL_BG);
    if (accuracyPercent > 85) {
      tft.drawString("Great match!", SCREEN_W / 2, 270);
    } else if (accuracyPercent > 60) {
      tft.drawString("Getting closer", SCREEN_W / 2, 270);
    } else {
      tft.drawString("Try again!", SCREEN_W / 2, 270);
    }
  } else {
    tft.setFreeFont(&FreeSans9pt7b);
    tft.setTextColor(TFT_BLACK, COL_BG);
    tft.drawString("Scan a color", SCREEN_W / 2, 240);
  }

  tft.setTextDatum(TL_DATUM);

  // BOTTOM BAR
  tft.fillRect(0, BTN_Y, SCREEN_W, BTN_H, COL_TOPBAR);

  int halfW = SCREEN_W / 2;
  int homeX = 0;
  int homeW = halfW;
  int scanX = halfW;
  int scanW = halfW;

  tft.fillRect(homeX, BTN_Y, homeW, BTN_H, COL_BTN_HOME);
  tft.fillRect(scanX, BTN_Y, scanW, BTN_H, COL_BTN_SCAN);

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("HOME", homeX + homeW / 2, BTN_Y + BTN_H / 2 + 1);
  tft.drawString("SCAN", scanX + scanW / 2, BTN_Y + BTN_H / 2 + 1);

  tft.setTextDatum(TL_DATUM);
}

// ==================== COLOR SCAN + ACCURACY ====================

void scanColor() {
  if (!sensorOk) {
    haveUserColor = false;
    drawDailyMission();
    return;
  }

  uint16_t r16, g16, b16, c16;
  tcs.getRawData(&r16, &g16, &b16, &c16);

  // DARK CASE: very low light → treat as neutral grey/black
  if (c16 < CLEAR_WHITE * 0.05f) {   // ~5% of white
    float brightness = (float)c16 / CLEAR_WHITE;
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;

    uint8_t v = (uint8_t)(brightness * 255.0f + 0.5f);
    userR = v;
    userG = v;
    userB = v;
  } else {
    // NORMAL / BRIGHT CASE: white-balance + normalize
    float c = (float)c16;
    if (c < 1.0f) c = 1.0f;

    float r = (float)r16;
    float g = (float)g16;
    float b = (float)b16;

    // Normalize by clear channel
    float rn = r / c;
    float gn = g / c;
    float bn = b / c;

    // Apply per-channel gains (white reference)
    rn *= kR;
    gn *= kG;
    bn *= kB;

    // Normalize so max channel = 1
    float maxc = rn;
    if (gn > maxc) maxc = gn;
    if (bn > maxc) maxc = bn;
    if (maxc < 1e-6f) maxc = 1.0f;

    rn /= maxc;
    gn /= maxc;
    bn /= maxc;

    int R = (int)(rn * 255.0f + 0.5f);
    int G = (int)(gn * 255.0f + 0.5f);
    int B = (int)(bn * 255.0f + 0.5f);

    if (R < 0) R = 0; if (R > 255) R = 255;
    if (G < 0) G = 0; if (G > 255) G = 255;
    if (B < 0) B = 0; if (B > 255) B = 255;

    userR = (uint8_t)R;
    userG = (uint8_t)G;
    userB = (uint8_t)B;
  }

  // Update 565 + hex + flags
  user565 = tft.color565(userR, userG, userB);
  haveUserColor = true;

  char hexbuf[8];
  sprintf(hexbuf, "#%02X%02X%02X", userR, userG, userB);
  userHex = String(hexbuf);

  // Match vs mission
  float dr = (float)missionR - (float)userR;
  float dg = (float)missionG - (float)userG;
  float db = (float)missionB - (float)userB;

  float dist = sqrtf(dr * dr + dg * dg + db * db);
  float maxDist = sqrtf(255.0f * 255.0f * 3.0f);

  float acc = 100.0f - (dist / maxDist * 100.0f);
  if (acc < 0.0f)   acc = 0.0f;
  if (acc > 100.0f) acc = 100.0f;

  accuracyPercent = (int)(acc + 0.5f);

  bleSendColor(userR, userG, userB);
  drawDailyMission();
}

// ==================== TOUCH HANDLERS ====================

UiScreen handleHomeTouch(int x, int y, bool newPress) {
  if (!newPress) return UI_HOME;

  for (int i = 0; i < 3; i++) {
    Button &b = homeButtons[i];
    if (x >= b.x && x <= b.x + b.w &&
        y >= b.y && y <= b.y + b.h) {
      return b.target;
    }
  }
  return UI_HOME;
}

UiScreen handleAppTouch(int x, int y, bool newPress) {
  if (!newPress) return UI_APP;

  if (y >= BTN_Y) {
    int halfW = SCREEN_W / 2;
    if (x < halfW) {
      drawHomeScreen();
      return UI_HOME;
    } else {
      connectWiFi();
      drawAppScreen();
      return UI_APP;
    }
  }
  return UI_APP;
}

UiScreen handleDrawTouch(int x, int y, bool newPress) {
  if (y >= BTN_Y) {
    int halfW = SCREEN_W / 2;

    if (newPress) {
      if (x < halfW) {
        drawing = false;
        drawHomeScreen();
        return UI_HOME;
      } else {
        drawing = false;
        drawDrawScreenUI();
        return UI_DRAW;
      }
    }
    return UI_DRAW;
  }

  if (!drawing) {
    drawing = true;
    lastX = x;
    lastY = y;
  }
  tft.drawLine(lastX, lastY, x, y, TFT_RED);
  lastX = x;
  lastY = y;

  return UI_DRAW;
}

UiScreen handleDailyMissionTouch(int x, int y, bool newPress) {
  if (!newPress) return UI_DAILY;

  // LED line tap area (full width around y ~ 70)
  if (y >= 60 && y <= 85) {
    ledMode = (ledMode == LED_MODE_OFF) ? LED_MODE_DIM : LED_MODE_OFF;
    applyLedMode();
    drawDailyMission();
    return UI_DAILY;
  }

  // Bottom bar HOME/SCAN
  if (y >= BTN_Y) {
    int halfW = SCREEN_W / 2;

    if (x < halfW) {
      drawHomeScreen();
      return UI_HOME;
    } else {
      scanColor();
      return UI_DAILY;
    }
  }

  return UI_DAILY;
}

// ==================== SETUP & LOOP ====================

void setup() {
  Serial.begin(115200);

  // Power + backlight
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  pinMode(PIN_TOUCH_RES, OUTPUT);
  digitalWrite(PIN_TOUCH_RES, LOW);
  delay(200);
  digitalWrite(PIN_TOUCH_RES, HIGH);

  // I2C + touch
  Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL);
  touch.init();
  touch.setRotation(ROT);

  // TFT
  tft.init();
  tft.setRotation(ROT);
  tft.setTextWrap(false);

  SCREEN_W = tft.width();
  SCREEN_H = tft.height();

  BTN_H = 40;
  BTN_Y = SCREEN_H - BTN_H;

  // Initial mission color
  applyMissionColorFromRGB(missionR, missionG, missionB);

  // Color sensor
  sensorOk = tcs.begin();
  Serial.println(sensorOk ? "TCS34725 OK" : "TCS34725 not found");

  // LED control pin (for sensor LED when wired)
  pinMode(LED_PIN, OUTPUT);
  applyLedMode();

  // Connectivity
  wifiConnected = false;
  setupBLE();
  setupWifiAndServer();

  currentScreen = UI_HOME;
  drawHomeScreen();
}

void loop() {
  // handle HTTP requests
  server.handleClient();

  // TOUCH
  bool touching = touch.read();
  if (touching) {
    TP_Point p = touch.getPoint(0);
    int rx = p.x;
    int ry = p.y;
    int x, y;
    mapTouch(rx, ry, x, y);

    bool newPress = !wasTouching;

    switch (currentScreen) {
      case UI_HOME: {
        UiScreen next = handleHomeTouch(x, y, newPress);
        if (next != currentScreen) {
          currentScreen = next;
          if      (next == UI_APP)   drawAppScreen();
          else if (next == UI_DRAW)  drawDrawScreenUI();
          else if (next == UI_DAILY) drawDailyMission();
          else                       drawHomeScreen();
        }
        break;
      }

      case UI_APP:
        currentScreen = handleAppTouch(x, y, newPress);
        break;

      case UI_DRAW:
        currentScreen = handleDrawTouch(x, y, newPress);
        break;

      case UI_DAILY:
        currentScreen = handleDailyMissionTouch(x, y, newPress);
        break;
    }

    wasTouching = true;
  } else {
    drawing = false;
    wasTouching = false;
  }
}
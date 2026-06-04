#ifndef ENABLE_DIAGNOSTICS
#define ENABLE_DIAGNOSTICS 0
#endif

#if ENABLE_DIAGNOSTICS
#define DEBUG_ESP_PORT Serial
#endif

#include <WiFi.h> 
#include <WebSocketsClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <vector>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Preferences.h>
#include "esp_log.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <FastLED.h>

#define CURRENT_VERSION "v0.10.2"

static const char* TAG = "FitzBell";

// -------- LED Strip --------
#define LED_PIN     5
#define NUM_LEDS    12
#define BRIGHTNESS  64
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
CRGB leds[NUM_LEDS];

// -------- LED Helper --------
void setLedColor(CRGB color) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}

int ringWrap(int index) {
  while (index < 0) index += NUM_LEDS;
  return index % NUM_LEDS;
}

uint8_t triWave8FromPhase(uint8_t phase) {
  return (phase < 128) ? (phase * 2) : ((255 - phase) * 2);
}

// -------- OLED Display --------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// -------- Device User --------
char userId[40] = "Guest"; // Mutable buffer for user ID
Preferences preferences;   // For saving userId to NVS

// -------- WebSocket server details --------
const char serverAddress[] = "gamerbell.fitznet.doomdns.org";  // Your server IP / hostname
const int  serverPort      = 443;              // HTTPS/API port
const int  wsPort          = 443;              // WSS port
const char wsPath[]        = "/ws";           // WebSocket path

// -------- Hardware pin for the button --------
#define BUTTON_PIN 13   // GPIO pin for the button (to GND with INPUT_PULLUP)

// WebSocket client
WebSocketsClient webSocket;

// Track button state to prevent duplicate messages
bool buttonPressed = false;
bool wsConnected = false;
bool updateInProgress = false;
int updatePercent = -1;
unsigned long ledLastFrameMs = 0;
uint8_t ledPhase = 0;
// OLED redraw is a ~20-80ms blocking I2C push; defer it out of the hot path
// and throttle to ~20Hz so it never stalls the LED render loop.
bool displayDirty = false;
unsigned long lastDisplayFlushMs = 0;
const unsigned long displayMinIntervalMs = 50;
unsigned long txActivityUntilMs = 0;
unsigned long rxActivityUntilMs = 0;
const unsigned long activityWindowMs = 300;

// Update Scheduler
unsigned long lastUpdateCheck = 0;
const unsigned long updateInterval = 60000; // 1 minute (prototype)
const unsigned long ledFrameIntervalMs = 16; // ~60 FPS

// Count Polling
unsigned long lastCountCheck = 0;
const unsigned long countInterval = 10000; // 10 seconds
const unsigned long backendRetryDelayMs = 30000; // back off after backend failures
unsigned long backendRetryAtMs = 0;
const unsigned long wsDiagIntervalMs = 30000;
unsigned long lastWsDiagMs = 0;
int onlineCount = 0;
bool countApiError = false;

struct CountFetchResult {
  bool wifiDown;
  bool success;
  bool parseError;
  int httpCode;
  int count;
};

enum class UpdateJobResult : uint8_t {
  NONE,
  FAILED,
  NO_UPDATES,
  OK
};

portMUX_TYPE networkMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t networkTaskHandle = nullptr;

volatile bool countJobPending = false;
volatile bool countJobRunning = false;
volatile bool countResultReady = false;
CountFetchResult countResult = {false, false, false, -1, 0};

volatile bool updateJobPending = false;
volatile bool updateJobRunning = false;
volatile bool updateRequestSilent = true;
volatile bool updateResultReady = false;
UpdateJobResult updateResult = UpdateJobResult::NONE;
char updateErrorMsg[96] = {0};

// Global display state
String statusMessage = "Booting...";
std::vector<String> activeUsers;
String lastConnectionError = "";

enum class LedMode {
  BOOTING,
  WIFI_DOWN,
  BACKEND_DOWN,
  WS_DOWN,
  UPDATING,
  TX_ONLY,
  RX_ONLY,
  TX_RX,
  BUTTON_HELD,
  COUNT_ERROR,
  ACTIVE_USERS,
  READY
};

LedMode currentLedMode = LedMode::BOOTING;

bool isBackendUnreachable() {
  return WiFi.status() == WL_CONNECTED && !wsConnected && countApiError;
}

const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "WL_UNKNOWN";
  }
}

void logDiagnostics(const char* context) {
#if ENABLE_DIAGNOSTICS
  String ip = WiFi.localIP().toString();
  Serial.printf(
    "[diag] %s | wifi=%s ip=%s rssi=%ld ws=%s countErr=%s count=%d retryAt=%lu uptime=%lu\n",
    context,
    wifiStatusToString(WiFi.status()),
    ip.c_str(),
    WiFi.RSSI(),
    wsConnected ? "connected" : "disconnected",
    countApiError ? "true" : "false",
    onlineCount,
    backendRetryAtMs,
    millis()
  );
#else
  (void)context;
#endif
}

void logWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
#if ENABLE_DIAGNOSTICS
  (void)info;
  String ip = WiFi.localIP().toString();
  Serial.printf(
    "[wifi] event=%d status=%s ip=%s rssi=%ld\n",
    (int)event,
    wifiStatusToString(WiFi.status()),
    ip.c_str(),
    WiFi.RSSI()
  );
#else
  (void)event;
  (void)info;
#endif
}

void logDomainResolution(const char* context) {
#if ENABLE_DIAGNOSTICS
  IPAddress resolvedIp;
  bool resolved = WiFi.hostByName(serverAddress, resolvedIp);
  Serial.printf(
    "[dns] %s host=%s resolved=%s ip=%s apiPort=%d wsPath=%s\n",
    context,
    serverAddress,
    resolved ? "true" : "false",
    resolved ? resolvedIp.toString().c_str() : "n/a",
    serverPort,
    wsPath
  );
#else
  (void)context;
#endif
}

void logHttpRequest(const char* context, const String& url) {
#if ENABLE_DIAGNOSTICS
  Serial.printf(
    "[http] %s url=%s host=%s apiPort=%d wsPath=%s wifi=%s ip=%s rssi=%ld\n",
    context,
    url.c_str(),
    serverAddress,
    serverPort,
    wsPath,
    wifiStatusToString(WiFi.status()),
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI()
  );
#else
  (void)context;
  (void)url;
#endif
}

void logWebSocketTarget() {
#if ENABLE_DIAGNOSTICS
  Serial.printf(
    "[ws] target=wss://%s:%d%s\n",
    serverAddress,
    wsPort,
    wsPath
  );
#endif
}

void runWebSocketHandshakeProbe(bool includePortInHostHeader, const char* originHeader) {
#if ENABLE_DIAGNOSTICS
  WiFiClientSecure probe;
  probe.setInsecure();
  probe.setTimeout(5000);

  String hostHeader = String(serverAddress);
  if (includePortInHostHeader) {
    hostHeader += ":" + String(wsPort);
  }

  Serial.printf("[wsdiag] probe begin hostHeader=%s origin=%s\n",
                hostHeader.c_str(),
                originHeader ? originHeader : "<none>");

  if (!probe.connect(serverAddress, wsPort)) {
    Serial.printf("[wsdiag] probe connect failed host=%s port=%d\n", serverAddress, wsPort);
    return;
  }

  String req = "GET " + String(wsPath) + " HTTP/1.1\r\n";
  req += "Host: " + hostHeader + "\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Sec-WebSocket-Version: 13\r\n";
  req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  if (originHeader && strlen(originHeader) > 0) {
    req += "Origin: ";
    req += originHeader;
    req += "\r\n";
  }
  req += "User-Agent: FitzBell-wsdiag\r\n\r\n";

  probe.print(req);

  unsigned long deadline = millis() + 5000;
  while (!probe.available() && millis() < deadline) {
    delay(10);
  }

  if (!probe.available()) {
    Serial.println("[wsdiag] probe timeout waiting for response");
    probe.stop();
    return;
  }

  String statusLine = probe.readStringUntil('\n');
  statusLine.trim();
  Serial.printf("[wsdiag] status %s\n", statusLine.c_str());

  while (millis() < deadline && probe.connected()) {
    String line = probe.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      Serial.println("[wsdiag] end headers");
      break;
    }
    Serial.printf("[wsdiag] hdr %s\n", line.c_str());
  }

  if (probe.available()) {
    String body = probe.readString();
    body.trim();
    if (body.length() > 0) {
      Serial.printf("[wsdiag] body %s\n", body.c_str());
    }
  }

  probe.stop();
#else
  (void)includePortInHostHeader;
  (void)originHeader;
#endif
}

void runWebSocketHandshakeDiagnostics(const char* reason, bool force = false) {
#if ENABLE_DIAGNOSTICS
  unsigned long now = millis();
  if (!force && (now - lastWsDiagMs) < wsDiagIntervalMs) {
    return;
  }

  lastWsDiagMs = now;
  Serial.printf("[wsdiag] reason=%s\n", reason);
  logDomainResolution("wsdiag");
  runWebSocketHandshakeProbe(true, NULL);
  runWebSocketHandshakeProbe(false, NULL);
  runWebSocketHandshakeProbe(false, "file://");
#else
  (void)reason;
  (void)force;
#endif
}

void updateProgress(int cur, int total);
void setStatus(String msg);
void refreshConnectivityStatus();
void markBackendRetryBackoff();
void updateScreen();

CountFetchResult doFetchOnlineCountBlocking() {
  CountFetchResult result = {false, false, false, -1, 0};

  if (WiFi.status() != WL_CONNECTED) {
    result.wifiDown = true;
    return result;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  String url = "https://" + String(serverAddress) + "/count";
  logHttpRequest("count-fetch", url);
  http.setConnectTimeout(200);
  http.setTimeout(250);

  http.begin(client, url);
  result.httpCode = http.GET();

  if (result.httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    ESP_LOGI(TAG, "Count response: %s", payload.c_str());
#if ENABLE_DIAGNOSTICS
    Serial.printf("[count] url=%s payload=%s\n", url.c_str(), payload.c_str());
#endif

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      result.count = doc["count"] | 0;
      result.success = true;
    } else {
      result.parseError = true;
    }
  } else {
    ESP_LOGW(TAG, "Failed to fetch count, HTTP code: %d", result.httpCode);
#if ENABLE_DIAGNOSTICS
    Serial.printf("[count] url=%s httpCode=%d\n", url.c_str(), result.httpCode);
#endif
  }

  http.end();
  return result;
}

UpdateJobResult doFirmwareUpdateCheckBlocking() {
  WiFiClientSecure client;
  client.setInsecure();

  String updateUrl = "https://" + String(serverAddress) + "/api/firmware/latest";
  logHttpRequest("firmware-update", updateUrl);

  httpUpdate.onProgress(updateProgress);
  client.setTimeout(12000);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(client, updateUrl, CURRENT_VERSION);
  switch (ret) {
    case HTTP_UPDATE_FAILED: {
      String err = httpUpdate.getLastErrorString();
      ESP_LOGE(TAG, "Update failed: %s", err.c_str());
#if ENABLE_DIAGNOSTICS
      Serial.printf("[update] url=%s error=%s\n", updateUrl.c_str(), err.c_str());
#endif
      portENTER_CRITICAL(&networkMux);
      strncpy(updateErrorMsg, err.c_str(), sizeof(updateErrorMsg) - 1);
      updateErrorMsg[sizeof(updateErrorMsg) - 1] = '\0';
      portEXIT_CRITICAL(&networkMux);
      return UpdateJobResult::FAILED;
    }
    case HTTP_UPDATE_NO_UPDATES:
      ESP_LOGI(TAG, "No updates available");
#if ENABLE_DIAGNOSTICS
      Serial.printf("[update] url=%s result=no-updates\n", updateUrl.c_str());
#endif
      return UpdateJobResult::NO_UPDATES;
    case HTTP_UPDATE_OK:
      ESP_LOGI(TAG, "Update installed");
#if ENABLE_DIAGNOSTICS
      Serial.printf("[update] url=%s result=ok\n", updateUrl.c_str());
#endif
      return UpdateJobResult::OK;
  }

  return UpdateJobResult::FAILED;
}

void requestOnlineCountFetch() {
  portENTER_CRITICAL(&networkMux);
  bool busy = countJobPending || countJobRunning;
  if (!busy) {
    countJobPending = true;
  }
  portEXIT_CRITICAL(&networkMux);
}

void requestFirmwareUpdateCheck(bool silent) {
  portENTER_CRITICAL(&networkMux);
  bool busy = updateJobPending || updateJobRunning;
  if (!busy) {
    updateJobPending = true;
    updateRequestSilent = silent;
    updateInProgress = true;
    updatePercent = 0;
  }
  portEXIT_CRITICAL(&networkMux);

  if (!silent) {
    setStatus("Checking Update...");
  }
  ESP_LOGI(TAG, "Checking for firmware updates...");
  logDiagnostics("firmware-check-start");
}

void processNetworkResults() {
  bool hasCountResult = false;
  CountFetchResult localCountResult = {false, false, false, -1, 0};

  bool hasUpdateResult = false;
  UpdateJobResult localUpdateResult = UpdateJobResult::NONE;
  bool localUpdateSilent = true;

  portENTER_CRITICAL(&networkMux);
  if (countResultReady) {
    localCountResult = countResult;
    countResultReady = false;
    hasCountResult = true;
  }

  if (updateResultReady) {
    localUpdateResult = updateResult;
    updateResultReady = false;
    localUpdateSilent = updateRequestSilent;
    hasUpdateResult = true;
  }
  portEXIT_CRITICAL(&networkMux);

  if (hasCountResult) {
    if (localCountResult.wifiDown) {
      if (!countApiError) {
        countApiError = true;
        refreshConnectivityStatus();
      }
      logDiagnostics("count-skip-no-wifi");
    } else if (localCountResult.success) {
      if (onlineCount != localCountResult.count || countApiError) {
        onlineCount = localCountResult.count;
        countApiError = false;
        refreshConnectivityStatus();
      }
    } else {
      if (localCountResult.parseError) {
        ESP_LOGW(TAG, "Failed to parse count JSON");
      }
      if (!countApiError) {
        countApiError = true;
        refreshConnectivityStatus();
      }
      markBackendRetryBackoff();
    }
  }

  if (hasUpdateResult) {
    updateInProgress = false;

    switch (localUpdateResult) {
      case UpdateJobResult::FAILED:
        updatePercent = -1;
        if (!localUpdateSilent) {
          setStatus("Update Failed");
        } else {
          updateScreen();
        }
        break;
      case UpdateJobResult::NO_UPDATES:
        updatePercent = -1;
        if (!localUpdateSilent) {
          setStatus("Up to Date");
        }
        break;
      case UpdateJobResult::OK:
        updatePercent = 100;
        break;
      case UpdateJobResult::NONE:
        updatePercent = -1;
        break;
    }
  }
}

void networkWorkerTask(void* parameter) {
  (void)parameter;

  while (true) {
    bool runCount = false;
    bool runUpdate = false;

    portENTER_CRITICAL(&networkMux);
    if (countJobPending && !countJobRunning) {
      countJobPending = false;
      countJobRunning = true;
      runCount = true;
    } else if (updateJobPending && !updateJobRunning) {
      updateJobPending = false;
      updateJobRunning = true;
      runUpdate = true;
    }
    portEXIT_CRITICAL(&networkMux);

    if (runCount) {
      CountFetchResult result = doFetchOnlineCountBlocking();
      portENTER_CRITICAL(&networkMux);
      countResult = result;
      countResultReady = true;
      countJobRunning = false;
      portEXIT_CRITICAL(&networkMux);
      continue;
    }

    if (runUpdate) {
      UpdateJobResult result = doFirmwareUpdateCheckBlocking();
      portENTER_CRITICAL(&networkMux);
      updateResult = result;
      updateResultReady = true;
      updateJobRunning = false;
      portEXIT_CRITICAL(&networkMux);
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
void fetchOnlineCount();

// ---------- Update Display UI ----------
void updateScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Header
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Fitz-Net Bell");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Main Body
  display.setCursor(0, 20);
  
  if (activeUsers.size() > 0) {
    display.setTextSize(2);
    for (const auto& user : activeUsers) {
      display.println(user);
    }
  } else {
    display.setTextSize(1);
    display.println(statusMessage);
  }

  // Footer (Online Users Count)
  display.drawLine(0, 52, 128, 52, SSD1306_WHITE);
  display.setCursor(0, 54);
  display.setTextSize(1);
  if (countApiError) {
    display.println("Online: API error");
  } else {
    display.print("Online: ");
    display.println(onlineCount);
  }

  display.display();
  displayDirty = false;
  lastDisplayFlushMs = millis();
}

// Defer an OLED redraw to the throttled flush in loop(); use this on the hot
// path (button/WS events) instead of updateScreen() so the blocking I2C push
// never freezes the LED animation.
void requestScreenUpdate() {
  displayDirty = true;
}

void markTxActivity() {
  txActivityUntilMs = millis() + activityWindowMs;
}

void markRxActivity() {
  rxActivityUntilMs = millis() + activityWindowMs;
}

void updateLedState() {
  unsigned long now = millis();
  bool txActive = now < txActivityUntilMs;
  bool rxActive = now < rxActivityUntilMs;

  if (updateInProgress) {
    currentLedMode = LedMode::UPDATING;
  } else if (txActive && rxActive) {
    currentLedMode = LedMode::TX_RX;
  } else if (txActive) {
    currentLedMode = LedMode::TX_ONLY;
  } else if (rxActive) {
    currentLedMode = LedMode::RX_ONLY;
  } else if (buttonPressed) {
    currentLedMode = LedMode::BUTTON_HELD;
  } else if (WiFi.status() != WL_CONNECTED) {
    currentLedMode = LedMode::WIFI_DOWN;
  } else if (isBackendUnreachable()) {
    currentLedMode = LedMode::BACKEND_DOWN;
  } else if (!wsConnected) {
    currentLedMode = LedMode::WS_DOWN;
  } else if (countApiError) {
    currentLedMode = LedMode::COUNT_ERROR;
  } else if (!activeUsers.empty()) {
    currentLedMode = LedMode::ACTIVE_USERS;
  } else {
    currentLedMode = LedMode::READY;
  }
}

void renderLedAnimation() {
  updateLedState();
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  int usersTx = max(1, min((int)activeUsers.size(), NUM_LEDS));
  if (buttonPressed) {
    usersTx = max(1, min(usersTx + 1, NUM_LEDS));
  }

  switch (currentLedMode) {
    case LedMode::BOOTING: {
      for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t wave = triWave8FromPhase((uint8_t)(ledPhase + i * (255 / max(1, NUM_LEDS))));
        leds[i] = CHSV(160, 220, scale8(wave, 140));
      }
      leds[ringWrap(ledPhase / 21)] += CRGB(40, 40, 70);
      break;
    }

    case LedMode::WIFI_DOWN: {
      uint8_t breath = triWave8FromPhase(ledPhase);
      fill_solid(leds, NUM_LEDS, CHSV(160, 255, scale8(breath, 120)));
      leds[ringWrap(ledPhase / 21)] += CRGB::White;
      break;
    }

    case LedMode::BACKEND_DOWN: {
      bool pulse = (ledPhase % 96) < 48;
      fill_solid(leds, NUM_LEDS, pulse ? CHSV(12, 255, 85) : CHSV(12, 255, 18));
      int head = ringWrap(ledPhase / 14);
      leds[head] += CRGB::White;
      leds[ringWrap(head - 1)] += CHSV(18, 255, 80);
      leds[ringWrap(head + 1)] += CHSV(8, 255, 60);
      break;
    }

    case LedMode::WS_DOWN: {
      fill_solid(leds, NUM_LEDS, CHSV(42, 255, 20));
      int head = ringWrap(ledPhase / 18);
      for (int trail = 0; trail < 4; trail++) {
        int idx = ringWrap(head - trail);
        uint8_t v = 170 - trail * 40;
        leds[idx] = CHSV(42, 255, v);
      }
      break;
    }

    case LedMode::UPDATING: {
      int lit = (updatePercent < 0) ? 0 : map(constrain(updatePercent, 0, 100), 0, 100, 0, NUM_LEDS);
      for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = (i < lit) ? CHSV(128, 220, 120) : CHSV(128, 220, 12);
      }
      int spinner = ringWrap(ledPhase / 16);
      leds[spinner] = CRGB::White;
      break;
    }

    case LedMode::TX_ONLY: {
      fill_solid(leds, NUM_LEDS, CHSV(182, 210, 12));
      int head = ringWrap(ledPhase / max(5, 22 - usersTx));
      for (int trail = 0; trail < 5; trail++) {
        int idx = ringWrap(head - trail);
        uint8_t v = 200 - trail * 38;
        leds[idx] += CHSV(182, 255, v);
      }
      leds[0] += CHSV(182, 40, 50);
      break;
    }

    case LedMode::RX_ONLY: {
      fill_solid(leds, NUM_LEDS, CHSV(145, 190, 10));
      int head = ringWrap(NUM_LEDS - 1 - (ledPhase / max(5, 22 - usersTx)));
      for (int trail = 0; trail < 5; trail++) {
        int idx = ringWrap(head + trail);
        uint8_t v = 200 - trail * 38;
        leds[idx] += CHSV(128, 220, v);
      }
      leds[0] += CHSV(145, 40, 45);
      break;
    }

    case LedMode::TX_RX: {
      fill_solid(leds, NUM_LEDS, CHSV(170, 120, 8));
      int speedDiv = max(5, 22 - usersTx);
      int txHead = ringWrap(ledPhase / speedDiv);
      int rxHead = ringWrap(NUM_LEDS - 1 - (ledPhase / speedDiv));

      for (int trail = 0; trail < 4; trail++) {
        uint8_t v = 210 - trail * 45;
        leds[ringWrap(txHead - trail)] += CHSV(182, 255, v);
        leds[ringWrap(rxHead + trail)] += CHSV(128, 255, v);
      }

      for (int i = 0; i < NUM_LEDS; i++) {
        if (leds[i].r > 0 && leds[i].g > 0 && (i == txHead || i == rxHead)) {
          leds[i] += CRGB(60, 60, 60);
        }
      }
      leds[0] += CRGB(20, 20, 25);
      break;
    }

    case LedMode::BUTTON_HELD: {
      for (int i = 0; i < NUM_LEDS; i++) {
        int d = min(i, NUM_LEDS - i);
        uint8_t pulse = triWave8FromPhase((uint8_t)(ledPhase + d * 28));
        leds[i] = CHSV(0, 255, scale8(pulse, 210));
      }
      int txHead = ringWrap(ledPhase / max(5, 22 - usersTx));
      leds[txHead] += CHSV(182, 255, 110);
      leds[0] += CRGB(90, 0, 0);
      break;
    }

    case LedMode::COUNT_ERROR: {
      bool on = (ledPhase % 64) < 32;
      fill_solid(leds, NUM_LEDS, on ? CHSV(8, 255, 160) : CHSV(8, 255, 20));
      leds[0] = on ? CRGB::White : CRGB(20, 20, 20);
      break;
    }

    case LedMode::ACTIVE_USERS: {
      fill_solid(leds, NUM_LEDS, CHSV(96, 255, 14));
      int users = min((int)activeUsers.size(), NUM_LEDS);
      for (int i = 0; i < users; i++) {
        leds[i] = CHSV(96, 220, 140);
      }
      int marker = ringWrap(ledPhase / max(6, 24 - users));
      leds[marker] += CHSV(140, 200, 70);
      break;
    }

    case LedMode::READY: {
      uint8_t breath = triWave8FromPhase(ledPhase);
      fill_solid(leds, NUM_LEDS, CHSV(96, 240, 28 + scale8(breath, 45)));
      leds[0] += CHSV(96, 80, 40);
      break;
    }
  }

  FastLED.show();
}

// ---------- Helper: set status and update screen ----------
void setStatus(String msg) {
  statusMessage = msg;
  updateScreen();
  updateLedState();
}

void logConnectionError(const String& msg) {
  if (lastConnectionError != msg) {
    Serial.println("Connection error: " + msg);
    lastConnectionError = msg;
    logDiagnostics("connection-error");
  }
}

void markBackendRetryBackoff() {
  backendRetryAtMs = millis() + backendRetryDelayMs;
}

void refreshConnectivityStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    logConnectionError("WiFi Disconnected");
    setStatus("WiFi Disconnected");
  } else if (isBackendUnreachable()) {
    logConnectionError("Backend Unreachable");
    setStatus("Backend Unreachable");
  } else if (!wsConnected) {
    logConnectionError("WS Disconnected");
    setStatus("WS Disconnected");
  } else if (countApiError) {
    logConnectionError("API Unavailable");
    setStatus("API Unavailable");
  } else {
    if (lastConnectionError.length() > 0) {
      Serial.println("Connection restored");
      lastConnectionError = "";
    }
    logDiagnostics("connection-restored");
    setStatus("Ready");
  }
}

// ---------- Helper: send JSON over WebSocket (button events) ----------
void sendButtonEvent(const char* eventType) {
  // Build JSON string for ButtonEventDto
  // { "buttonEvent": "PRESSED", "deviceId": "Matt", "firmwareVersion": "v1.0.0" }
  String json = String("{\"buttonEvent\":\"") + eventType +
                "\",\"deviceId\":\"" + userId + 
                "\",\"firmwareVersion\":\"" + CURRENT_VERSION + "\"}";

  ESP_LOGI(TAG, "Sending: %s", json.c_str());

  markTxActivity();
  webSocket.sendTXT(json);

  // Update local list immediately for responsiveness
  bool found = false;
  for (int i = 0; i < activeUsers.size(); i++) {
    if (activeUsers[i] == userId) {
      if (String(eventType) == "RELEASED") {
        activeUsers.erase(activeUsers.begin() + i);
      }
      found = true;
      break;
    }
  }
  if (!found && String(eventType) == "PRESSED") {
    activeUsers.push_back(userId);
  }

  requestScreenUpdate();
}

// ---------- Helper: send a CONNECTED status (no buttonEvent field) ----------
// void sendConnectedStatus() {
//   if (!webSocket.connected()) {
//     return;
//   }

//   String msg = "{\"type\":\"CONNECTED\",\"deviceId\":\"device-1234\"}";

//   Serial.print("Sending (status): ");
//   Serial.println(msg);

//   webSocket.beginMessage(TYPE_TEXT);
//   webSocket.print(msg);
//   webSocket.endMessage();
// }

// ---------- WiFi connect (using WiFiManager) ----------
void connectToWiFi() {
  setStatus("Configuring WiFi...");

  WiFiManager wm;

  // Custom parameter for User ID
  // id, placeholder, default, length
  WiFiManagerParameter custom_userid("userid", "Enter User Name", userId, 40);
  wm.addParameter(&custom_userid);

  // Callback to save params
  wm.setSaveParamsCallback([&]() {
    Serial.println("Saving params");
    strcpy(userId, custom_userid.getValue());
    preferences.begin("app-config", false);
    preferences.putString("userId", userId);
    preferences.end();
  });

  // If you want to reset settings for testing, uncomment:
  // wm.resetSettings();

  // Automatically connect using saved credentials,
  // if connection fails, it starts an access point with the specified name
  bool res = wm.autoConnect("FitzNetBell-Setup"); 

  if(!res) {
    Serial.println("Failed to connect");
    setStatus("WiFi Failed");
    logDiagnostics("wifi-manager-failed");
    // ESP.restart();
  } 
  else {
    // If you get here you have connected to the WiFi
    Serial.println("Connected to WiFi!");
    logDiagnostics("wifi-manager-connected");
    setStatus("WiFi Connected");
    
    // Read updated parameter if it was just saved
    // (The callback handles saving, but we ensure our runtime var is current)
    if (strlen(custom_userid.getValue()) > 0) {
       strcpy(userId, custom_userid.getValue());
    }
  }
}

// ---------- WebSocket Event Handler ----------
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
          if (wsConnected) {
            ESP_LOGW(TAG, "WS Disconnected");
          }
          wsConnected = false;
          logDiagnostics("ws-disconnected");
          runWebSocketHandshakeDiagnostics("event-disconnected");
            refreshConnectivityStatus();
            break;
        case WStype_CONNECTED:
          if (!wsConnected) {
            ESP_LOGI(TAG, "WS Connected");
          }
          wsConnected = true;
          logDiagnostics("ws-connected");
            refreshConnectivityStatus();
            break;
        case WStype_ERROR:
#if ENABLE_DIAGNOSTICS
          Serial.printf("[ws] error len=%u payload=%s\n", (unsigned)length, (length > 0 && payload) ? (const char*)payload : "<none>");
#endif
            logDiagnostics("ws-error");
          runWebSocketHandshakeDiagnostics("event-error");
            break;
        case WStype_TEXT:
#if ENABLE_DIAGNOSTICS
            Serial.printf("Received: %s\n", payload);
#endif
          markRxActivity();
            
            // Parse JSON
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (!error) {
              const char* evt = doc["buttonEvent"];
              const char* user = doc["userId"];
              const char* device = doc["deviceId"];

              String displayName = (user) ? String(user) : ((device) ? String(device) : "Unknown");

              if (evt) {
                if (strcmp(evt, "PRESSED") == 0) {
                   bool exists = false;
                   for(const auto& u : activeUsers) { if(u == displayName) exists = true; }
                   if(!exists) activeUsers.push_back(displayName);
                } else if (strcmp(evt, "RELEASED") == 0) {
                   for (int i = 0; i < activeUsers.size(); i++) {
                     if (activeUsers[i] == displayName) {
                       activeUsers.erase(activeUsers.begin() + i);
                       break;
                     }
                   }
                }
              }
            }
            requestScreenUpdate();
            break;
    }
}

// ---------- Firmware Update Logic ----------
void updateProgress(int cur, int total) {
  updateInProgress = true;
  if (total > 0) {
    updatePercent = (cur * 100) / total;
  }
}

void checkFirmwareUpdate(bool silent) {
  requestFirmwareUpdateCheck(silent);
}

// ---------- Arduino setup ----------
void setup() {
  Serial.begin(115200);
#if ENABLE_DIAGNOSTICS
  Serial.setDebugOutput(true);
  esp_log_level_set("*", ESP_LOG_VERBOSE);
  esp_log_level_set(TAG, ESP_LOG_VERBOSE);
#else
  Serial.setDebugOutput(false);
  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set(TAG, ESP_LOG_INFO);
#endif
  
  // Give the serial monitor a moment to hook up
  delay(1000); 
  Serial.println("\n\n=====================================");
  Serial.println("FitzBell Booting...");
  Serial.println("Firmware Version: " CURRENT_VERSION);
  Serial.println("=====================================\n");

  // LED Init
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  // Startup color
  setLedColor(CRGB::Blue);
  currentLedMode = LedMode::BOOTING;

  // I2C Scanner
  Wire.begin();
  Wire.setClock(400000); // 400kHz: ~4x faster OLED pushes than the 100kHz default
  Serial.println("Scanning for I2C devices...");
  int nDevices = 0;
  for(byte address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address<16) Serial.print("0");
      Serial.println(address,HEX);
      nDevices++;
    }
  }
  if (nDevices == 0) Serial.println("No I2C devices found\nCheck wiring: SDA->21, SCL->22");
  else Serial.println("I2C Scan done");

  // OLED init
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  // Try 0x3C first, if that fails, you might need 0x3D
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 allocation failed"));
    // for(;;); // Don't loop forever, let it try to run anyway
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Boot Splash
  display.setCursor(10, 10);
  display.println("Fitz-Net Bell");
  display.setCursor(10, 30);
  display.println("Initializing...");
  display.display();

  // Load saved user ID
  preferences.begin("app-config", true); // Read-only mode
  String savedId = preferences.getString("userId", "Guest");
  savedId.toCharArray(userId, 40);
  preferences.end();

  logDomainResolution("boot");
  logWebSocketTarget();

#if ENABLE_DIAGNOSTICS
  WiFi.onEvent(logWiFiEvent);
#endif

  pinMode(BUTTON_PIN, INPUT_PULLUP);  // button to GND, internal pull-up

  xTaskCreatePinnedToCore(
    networkWorkerTask,
    "networkWorker",
    8192,
    nullptr,
    1,
    &networkTaskHandle,
    0
  );

  connectToWiFi();
  checkFirmwareUpdate(false);
  
  // Init WebSocket
  webSocket.setExtraHeaders("");
  webSocket.beginSSL(serverAddress, wsPort, wsPath, "", "");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);
  logDiagnostics("websocket-configured");
  runWebSocketHandshakeDiagnostics("setup", true);

  fetchOnlineCount();
  refreshConnectivityStatus();
  updateLedState();
}

// ---------- Fetch Online Users Count ----------
void fetchOnlineCount() {
  requestOnlineCountFetch();
}

// ---------- Arduino loop ----------
void loop() {
  webSocket.loop();
  processNetworkResults();

  unsigned long now = millis();
  while (now - ledLastFrameMs >= ledFrameIntervalMs) {
    ledLastFrameMs += ledFrameIntervalMs;
    ledPhase += 2;
    renderLedAnimation();
    now = millis();
  }

  // Flush any pending OLED redraw, throttled so the blocking I2C push can't
  // starve the LED frame loop above.
  if (displayDirty && (now - lastDisplayFlushMs) >= displayMinIntervalMs) {
    updateScreen();
    now = millis();
  }

  // Check for updates periodically
  if (now - lastUpdateCheck >= updateInterval && now >= backendRetryAtMs) {
    if (!buttonPressed && now >= txActivityUntilMs && now >= rxActivityUntilMs) {
      lastUpdateCheck = now;
      checkFirmwareUpdate(true);
    }
  }

  // Poll online count periodically
  if (now - lastCountCheck >= countInterval && now >= backendRetryAtMs) {
    lastCountCheck = now;
    fetchOnlineCount();
  }

  // Read button state (active LOW)
  int state = digitalRead(BUTTON_PIN);

  if (state == LOW && !buttonPressed) {
    // Transition: not pressed -> pressed
    buttonPressed = true;
    Serial.print("Button Pressed by ");
    Serial.println(userId);
    sendButtonEvent("PRESSED");
    updateLedState();
  } else if (state == HIGH && buttonPressed) {
    // Transition: pressed -> released
    buttonPressed = false;
    Serial.print("Button Released by ");
    Serial.println(userId);
    sendButtonEvent("RELEASED");
    updateLedState();
  }

  yield();
}

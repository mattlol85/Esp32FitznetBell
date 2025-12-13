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
#include <FastLED.h>

#define CURRENT_VERSION "v0.5.0"

static const char* TAG = "FitzBell";

// -------- LED Strip --------
#define LED_PIN     5
#define NUM_LEDS    3
#define BRIGHTNESS  64
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
CRGB leds[NUM_LEDS];

// -------- LED Helper --------
void setLedColor(CRGB color) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
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
const char serverAddress[] = "192.168.1.164";  // Your server IP / hostname
const int  serverPort      = 8080;             // Your server port
const char wsPath[]        = "/ws";            // WebSocket path

// -------- Hardware pin for the button --------
#define BUTTON_PIN 13   // GPIO pin for the button (to GND with INPUT_PULLUP)

// WebSocket client
WebSocketsClient webSocket;

// Track button state to prevent duplicate messages
bool buttonPressed = false;

// Update Scheduler
unsigned long lastUpdateCheck = 0;
const unsigned long updateInterval = 30000; // 30 seconds

// Count Polling
unsigned long lastCountCheck = 0;
const unsigned long countInterval = 10000; // 10 seconds
int onlineCount = 0;
bool countApiError = false;

// Global display state
String statusMessage = "Booting...";
std::vector<String> activeUsers;

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
}

// ---------- Helper: set status and update screen ----------
void setStatus(String msg) {
  statusMessage = msg;
  updateScreen();
}

// ---------- Helper: send JSON over WebSocket (button events) ----------
void sendButtonEvent(const char* eventType) {
  // Build JSON string for ButtonEventDto
  // { "buttonEvent": "PRESSED", "deviceId": "Matt", "firmwareVersion": "v1.0.0" }
  String json = String("{\"buttonEvent\":\"") + eventType +
                "\",\"deviceId\":\"" + userId + 
                "\",\"firmwareVersion\":\"" + CURRENT_VERSION + "\"}";

  ESP_LOGI(TAG, "Sending: %s", json.c_str());

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

  updateScreen();
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
    // ESP.restart();
  } 
  else {
    // If you get here you have connected to the WiFi
    Serial.println("Connected to WiFi!");
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
            ESP_LOGW(TAG, "WS Disconnected");
            setStatus("WS Disconnected");
            break;
        case WStype_CONNECTED:
            ESP_LOGI(TAG, "WS Connected");
            setStatus("Ready");
            break;
        case WStype_TEXT:
            Serial.printf("Received: %s\n", payload);
            
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
            updateScreen();
            break;
    }
}

// ---------- Firmware Update Logic ----------
void updateProgress(int cur, int total) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Firmware Update");
  
  display.setCursor(0, 20);
  display.println("Downloading...");
  
  int percent = (cur * 100) / total;
  display.setCursor(0, 35);
  display.print(percent);
  display.println("%");
  
  // Progress bar
  display.drawRect(0, 50, 128, 10, SSD1306_WHITE);
  display.fillRect(2, 52, map(percent, 0, 100, 0, 124), 6, SSD1306_WHITE);
  
  display.display();
}

void checkFirmwareUpdate(bool silent) {
  if (!silent) setStatus("Checking Update...");
  ESP_LOGI(TAG, "Checking for firmware updates...");

  WiFiClient client;
  
  // Build URL: http://192.168.1.164:8080/api/firmware/latest
  String updateUrl = "http://" + String(serverAddress) + ":" + String(serverPort) + "/api/firmware/latest";

  // Register callback for progress bar
  httpUpdate.onProgress(updateProgress);
  
  // Increase timeout for large files (default is often too short)
  client.setTimeout(12000); 

  // Check and update
  // This sends 'x-ESP32-version: <CURRENT_VERSION>' header
  // We also set followRedirects to true just in case
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  t_httpUpdate_return ret = httpUpdate.update(client, updateUrl, CURRENT_VERSION);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      ESP_LOGE(TAG, "Update failed: %s", httpUpdate.getLastErrorString().c_str());
      if (!silent) {
        setStatus("Update Failed");
        delay(2000);
      } else {
        // Restore screen in case progress bar was shown
        updateScreen();
      }
      break;

    case HTTP_UPDATE_NO_UPDATES:
      ESP_LOGI(TAG, "No updates available");
      if (!silent) {
        setStatus("Up to Date");
        delay(1000);
      }
      break;

    case HTTP_UPDATE_OK:
      ESP_LOGI(TAG, "Update installed");
      // Device will restart automatically
      break;
  }
}

// ---------- Arduino setup ----------
void setup() {
  Serial.begin(115200);
  
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

  // I2C Scanner
  Wire.begin();
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

  pinMode(BUTTON_PIN, INPUT_PULLUP);  // button to GND, internal pull-up

  connectToWiFi();
  checkFirmwareUpdate(false);
  
  // Init WebSocket
  webSocket.begin(serverAddress, serverPort, wsPath);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

// ---------- Fetch Online Users Count ----------
void fetchOnlineCount() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!countApiError) {
      countApiError = true;
      updateScreen();
    }
    return;
  }

  HTTPClient http;
  String url = "http://" + String(serverAddress) + ":" + String(serverPort) + "/count";
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    ESP_LOGI(TAG, "Count response: %s", payload.c_str());
    
    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      int count = doc["count"];
      if (onlineCount != count || countApiError) {
        onlineCount = count;
        countApiError = false;
        updateScreen();
      }
    } else {
      ESP_LOGW(TAG, "Failed to parse count JSON");
      if (!countApiError) {
        countApiError = true;
        updateScreen();
      }
    }
  } else {
    ESP_LOGW(TAG, "Failed to fetch count, HTTP code: %d", httpCode);
    if (!countApiError) {
      countApiError = true;
      updateScreen();
    }
  }
  
  http.end();
}

// ---------- Arduino loop ----------
void loop() {
  webSocket.loop();

  // Check for updates periodically
  if (millis() - lastUpdateCheck >= updateInterval) {
    lastUpdateCheck = millis();
    checkFirmwareUpdate(true);
  }

  // Poll online count periodically
  if (millis() - lastCountCheck >= countInterval) {
    lastCountCheck = millis();
    fetchOnlineCount();
  }

  // Poll online count periodically
  if (millis() - lastCountCheck >= countInterval) {
    lastCountCheck = millis();
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
  } else if (state == HIGH && buttonPressed) {
    // Transition: pressed -> released
    buttonPressed = false;
    Serial.print("Button Released by ");
    Serial.println(userId);
    sendButtonEvent("RELEASED");
  }

  delay(50);  
}

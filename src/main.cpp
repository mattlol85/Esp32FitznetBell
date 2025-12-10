#include <WiFi.h> 
#include <ArduinoHttpClient.h> 
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <vector>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Preferences.h>
#include "esp_log.h"

static const char* TAG = "FitzBell";

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

// Underlying TCP client
WiFiClient wifiClient;

// WebSocket client from ArduinoHttpClient
WebSocketClient webSocket(wifiClient, serverAddress, serverPort);

// Track button state to prevent duplicate messages
bool buttonPressed = false;

// Global display state
String statusMessage = "Booting...";
String lastWsMessage = "";
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

  // Footer (Incoming WS message)
  if (lastWsMessage.length() > 0) {
      display.drawLine(0, 52, 128, 52, SSD1306_WHITE);
      display.setCursor(0, 54);
      display.setTextSize(1);
      display.println(lastWsMessage);
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
  if (!webSocket.connected()) {
    ESP_LOGW(TAG, "Not sending, WebSocket not connected");
    setStatus("WS Disconnected");
    return;
  }

  // Build JSON string for ButtonEventDto
  // { "buttonEvent": "PRESSED", "deviceId": "Matt" }
  String json = String("{\"buttonEvent\":\"") + eventType +
                "\",\"deviceId\":\"" + userId + "\"}";

  ESP_LOGI(TAG, "Sending: %s", json.c_str());

  webSocket.beginMessage(TYPE_TEXT);  // text frame
  webSocket.print(json);
  webSocket.endMessage();

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

// ---------- WebSocket connect ----------
void connectToWebSocket() {
  ESP_LOGI(TAG, "Connecting to WebSocket...");
  setStatus("WS Connecting...");

  // Perform WebSocket handshake
  webSocket.begin(wsPath);  // can pass path here, e.g. "/ws"

  if (webSocket.connected()) {
    ESP_LOGI(TAG, "WebSocket connected!");
    setStatus("Ready");
    // ✅ status message that won't break enum deserialization
    // sendConnectedStatus();
  } else {
    ESP_LOGE(TAG, "WebSocket connection failed");
    setStatus("WS Failed");
  }
}

// ---------- Handle incoming messages ----------
void handleIncomingMessages() {
  // parseMessage() checks if there's a complete frame available
  int messageSize = webSocket.parseMessage();
  if (messageSize > 0) {
    Serial.print("Received message: ");
    String msg;

    while (webSocket.available()) {
      char c = webSocket.read();
      Serial.print(c);
      msg += c;
    }
    Serial.println();

    // Parse JSON
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, msg);

    if (!error) {
      const char* evt = doc["buttonEvent"];
      const char* user = doc["userId"]; // Assuming server sends userId
      const char* device = doc["deviceId"];

      // Fallback if no userId
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
        lastWsMessage = displayName + " " + evt;
      } else {
        // Generic message
        lastWsMessage = "Msg Recv";
      }
    } else {
      lastWsMessage = "Parse Error";
    }
    updateScreen();
  }
}

// ---------- Arduino setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);

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
  connectToWebSocket();
}

// ---------- Arduino loop ----------
void loop() {
  // Reconnect WebSocket if needed
  if (!webSocket.connected()) {
    connectToWebSocket();
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

  // Check for any incoming messages
  handleIncomingMessages();

  delay(50);  // simple debounce + loop pacing
}

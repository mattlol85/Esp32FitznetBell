# Esp32FitznetBell

A smart, connected bell/button for the **Fitz-Net** platform, built for the ESP32 using PlatformIO. The device connects over a secure WebSocket to the **GamerBell** relay to broadcast button press/release events, mirrors who else is pressing their button on a local OLED screen and a WS2812B LED ring, and keeps itself current with **over-the-air (OTA) firmware updates**.

It is one of five sibling repos in the Fitz-Net platform. It speaks the same WebSocket event schema as GamerBell, and firmware is delivered to devices via GamerBell's `GET /api/firmware/latest`, which serves `.bin` artifacts published to this repo's GitHub Releases.

## Features

- **Real-time communication** — Connects to the GamerBell relay over a secure WebSocket (WSS) to send and receive button events instantly.
- **OTA firmware updates** — On boot and once per minute (while idle), the device checks `GET /api/firmware/latest` and silently flashes itself if a newer build is available. No USB needed after the first flash.
- **WS2812B LED ring** — A 12-LED ring shows rich animated feedback for every state: booting, Wi-Fi down, backend/WebSocket down, updating (progress bar), TX/RX traffic, button held, active users, and idle "ready" breathing.
- **OLED display** — Shows device status, connection state, the names of users currently holding their button, and a live count of online devices.
- **Online count** — Polls `GET /count` every 10 seconds and displays how many devices are connected to the relay.
- **Non-blocking networking** — HTTP count polling and OTA update checks run on a dedicated FreeRTOS task pinned to core 0, so the UI and LED animations stay smooth.
- **Easy setup** — A Wi-Fi captive portal (WiFiManager) handles Wi-Fi credentials and the device's user name on first boot; both are persisted to NVS.
- **Optional diagnostics** — A compile-time flag enables verbose Wi-Fi / DNS / HTTP / WebSocket-handshake logging for troubleshooting.

## Hardware Requirements

- **ESP32 development board** (e.g., ESP32 DevKit V1)
- **0.96" OLED display** (SSD1306 driver, I²C interface)
- **WS2812B LED ring / strip** (12 LEDs)
- **Push button**
- Jumper wires and a breadboard

### Pinout

| Component | ESP32 Pin | Notes |
|---|---|---|
| **Button** | GPIO 13 | One side to GPIO 13, the other to GND. Uses internal pull-up (active LOW). |
| **LED ring (DIN)** | GPIO 5 | WS2812B data in. 12 LEDs, GRB color order. |
| **OLED SDA** | GPIO 21 | Default I²C SDA |
| **OLED SCL** | GPIO 22 | Default I²C SCL |
| **OLED VCC** | 3.3V | |
| **OLED GND** | GND | |

> The LED ring should be powered appropriately for its size; 12 LEDs at the default brightness (64/255) is fine off the ESP32's 5V/VIN rail in most cases.

## Software & Libraries

This project is built with **PlatformIO** (VS Code extension recommended).

### Dependencies

Declared in `platformio.ini` and installed automatically:

- `links2004/WebSockets` — WebSocket (WSS) client
- `adafruit/Adafruit SSD1306` — OLED display driver
- `adafruit/Adafruit GFX Library` — graphics core
- `bblanchon/ArduinoJson` — JSON parsing
- `tzapu/WiFiManager` — Wi-Fi configuration captive portal
- `fastled/FastLED@3.6.0` — WS2812B LED ring animations

`HTTPClient`, `HTTPUpdate`, `WiFiClientSecure`, and `Preferences` come from the Arduino-ESP32 framework.

## Configuration

Wi-Fi credentials and the device user name are configured at runtime via the captive portal (see [First-Time Setup](#first-time-setup)) — they are **not** hardcoded.

The relay endpoint is defined near the top of `src/main.cpp`. By default it points at the production GamerBell host over TLS:

```cpp
// -------- WebSocket server details --------
const char serverAddress[] = "gamerbell.fitznet.doomdns.org";  // GamerBell host
const int  serverPort      = 443;     // HTTPS/API port (used for /count and OTA)
const int  wsPort          = 443;     // WSS port
const char wsPath[]        = "/ws";   // WebSocket path
```

The device talks to three endpoints on this host:

| Endpoint | Purpose |
|---|---|
| `wss://<host>:443/ws` | WebSocket for button press/release events |
| `https://<host>/count` | Online device count (polled every 10s) |
| `https://<host>/api/firmware/latest` | OTA firmware image |

> TLS certificates are accepted with `setInsecure()` (no cert pinning), which is appropriate for this personal-platform setup.

### Build flags

`platformio.ini` exposes a diagnostics toggle:

```ini
build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DENABLE_DIAGNOSTICS=0
    ; Set to 1 for verbose websocket/http/DNS diagnostics
```

Set `-DENABLE_DIAGNOSTICS=1` to enable detailed serial logging, including a raw WebSocket handshake probe — useful when debugging relay connectivity.

## How to Run

1. **Install VS Code** and the **PlatformIO** extension.
2. **Clone** or open this project folder.
3. **Connect** your ESP32 to your computer via USB.
4. **Build** the project (checkmark icon in the PlatformIO toolbar).
5. **Upload** the firmware (arrow icon).
6. **Monitor** serial output (plug icon) to see boot logs — baud rate **115200**.

On boot the firmware runs an I²C scanner and prints any detected device addresses, which helps confirm OLED wiring (expects `0x3C`).

## First-Time Setup

1. **Power on** the ESP32.
2. On your phone or computer, look for a Wi-Fi network named **`FitzNetBell-Setup`**.
3. **Connect** to it — a configuration page should open automatically (captive portal). If not, browse to `192.168.4.1`.
4. Click **Configure WiFi**.
5. Select your home Wi-Fi network and enter the password.
6. Enter your desired **User Name** in the custom field (this becomes the device's `deviceId`).
7. Click **Save**. The device connects and stores both Wi-Fi credentials and the user name in NVS.

To re-run setup, uncomment `wm.resetSettings();` in `connectToWiFi()` (or clear NVS) and re-flash.

## Usage

1. Once configured, the screen shows **"Ready"** and the LED ring breathes green when connected.
2. **Press and hold** the button to send a `PRESSED` event — your name appears on the screen and the ring animates.
3. **Release** to send a `RELEASED` event.
4. If others connected to the same relay press their buttons, their names appear on your screen and contribute to the active-user animation.
5. The footer shows the live **Online** device count; it reads "API error" if the count endpoint is unreachable.

When the exact user name **Joe** is holding the button, the OLED shows
**"Joe would like to game"** beside a small animated emergency beacon. This
works for local and received button events. Releasing the button removes
the message and beacon through the normal screen update flow; it does not
latch or linger. The SSD1306 is monochrome, so the beacon is white, not red.
While Joe is active, up to four other active names appear below the message.
Other users retain the existing name display, and LED ring behavior is unchanged.

### LED ring states

| State | Behavior |
|---|---|
| Booting | Blue/purple wave sweep |
| Wi-Fi down | Blue breathing with a roving white pixel |
| Backend unreachable | Orange pulse with a comet head |
| WebSocket down | Amber trailing comet |
| Updating | Magenta progress bar + white spinner (tracks OTA %) |
| TX / RX / TX+RX | Blue (send) and green (receive) comets |
| Button held | Red expanding pulse |
| Count error | Red blink |
| Active users | Green segment per active user |
| Ready (idle) | Gentle green breathing |

## WebSocket Event Schema

The device sends a `ButtonEventDto`-shaped JSON message on each press/release:

```json
{ "buttonEvent": "PRESSED", "deviceId": "<user name>", "firmwareVersion": "v0.10.1" }
```

`buttonEvent` is `"PRESSED"` or `"RELEASED"`. Incoming messages are parsed the same way (`buttonEvent` plus `userId`/`deviceId`) to maintain the active-user list. Keep this payload aligned with GamerBell's `ButtonEventDto` and the website's `WebSocketButton.jsx`.

## CI / Releases

Two GitHub Actions workflows live in `.github/workflows/`:

- **`build.yml`** — Runs `pio run` on every push and PR to `main` and uploads `firmware.bin` as a build artifact.
- **`release.yml`** — Manual (`workflow_dispatch`) release. Choose `patch` / `minor` / `major`; the workflow computes the next semver tag, rewrites `#define CURRENT_VERSION` in `src/main.cpp`, commits the bump, tags it, builds, and publishes a GitHub Release with `firmware.bin` attached.

The published `firmware.bin` is what GamerBell serves to devices via `GET /api/firmware/latest`. The device compares its compiled `CURRENT_VERSION` against the served build and self-flashes when a newer release is available — so cutting a release is all it takes to push an update to every bell.

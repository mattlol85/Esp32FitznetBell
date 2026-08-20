# Esp32FitznetBell — Agentic Development Guide

This repo contains the **ESP32 firmware** for the Fitz-Net bell button. It is one of five sibling repos in the Fitz-Net platform.

---

## Platform Overview

| Repo | Path | Role | Stack |
|---|---|---|---|
| **Fitz-Net** | `../Fitz-Net` | Orchestration hub, GitHub Actions, agentic sandbox | Markdown, YAML |
| **fitz-net-api** | `../fitz-net-api` | REST API backend | Java 21, Spring Boot 3.4, Gradle, MongoDB |
| **fitz-net-website** | `../fitz-net-website` | React SPA frontend | React 19, Vite, React Router v7 |
| **GamerBell** | `../GamerBell` | WebSocket relay + OTA firmware server | Java 21, Spring Boot 3.4, Gradle |
| **Esp32FitznetBell** (this repo) | `../Esp32FitznetBell` | ESP32 bell button firmware | C++, PlatformIO, Arduino |

---

## This Repo's Role

The firmware runs on an ESP32 DevKit and does five things:

1. Connects to the **GamerBell** WebSocket relay (`wss://<host>:443/ws`) and sends `PRESSED`/`RELEASED` events when the physical button is held or released.
2. Receives button events from other devices and displays active user names on an OLED screen.
3. Polls `GET /count` every 10 s and shows how many devices are online.
4. Checks `GET /api/firmware/latest` on boot and every 60 s; self-flashes over the air (OTA) when GamerBell serves a newer binary.
5. Drives a 12-LED WS2812B ring with animated states for every connectivity and activity condition.
6. Reports known failure points (count-fetch errors, WebSocket disconnects/errors, OTA failures) to GamerBell's `POST /api/devices/log` so they're visible in Loki/Grafana instead of only on the local Serial console.

All networking (HTTP count fetch, OTA check, device error log POSTs) runs on a dedicated FreeRTOS task pinned to core 0 via `networkWorkerTask`. The UI, LED rendering, WebSocket loop, and button polling stay on core 1 in `loop()`.

---

## File Map

```
src/main.cpp        — entire firmware (single-file; ~1 200 lines)
platformio.ini      — board, framework, build flags, lib_deps
lib/                — local libraries (currently empty; use lib_deps instead)
include/            — shared headers (currently empty)
test/               — placeholder for Unity unit tests
.github/workflows/
  build.yml         — pio run on every push/PR to main; uploads firmware.bin artifact
  release.yml       — manual workflow_dispatch release (patch/minor/major semver bump)
```

All logic lives in `src/main.cpp`. Do not split it into multiple files without a clear reason — the single-file layout keeps compile times fast and avoids header-include complexity for a project of this size.

---

## Key Concepts

### LedMode state machine

`LedMode` is an `enum class` that captures every device state. `updateLedState()` derives the current mode from boolean flags (`buttonPressed`, `wsConnected`, `countApiError`, `updateInProgress`, etc.) and `renderLedAnimation()` switches on it to drive the FastLED ring. Add new states here — do not inline ad-hoc LED writes elsewhere.

### Network worker / job queue

HTTP and OTA calls block for up to several seconds and must not run on core 1. The pattern is:

1. Caller sets `countJobPending = true` (or `updateJobPending`) inside a `portENTER_CRITICAL` / `portEXIT_CRITICAL` guard.
2. `networkWorkerTask` (core 0) picks up the pending flag, runs the blocking call, writes the result struct, and sets `*ResultReady = true`.
3. `processNetworkResults()` (called every `loop()` iteration on core 1) reads the result under the same critical section and applies it to global state.

When adding new background network calls, follow this same producer/consumer pattern. Never call `HTTPClient` or `HTTPUpdate` from `loop()` directly.

### Device error logging (`queueDeviceLog`)

`queueDeviceLog(level, source, message)` queues a fire-and-forget `POST /api/devices/log` to GamerBell, sent by `networkWorkerTask` via `doSendDeviceLogBlocking()`. Unlike the count/update jobs there's no result struct — nothing reads a response, since this is best-effort telemetry, not app state. If a log send is already pending/in-flight, `queueDeviceLog` silently drops the new one rather than queuing, so a repeatedly-failing condition can't back up the job queue behind button/count/OTA jobs. Call it at the point a failure *transitions* into existence (e.g. `if (!countApiError) { ... queueDeviceLog(...); }`), not on every retry, to avoid log spam.

### OLED throttling

`updateScreen()` is a blocking I2C push (~20–80 ms). Use `requestScreenUpdate()` on the hot path (button/WebSocket events) to set `displayDirty = true`; the flush is throttled to ~20 Hz in `loop()`. Only call `updateScreen()` directly for one-shot status messages that must appear immediately (e.g., `setStatus("Checking Update...")`).

### WebSocket event schema

The device sends and receives `ButtonEventDto`-shaped JSON:

```json
{ "buttonEvent": "PRESSED",  "deviceId": "<userId>", "firmwareVersion": "v0.11.1" }
{ "buttonEvent": "RELEASED", "deviceId": "<userId>", "firmwareVersion": "v0.11.1" }
```

Incoming messages accept either `userId` or `deviceId` as the display name. **This schema must stay aligned with GamerBell's `ButtonEventDto` and `fitz-net-website`'s `WebSocketButton.jsx`.** If you change the field names here, update those two files in the same PR or a coordinated set of PRs.

### OTA versioning

The active firmware version is the `#define CURRENT_VERSION` string at the top of `src/main.cpp` (e.g., `"v0.11.1"`). `httpUpdate.update()` passes this as the `If-None-Match`-equivalent header to GamerBell, which only streams the binary when a newer release exists. **CI owns this value — never bump `CURRENT_VERSION` manually.** The `release.yml` workflow computes the next semver, rewrites the define, commits it, and tags the release automatically.

### Diagnostics flag

`ENABLE_DIAGNOSTICS` (off by default in `platformio.ini`) gates all verbose `Serial.printf` and the WebSocket handshake probe. Toggle it locally for debugging; never commit with it enabled.

---

## Hardware Pinout

| Component | GPIO | Notes |
|---|---|---|
| Button | 13 | Active LOW, `INPUT_PULLUP` |
| LED ring (DIN) | 5 | WS2812B, 12 LEDs, GRB |
| OLED SDA | 21 | I²C, 400 kHz |
| OLED SCL | 22 | I²C, 400 kHz |

---

## Build & Flash

```bash
# Build only (what CI does)
pio run

# Build + flash via USB
pio run --target upload

# Serial monitor (115200 baud)
pio device monitor
```

There is no automated test runner for hardware-dependent code. The `test/` directory exists for Unity unit tests on pure logic; add them there if extracting testable functions.

---

## CI Workflows

### `build.yml`
Triggered on every push and PR to `main`. Runs `pio run` and uploads `firmware.bin` as a build artifact. A failing build blocks merge.

### `release.yml`
Manual `workflow_dispatch`. Input: `patch` / `minor` / `major`.

Steps:
1. Computes the next semver tag from the latest GitHub Release.
2. Rewrites `#define CURRENT_VERSION` in `src/main.cpp`.
3. Commits the version bump and tags it.
4. Builds with `pio run`.
5. Publishes a GitHub Release with `firmware.bin` attached.

The published `firmware.bin` is what GamerBell serves to devices via `GET /api/firmware/latest`. Cutting a release is all it takes to push an OTA update to every bell.

---

## Cross-Repo Considerations

- **GamerBell** — The relay that this firmware connects to. If you change the WebSocket path (`wsPath`), server address constants, or the event schema, update GamerBell in sync. GamerBell also owns `GET /api/firmware/latest`; if you change the OTA URL shape, coordinate with GamerBell.
- **fitz-net-website** — `WebSocketButton.jsx` consumes the same `ButtonEventDto` schema. Keep `buttonEvent`, `deviceId`, and `userId` field names aligned.
- **fitz-net-api / Fitz-Net** — No direct dependency; changes here do not affect them.
- For the full cross-repo workflow, see `../Fitz-Net/Fitz-Net-Agent-Sandbox/AGENT_INSTRUCTIONS.md`.

---

## Commit Convention

```
feat(subject): description
fix(subject): description
chore(subject): description
```

Good subject tokens: `display`, `leds`, `ws`, `ota`, `wifi`, `ci`, `button`.

---

## Common Pitfalls

- **Never call blocking HTTP/OTA from `loop()`** — always go through the job-queue pattern.
- **Don't call `updateScreen()` from WebSocket events** — use `requestScreenUpdate()` to avoid stalling the LED frame loop.
- **Don't commit `ENABLE_DIAGNOSTICS=1`** — it floods serial output and slightly changes timing.
- **Never touch `CURRENT_VERSION`** — CI rewrites and commits it via `release.yml`. Bumping it manually will conflict with the release workflow and may break OTA version comparison.
- **WiFiManager resets** — `wm.resetSettings()` is intentionally commented out. Un-comment only for local testing; never commit it uncommented.
- **TLS is `setInsecure()`** — no cert pinning by design for this personal platform. Don't add pinning without coordinating with GamerBell's certificate rotation.

# CodingLight

An ESP32-C3 desk status light for AI coding agents.

CodingLight turns a small red/yellow/green LED traffic light into an ambient
status display for Codex CLI. It can be controlled over HTTP, USB Serial, or
BLE Nordic UART Service.

## Lamp Language

| Pattern | Meaning |
| --- | --- |
| Steady green | Idle |
| Slow green/yellow/red chase | Agent is thinking, coding, building, or running tools |
| Flashing yellow | Permission or confirmation is needed |
| Flashing red | Error, blocked state, or failure |
| Flashing green for 20 seconds | Task finished, then returns to idle |
| Off | Manually cleared |

## Hardware

Tested board:

- ESP32-C3 Super Mini

LED wiring:

```text
GPIO2 -> Green LED cathode
GPIO3 -> Yellow LED cathode
GPIO4 -> Red LED cathode
LED anodes -> 3.3V through current-limiting resistors
```

The sketch assumes common-anode LEDs:

```text
LED ON  = LOW
LED OFF = HIGH
```

All LED output uses LEDC PWM. Animation code does not use `digitalWrite()`.

## Repository Layout

```text
CodingLight.ino                 Arduino sketch
wifi_secrets.example.h          Example WiFi credentials file
codex-hooks/codinglight_status.py
codex-hooks/hooks.example.json
```

`wifi_secrets.h` is intentionally ignored by git.

## Firmware Setup

1. Install Arduino IDE.
2. Install the Espressif ESP32 board package.
3. Open `CodingLight.ino`.
4. Select an ESP32-C3 board profile, such as `ESP32C3 Dev Module`.
5. If the default partition is too small, select a larger app partition. OTA firmware upload is not used, so a `Huge APP` style partition is fine.
6. Upload over USB.

Serial Monitor baud rate:

```text
115200
```

## WiFi Credentials

Copy the example file:

```bash
cp wifi_secrets.example.h wifi_secrets.h
```

Edit `wifi_secrets.h`:

```cpp
#pragma once

static const char WIFI_SSID[] = "your_wifi_name";
static const char WIFI_PASSWORD[] = "your_wifi_password";
```

If `wifi_secrets.h` is missing or the SSID is empty, the device still supports
USB Serial and BLE. HTTP and mDNS require WiFi.

## Control Interfaces

### Serial and BLE Commands

Send one command per line:

```text
PING
INFO
STATE OFF
STATE IDLE
STATE THINKING
STATE CODING
STATE BUILD
STATE SUCCESS
STATE ERROR
STATE WARNING
STATE OTA
BRIGHTNESS 0-255
```

Responses:

```text
PONG
OK
ERR
```

`INFO` returns JSON:

```json
{"state":"IDLE","ip":"192.168.1.50","wifi":true,"ble":true,"brightness":180,"uptime":12345}
```

### BLE

Device name:

```text
CodingLight
```

Nordic UART Service UUIDs:

```text
Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX       6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX       6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

Write commands to RX and subscribe to TX notifications for responses.

### HTTP

When WiFi is connected:

```text
http://codinglight.local/
```

If mDNS is not available on your network, use Serial or BLE `INFO` to get the
device IP.

REST endpoints:

```text
GET  /api/info
POST /api/state
POST /api/brightness
```

Examples:

```bash
curl http://codinglight.local/api/info

curl -X POST http://codinglight.local/api/state \
  -H "Content-Type: application/json" \
  -d '{"state":"CODING"}'

curl -X POST http://codinglight.local/api/brightness \
  -H "Content-Type: application/json" \
  -d '{"brightness":120}'
```

## Codex CLI Hook

The included hook adapter maps Codex lifecycle events to lamp states:

| Codex event | Lamp state |
| --- | --- |
| `SessionStart` | `IDLE` |
| `UserPromptSubmit` | `THINKING` |
| `PreToolUse` | `BUILD` |
| `PostToolUse` | `CODING` |
| `PermissionRequest` | `WARNING` |
| `Stop` | `SUCCESS` |

The hook supports:

| Transport | Notes |
| --- | --- |
| `http` | Recommended when the device is on WiFi |
| `usb` | Uses the USB Serial command protocol |
| `ble` | Uses BLE NUS; requires Python package `bleak` |
| `auto` | Tries HTTP, then USB, then BLE |

Install the example hook config:

```bash
mkdir -p ~/.codex
cp codex-hooks/hooks.example.json ~/.codex/hooks.json
```

Edit `~/.codex/hooks.json`:

- Replace `/absolute/path/to/CodingLight` with this repository path.
- Set `CODINGLIGHT_HOST` to `codinglight.local` or your device IP.

For HTTP:

```bash
export CODINGLIGHT_TRANSPORT=http
export CODINGLIGHT_HOST=codinglight.local
```

For USB Serial:

```bash
export CODINGLIGHT_TRANSPORT=usb
export CODINGLIGHT_SERIAL_PORT=/dev/ttyACM0
export CODINGLIGHT_SERIAL_BAUD=115200
```

For BLE:

```bash
python3 -m pip install bleak
export CODINGLIGHT_TRANSPORT=ble
export CODINGLIGHT_BLE_NAME=CodingLight
```

After changing Codex hooks, restart Codex CLI and run:

```text
/hooks
```

Review and trust the hook before expecting it to run.

## Security Notes

- The HTTP API has no authentication. Use it only on a trusted local network.
- Do not commit `wifi_secrets.h`.
- The hook script intentionally exits successfully if the light is unreachable so Codex work is not blocked by hardware.

## Project Status

This is a small personal hardware project. The firmware and hook protocol are
kept intentionally simple so the device remains easy to adapt to other agents
or status sources.

# ESP32-C3 AI Coding Status Light

Arduino sketch for an ESP32-C3 Super Mini status light with three common-anode LEDs.

## Hardware

Board: ESP32-C3 Super Mini

LED wiring:

```text
GPIO2 -> Green LED cathode
GPIO3 -> Yellow LED cathode
GPIO4 -> Red LED cathode
LED anodes -> 3.3V through appropriate resistors
```

The LEDs are common anode:

```text
LED ON  = LOW
LED OFF = HIGH
```

The sketch uses LEDC PWM, so animations and brightness control do not use `digitalWrite()`.

## Files

```text
CodingLight/
  CodingLight.ino              Main Arduino sketch
  wifi_secrets.example.h       Example WiFi credentials file
  wifi_secrets.h               Your local WiFi credentials, ignored by git
  .gitignore
  README.md
```

## WiFi Setup

Create `wifi_secrets.h` in the same folder as `CodingLight.ino`.

You can copy `wifi_secrets.example.h` and edit it:

```cpp
#pragma once

static const char WIFI_SSID[] = "your_wifi_name";
static const char WIFI_PASSWORD[] = "your_wifi_password";
```

`wifi_secrets.h` is listed in `.gitignore`, so it should not be pushed to GitHub.

If `wifi_secrets.h` is missing or the SSID is empty, the device still runs Serial and BLE normally, but WiFi, mDNS, and HTTP will not be reachable.

## Arduino IDE Setup

1. Install Arduino IDE.
2. Install the ESP32 board package from Espressif.
3. Open `CodingLight/CodingLight.ino`.
4. Select an ESP32-C3 board profile, for example `ESP32C3 Dev Module`.
5. Select the serial port.
6. Use a large enough partition scheme if the default app partition is too small.
   Since this project does not use OTA firmware upload, `Huge APP` is fine.
7. Click Upload.

Recommended serial monitor speed:

```text
115200 baud
```

## Serial Commands

Send one command per line over Serial Monitor.

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

## BLE Usage

BLE is enabled by default.

Device name:

```text
CodingLight
```

Nordic UART Service UUIDs:

```text
Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX:      6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX:      6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

Use a BLE UART app, connect to `CodingLight`, write commands to RX, and subscribe to TX notifications for responses.

## HTTP Usage

When WiFi is connected, open:

```text
http://codinglight.local/
```

If mDNS does not resolve on your network, get the IP from Serial:

```text
INFO
```

Then open:

```text
http://DEVICE_IP/
```

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

## Codex Hook Usage

This repository includes a Codex hook adapter:

```text
codex-hooks/codinglight_status.py
codex-hooks/hooks.example.json
```

The hook maps Codex lifecycle events to lamp states:

```text
SessionStart       -> IDLE
UserPromptSubmit   -> THINKING
PreToolUse         -> BUILD
PostToolUse        -> CODING
PermissionRequest  -> ERROR
Stop               -> WARNING
```

Red alerts are latched. After `PermissionRequest`, later `PreToolUse`,
`PostToolUse`, or `Stop` events keep the lamp in `ERROR` instead of hiding the
problem. The latch is cleared by the next `UserPromptSubmit` or `SessionStart`.

`Stop` maps to `WARNING` so a completed turn remains visible until you look at
Codex or submit the next prompt.

The hook supports three transports:

```text
http  WiFi HTTP API, fastest and recommended
usb   USB Serial, same protocol as Arduino Serial Monitor
ble   BLE Nordic UART Service, requires Python package bleak
auto  Try HTTP, then USB, then BLE
```

Recommended setup for this device:

```bash
mkdir -p ~/.codex
cp codex-hooks/hooks.example.json ~/.codex/hooks.json
```

Then edit `~/.codex/hooks.json` and replace:

```text
/absolute/path/to/CodingLight
```

with the absolute path to this repository.

For your current device IP:

```bash
export CODINGLIGHT_TRANSPORT=auto
export CODINGLIGHT_HOST=172.28.1.170
```

For HTTP only:

```bash
export CODINGLIGHT_TRANSPORT=http
export CODINGLIGHT_HOST=172.28.1.170
```

For USB Serial:

```bash
export CODINGLIGHT_TRANSPORT=usb
export CODINGLIGHT_SERIAL_PORT=/dev/ttyACM0
export CODINGLIGHT_SERIAL_BAUD=115200
```

On Windows, `CODINGLIGHT_SERIAL_PORT` will usually look like:

```text
COM3
```

USB Serial uses the same commands documented above, for example `STATE BUILD`.
If the optional `pyserial` package is installed, the hook uses it. On Linux and
macOS, it can also use a built-in POSIX serial fallback.

For BLE:

```bash
python3 -m pip install bleak
export CODINGLIGHT_TRANSPORT=ble
export CODINGLIGHT_BLE_NAME=CodingLight
```

If you know the BLE address, prefer it over scanning by name:

```bash
export CODINGLIGHT_BLE_ADDRESS=AA:BB:CC:DD:EE:FF
```

After changing Codex hooks, restart Codex CLI and run:

```text
/hooks
```

Review and trust the hook before expecting it to run.

## Animation States

```text
OFF       all LEDs off
IDLE      green steady on
THINKING  green/yellow/red slow cycle
CODING    green/yellow/red slow cycle
BUILD     green/yellow/red slow cycle
SUCCESS   green flashes 3 times, then returns to previous state
ERROR     red flashing and remains urgent
WARNING   yellow flashing and remains visible
OTA       green/yellow/red rotation
```

`OTA` is only a visual state in this version. Firmware updates are done over USB.

## GitHub Notes

Before pushing, check that `wifi_secrets.h` is not staged:

```bash
git status
```

Only commit the example secrets file:

```bash
git add CodingLight.ino wifi_secrets.example.h .gitignore README.md
git commit -m "Add ESP32-C3 coding status light"
```

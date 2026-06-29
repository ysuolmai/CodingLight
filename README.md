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

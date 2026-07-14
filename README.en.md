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

The repository maintains two hardware variants:

| Variant | Firmware | LED pins | Button |
| --- | --- | --- | --- |
| Wired | `firmware/CodingLightWired/CodingLightWired.ino` | GPIO2 green, GPIO3 yellow, GPIO4 red | Hold BOOT (GPIO9) to open WiFi setup |
| Battery | `firmware/CodingLightBattery/CodingLightBattery.ino` | GPIO2 red, GPIO3 yellow, GPIO4 green | GPIO5 cycles states; release after 2–5 seconds to sleep; release after at least 5 seconds for WiFi setup |

Wired variant LED wiring:

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
firmware/CodingLightWired/
  CodingLightWired.ino          Wired variant Arduino sketch
  wifi_secrets.example.h        Wired WiFi example
  ota_secrets.example.h         Wired OTA password example
firmware/CodingLightBattery/
  CodingLightBattery.ino        Battery variant Arduino sketch
  wifi_secrets.example.h        Battery WiFi example
  ota_secrets.example.h         Battery OTA password example
codex-hooks/codinglight_status.py
codex-hooks/hooks.example.json
.github/workflows/build-firmware.yml
```

`wifi_secrets.h` and `ota_secrets.h` are intentionally ignored by git.

## Firmware Setup

1. Install Arduino IDE.
2. Install the Espressif ESP32 board package.
3. Open `firmware/CodingLightWired/CodingLightWired.ino` for wired hardware, or `firmware/CodingLightBattery/CodingLightBattery.ino` for battery hardware.
4. Select an ESP32-C3 board profile, such as `ESP32C3 Dev Module`.
5. Select the `Minimal SPIFFS` partition scheme so the flash has two OTA application slots.
6. Upload over USB.

If the device currently uses `Huge APP` or another non-OTA partition table, the first migration must be flashed over USB. OTA works only after the new firmware and partition table are installed.

Serial Monitor baud rate:

```text
115200
```

## Cloud Build and Release

The repository includes a GitHub Actions firmware build workflow:

```text
.github/workflows/build-firmware.yml
```

It runs when:

- A commit is pushed to the `main` branch.
- `Build Firmware` is started manually from the GitHub Actions page.

Build outputs are uploaded to:

- The workflow run artifact.
- The `continuous` prerelease on GitHub Releases.

The `continuous` release is replaced by each successful build. It contains
separate `wired` and `battery` archives plus `SHA256SUMS.txt`. Do not flash one
variant onto the other because their LED pin assignments differ.

Cloud builds contain neither local WiFi nor OTA credentials. Both variants can
be provisioned through the `CodingLight-Setup` captive portal.

If release updates fail, check this repository setting:

```text
Settings -> Actions -> General -> Workflow permissions -> Read and write permissions
```

## WiFi Setup

Both variants use the same captive portal flow.

On first boot, if no WiFi credentials are configured, CodingLight starts an
open access point:

```text
CodingLight-Setup
```

Connect to it. A captive portal should open automatically on most phones and
laptops. If it does not, open:

```text
http://192.168.4.1/
```

Submit your SSID and password there. The credentials are saved in ESP32 NVS and
survive reboot.

To provision a different network later:

- On wired hardware, hold the ESP32-C3 Super Mini `BOOT` button for about 2.5 seconds.
- On battery hardware, hold GPIO5 for at least 5 seconds and release when the light changes from the 2-second off indication to yellow.

This re-enables the `CodingLight-Setup` AP. Opening the setup portal does not
erase existing credentials; they are replaced only after you submit a new
SSID. Releasing the battery button after 2–5 seconds still enters deep sleep.

If the battery variant is asleep, the first GPIO5 press only wakes it. Release
the button, wait for startup, then hold it again for at least 5 seconds.

You can also provide build-time fallback credentials. Copy the example file:

```bash
cp firmware/CodingLightWired/wifi_secrets.example.h firmware/CodingLightWired/wifi_secrets.h
```

Edit `wifi_secrets.h`:

```cpp
#pragma once

static const char WIFI_SSID[] = "your_wifi_name";
static const char WIFI_PASSWORD[] = "your_wifi_password";
```

`wifi_secrets.h` is ignored by git. Credentials saved from the captive portal
take priority over `wifi_secrets.h`.

If `wifi_secrets.h` is missing or the SSID is empty, the device still supports
USB Serial, BLE, and the setup AP. The HTTP control page is available through
the setup AP and through your LAN after WiFi connects.

The battery variant can also use a build-time `wifi_secrets.h` inside
`firmware/CodingLightBattery/`. Without saved or build-time credentials, it
starts the setup AP while USB Serial and BLE remain available.

## OTA Updates

Both firmware variants start ArduinoOTA after WiFi connects:

- Wired hostname: `codinglight.local`
- Battery hostname: `codinglight-battery.local`
- The lamp enters `OTA` during an update and `ERROR` if the update fails.
- Copy the relevant `ota_secrets.example.h` to `ota_secrets.h` and set a strong password.
- OTA updates only the application. Partition-table or bootloader changes, and recovery from a wrong variant, require USB flashing.

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
{"state":"IDLE","ip":"192.168.1.50","wifi":true,"ap":false,"ap_ip":"0.0.0.0","ble":true,"brightness":180,"uptime":12345}
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

When the setup AP is active:

```text
http://192.168.4.1/
```

shows the WiFi setup page. The normal light control page remains available at:

```text
http://192.168.4.1/control
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

## Codex CLI Hook

The included hook adapter lives in:

```text
codex-hooks/codinglight_status.py
codex-hooks/install.py
codex-hooks/hooks.example.json
```

It maps Codex lifecycle events to lamp states:

| Codex event | Lamp state |
| --- | --- |
| `SessionStart` | `IDLE` |
| `UserPromptSubmit` | `THINKING` |
| `PreToolUse` | `BUILD` |
| `PostToolUse` | `CODING` |
| `PermissionRequest` | `WARNING` |
| `Stop` | `SUCCESS` |

If Codex stops a turn because of an API, upstream, or third-party model
provider error, the hook watches local Codex session logs and switches the
light to `ERROR`.

The hook supports:

| Transport | Notes |
| --- | --- |
| `http` | Recommended when the device is on WiFi |
| `usb` | Uses the USB Serial command protocol and can auto-detect common USB serial ports |
| `ble` | Uses BLE NUS; requires Python package `bleak`; the installer can scan and save the device address |
| `auto` | Tries HTTP, then USB, then BLE, and stops after the first successful transport |

Recommended one-line interactive install:

```bash
curl -fsSL https://raw.githubusercontent.com/ysuolmai/CodingLight/main/codex-hooks/install.py | python3
```

The installer:

- Lets you configure HTTP by LAN scan or by manually entering the light IP.
- Lets you configure USB with runtime auto-detect or a fixed serial port.
- Lets you configure BLE by scanning devices and saving the selected address.
- Lets you type `skip` for any transport.
- Installs `~/.codex/hooks/codinglight_status.py` and updates `~/.codex/hooks.json`. Existing `hooks.json` is backed up, and non-CodingLight hooks are preserved where possible.

The same command supports both hardware variants. Discovery uses `/api/info`,
USB Serial, or BLE; the battery BLE name is `CodingLight-Battery`, and the old
`VibeCodingLight` name remains recognized.

Manual example hook config:

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
export CODINGLIGHT_BLE_NAME=CodingLight,CodingLight-Battery
```

After changing Codex hooks, restart Codex CLI and run:

```text
/hooks
```

Review and trust the hook before expecting it to run.

## Security Notes

- The HTTP API has no authentication. Use it only on a trusted local network.
- Do not commit `wifi_secrets.h` or `ota_secrets.h`.
- ArduinoOTA has no password when `OTA_PASSWORD` is empty; use that only temporarily on an isolated trusted network.
- The hook script intentionally exits successfully if the light is unreachable so Codex work is not blocked by hardware.

## Project Status

This is a small personal hardware project. The firmware and hook protocol are
kept intentionally simple so the device remains easy to adapt to other agents
or status sources.

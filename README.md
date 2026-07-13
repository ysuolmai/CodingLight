# 高质量<付费>中转站

https://sc.350303.xyz/register?aff=C8X8NEL4BXX6

# CodingLight

[English README](README.en.md)

一个基于 ESP32-C3 Super Mini 的桌面 AI 编程状态灯。

CodingLight 把红、黄、绿三色 LED 做成一个实体状态灯，用来显示 Codex CLI 等本地 AI 编程助手的状态。它支持 HTTP、USB Serial 和 BLE Nordic UART Service 三种控制方式。

## 灯语

| 灯效 | 含义 |
| --- | --- |
| 绿灯常亮 | 空闲 |
| 绿 / 黄 / 红慢速跑马灯 | Agent 正在思考、写代码、构建或运行工具 |
| 黄灯闪烁 | 需要权限或确认 |
| 红灯闪烁 | 出错、阻塞或失败 |
| 绿灯闪烁 20 秒 | 任务完成，然后自动回到空闲 |
| 全灭 | 手动清除 |

## 硬件

已测试开发板：

- ESP32-C3 Super Mini

仓库维护两种硬件版本：

| 版本 | 固件 | LED 接线 | 按键 |
| --- | --- | --- | --- |
| 无电池 / wired | `CodingLight.ino` | GPIO2 绿、GPIO3 黄、GPIO4 红 | BOOT(GPIO9) 长按打开配网 |
| 电池 / battery | `firmware/CodingLightBattery/CodingLightBattery.ino` | GPIO2 红、GPIO3 黄、GPIO4 绿 | GPIO5 短按切换状态，长按 2 秒后松开进入深睡 |

无电池版 LED 接线：

```text
GPIO2 -> 绿灯负极
GPIO3 -> 黄灯负极
GPIO4 -> 红灯负极
LED 正极 -> 通过限流电阻接 3.3V
```

本项目默认使用公共阳极 LED：

```text
LED 亮 = LOW
LED 灭 = HIGH
```

所有 LED 输出都使用 LEDC PWM。动画代码不使用 `digitalWrite()`。

## 项目结构

```text
CodingLight.ino                 Arduino 主程序
wifi_secrets.example.h          WiFi 配置示例
ota_secrets.example.h           OTA 密码示例
firmware/CodingLightBattery/
  CodingLightBattery.ino        电池版 Arduino 主程序
  wifi_secrets.example.h        电池版 WiFi 配置示例
  ota_secrets.example.h         电池版 OTA 密码示例
codex-hooks/codinglight_status.py
codex-hooks/hooks.example.json
.github/workflows/build-firmware.yml
README.md                       中文说明
README.en.md                    英文说明
```

`wifi_secrets.h` 和 `ota_secrets.h` 是本地密码文件，已经被 `.gitignore` 忽略，不应该提交到 GitHub。

## 烧录固件

1. 安装 Arduino IDE。
2. 安装 Espressif ESP32 开发板包。
3. 无电池版打开 `CodingLight.ino`；电池版打开 `firmware/CodingLightBattery/CodingLightBattery.ino`。
4. 开发板选择 ESP32-C3 对应型号，例如 `ESP32C3 Dev Module`。
5. 分区选择 `Minimal SPIFFS`，确保存在两个 OTA application slot。
6. 使用 USB 上传。

如果设备以前使用 `Huge APP` 或其他无 OTA 分区，第一次必须通过 USB 烧录新固件和分区表；切换成功后才能使用 OTA。

串口监视器波特率：

```text
115200
```

## 云编译和 Release

仓库包含 GitHub Actions 云编译 workflow：

```text
.github/workflows/build-firmware.yml
```

触发方式：

- push 到 `main` 分支时自动编译。
- 也可以在 GitHub Actions 页面手动运行 `Build Firmware`。

编译产物会上传到两处：

- 当前 workflow run 的 artifact。
- GitHub Release 里的 `continuous` prerelease。

`continuous` release 会被每次成功构建更新，包含分别标记为 `wired` 和 `battery` 的压缩包以及 `SHA256SUMS.txt`。不要在两个版本之间混刷固件，因为 LED 引脚定义不同。

云编译不会包含本地 WiFi 或 OTA 密码。无电池版烧录后可通过 `CodingLight-Setup` 配网；电池版没有 captive portal，公开构建默认通过 USB Serial 或 BLE 使用。

如果 release 更新失败，到仓库设置里确认：

```text
Settings -> Actions -> General -> Workflow permissions -> Read and write permissions
```

## WiFi 配网

以下 captive portal 配网流程适用于无电池版。

首次烧录后，如果固件里没有 WiFi 配置，CodingLight 会自动开启一个开放热点：

```text
CodingLight-Setup
```

连接这个热点后，手机或电脑通常会自动弹出 captive portal。如果没有自动弹出，手动打开：

```text
http://192.168.4.1/
```

在页面里填写 SSID 和密码后提交。设备会把 WiFi 配置保存到 ESP32 的 NVS 里，之后重启也会继续使用。

如果需要重新配网，长按 ESP32-C3 Super Mini 的 `BOOT` 按键约 2.5 秒，会重新开启 `CodingLight-Setup` 热点。只打开配网页不会清掉旧 SSID 和密码；只有提交新的 SSID 后才会覆盖旧配置。

也可以选择在本地编译时写入默认 WiFi。复制示例文件：

```bash
cp wifi_secrets.example.h wifi_secrets.h
```

编辑 `wifi_secrets.h`：

```cpp
#pragma once

static const char WIFI_SSID[] = "your_wifi_name";
static const char WIFI_PASSWORD[] = "your_wifi_password";
```

`wifi_secrets.h` 已经被 `.gitignore` 忽略，不会提交到 GitHub。通过配网页保存到 NVS 的配置优先级高于 `wifi_secrets.h`。

如果没有 `wifi_secrets.h`，或者 SSID 为空，设备仍然可以通过 USB Serial、BLE 和配网 AP 使用；HTTP 控制页面在 AP 下也可用，连接路由器后可以通过局域网访问。

电池版需要在 `firmware/CodingLightBattery/` 内复制并填写自己的 `wifi_secrets.h`。没有 WiFi 配置时，电池版仍可通过 USB Serial 和 BLE 使用，但 HTTP 和 OTA 不会启动。

## OTA 更新

两种固件在 WiFi 连接成功后都会启用 ArduinoOTA：

- 无电池版主机名：`codinglight.local`
- 电池版主机名：`codinglight-battery.local`
- OTA 过程中灯进入 `OTA` 状态；失败时进入 `ERROR`。
- 建议从对应目录的 `ota_secrets.example.h` 复制为 `ota_secrets.h` 并设置强密码。
- OTA 只能更新 application 固件。改变分区表、bootloader 或刷错固件时，必须回到 USB 烧录。

## 控制接口

### Serial 和 BLE 命令

每行发送一个命令：

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

返回值：

```text
PONG
OK
ERR
```

`INFO` 返回 JSON：

```json
{"state":"IDLE","ip":"192.168.1.50","wifi":true,"ap":false,"ap_ip":"0.0.0.0","ble":true,"brightness":180,"uptime":12345}
```

### BLE

BLE 设备名：

```text
CodingLight
```

Nordic UART Service UUID：

```text
Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX       6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX       6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

向 RX 写入命令，订阅 TX notification 接收响应。

### HTTP

WiFi 连接成功后，可以访问：

```text
http://codinglight.local/
```

如果你的网络不支持 mDNS，可以通过 Serial 或 BLE 发送 `INFO` 获取设备 IP。

配网 AP 打开时：

```text
http://192.168.4.1/
```

会显示 WiFi 配网页；原来的灯控制页面仍然可以通过下面地址打开：

```text
http://192.168.4.1/control
```

REST API：

```text
GET  /api/info
POST /api/state
POST /api/brightness
```

示例：

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

仓库内置了 Codex CLI hook 适配器：

```text
codex-hooks/codinglight_status.py
codex-hooks/install.py
codex-hooks/hooks.example.json
```

Codex 事件和灯效映射：

| Codex 事件 | 灯状态 |
| --- | --- |
| `SessionStart` | `IDLE` |
| `UserPromptSubmit` | `THINKING` |
| `PreToolUse` | `BUILD` |
| `PostToolUse` | `CODING` |
| `PermissionRequest` | `WARNING` |
| `Stop` | `SUCCESS` |

如果 Codex 因 API、上游或三方模型服务错误导致当前任务中止，hook 会监听本地 Codex session 日志并把灯切到 `ERROR`。

Hook 支持的传输方式：

| 传输 | 说明 |
| --- | --- |
| `http` | 推荐方式，设备连接 WiFi 后使用 |
| `usb` | 使用 USB Serial 命令协议；可自动识别常见 USB 串口 |
| `ble` | 使用 BLE NUS，需要 Python 包 `bleak`；安装脚本可扫描并保存设备地址 |
| `auto` | 依次尝试 HTTP、USB、BLE，第一条成功后停止，不会三路同时发送 |

推荐使用交互式一行安装：

```bash
curl -fsSL https://raw.githubusercontent.com/ysuolmai/CodingLight/main/codex-hooks/install.py | python3
```

安装脚本会：

- 询问是否配置 HTTP，并可扫描局域网或手动输入灯的 IP。
- 询问是否配置 USB，可使用运行时自动识别，也可指定串口。
- 询问是否配置 BLE，扫描后选择设备并保存地址。
- 每一种传输都可以输入 `skip` 跳过。
- 安装 `~/.codex/hooks/codinglight_status.py`，并更新 `~/.codex/hooks.json`。已有 `hooks.json` 会先备份，并尽量保留非 CodingLight hook。

同一条安装命令适用于两种硬件。安装器通过 `/api/info`、USB 串口或 BLE 扫描选择实际设备；电池版 BLE 名为 `CodingLight-Battery`，旧代码的 `VibeCodingLight` 也保持兼容。

也可以手动安装示例 hook 配置：

```bash
mkdir -p ~/.codex
cp codex-hooks/hooks.example.json ~/.codex/hooks.json
```

编辑 `~/.codex/hooks.json`：

- 把 `/absolute/path/to/CodingLight` 替换成这个仓库的绝对路径。
- 把 `CODINGLIGHT_HOST` 设置成 `codinglight.local` 或设备 IP。

HTTP 示例：

```bash
export CODINGLIGHT_TRANSPORT=http
export CODINGLIGHT_HOST=codinglight.local
```

USB Serial 示例：

```bash
export CODINGLIGHT_TRANSPORT=usb
export CODINGLIGHT_SERIAL_PORT=/dev/ttyACM0
export CODINGLIGHT_SERIAL_BAUD=115200
```

BLE 示例：

```bash
python3 -m pip install bleak
export CODINGLIGHT_TRANSPORT=ble
export CODINGLIGHT_BLE_NAME=CodingLight,CodingLight-Battery
```

修改 Codex hooks 后，重启 Codex CLI，并运行：

```text
/hooks
```

检查并信任这个 hook 后，它才会运行。

## 安全说明

- HTTP API 没有鉴权，只建议在可信本地网络使用。
- 不要提交 `wifi_secrets.h` 或 `ota_secrets.h`。
- 未设置 `OTA_PASSWORD` 时 ArduinoOTA 没有密码保护，只应在隔离的可信网络中临时使用。
- Hook 脚本在无法连接状态灯时仍然返回成功，避免硬件故障阻塞 Codex 工作。

## 项目状态

这是一个小型个人硬件项目。固件和 hook 协议都刻意保持简单，方便改造成其他 Agent 或状态来源的实体提示灯。

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

LED 接线：

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
codex-hooks/codinglight_status.py
codex-hooks/hooks.example.json
.github/workflows/build-firmware.yml
README.md                       中文说明
README.en.md                    英文说明
```

`wifi_secrets.h` 是你的本地 WiFi 密码文件，已经被 `.gitignore` 忽略，不应该提交到 GitHub。

## 烧录固件

1. 安装 Arduino IDE。
2. 安装 Espressif ESP32 开发板包。
3. 打开 `CodingLight.ino`。
4. 开发板选择 ESP32-C3 对应型号，例如 `ESP32C3 Dev Module`。
5. 如果默认分区太小，选择更大的 APP 分区。本项目不使用 OTA 固件上传，所以可以选择类似 `Huge APP` 的分区。
6. 使用 USB 上传。

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

`continuous` release 会被每次成功构建更新，适合下载“最新一次 main 分支固件”。产物里包含 `.bin`、`.elf`、`.map`、压缩包和 `SHA256SUMS.txt`。

云编译不会包含你的 `wifi_secrets.h`，因此不会把 WiFi 密码写进公开固件。直接烧录云编译固件后，设备会启动 `CodingLight-Setup` 配网 AP，通过 captive portal 配网即可。

如果 release 更新失败，到仓库设置里确认：

```text
Settings -> Actions -> General -> Workflow permissions -> Read and write permissions
```

## WiFi 配网

推荐方式是直接使用设备自带的配网 AP。

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

Hook 支持的传输方式：

| 传输 | 说明 |
| --- | --- |
| `http` | 推荐方式，设备连接 WiFi 后使用 |
| `usb` | 使用 USB Serial 命令协议 |
| `ble` | 使用 BLE NUS，需要 Python 包 `bleak` |
| `auto` | 依次尝试 HTTP、USB、BLE |

安装示例 hook 配置：

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
export CODINGLIGHT_BLE_NAME=CodingLight
```

修改 Codex hooks 后，重启 Codex CLI，并运行：

```text
/hooks
```

检查并信任这个 hook 后，它才会运行。

## 安全说明

- HTTP API 没有鉴权，只建议在可信本地网络使用。
- 不要提交 `wifi_secrets.h`。
- Hook 脚本在无法连接状态灯时仍然返回成功，避免硬件故障阻塞 Codex 工作。

## 项目状态

这是一个小型个人硬件项目。固件和 hook 协议都刻意保持简单，方便改造成其他 Agent 或状态来源的实体提示灯。

#!/usr/bin/env python3
"""Codex hook adapter for the ESP32-C3 CodingLight.

Supported transports:
  - HTTP: fastest and recommended when the ESP32 is on WiFi.
  - USB Serial: sends the same text commands as Arduino Serial Monitor.
  - BLE NUS: optional, requires the third-party Python package "bleak".

The script intentionally exits 0 even when the light is unreachable. Codex hooks
should never block coding work just because the physical indicator is offline.
"""

import asyncio
import json
import os
import sys
import time
import urllib.error
import urllib.request


DEFAULT_LIGHT_HOST = "codinglight.local"
DEFAULT_BLE_NAME = "CodingLight"
DEFAULT_BAUD = 115200
STATE_FILE = os.path.expanduser("~/.codex/tmp/codinglight_state.json")
HTTP_TIMEOUT_SECONDS = 0.7
SERIAL_TIMEOUT_SECONDS = 0.7
BLE_TIMEOUT_SECONDS = 4.0

NUS_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"


EVENT_TO_STATE = {
    "session_start": "IDLE",
    "user_prompt_submit": "THINKING",
    "pre_tool_use": "BUILD",
    "permission_request": "WARNING",
    "post_tool_use": "CODING",
    "stop": "SUCCESS",
}


def now_ms() -> int:
    return int(time.time() * 1000)


def load_state() -> dict:
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as handle:
            data = json.load(handle)
            return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_state(data: dict) -> None:
    os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
    tmp_file = f"{STATE_FILE}.tmp"
    with open(tmp_file, "w", encoding="utf-8") as handle:
        json.dump(data, handle, separators=(",", ":"))
    os.replace(tmp_file, STATE_FILE)


def send_http_state(state: str) -> bool:
    host = os.environ.get("CODINGLIGHT_HOST", DEFAULT_LIGHT_HOST).strip()
    if not host:
        return False

    if host.startswith("http://") or host.startswith("https://"):
        base_url = host.rstrip("/")
    else:
        base_url = f"http://{host}"

    body = json.dumps({"state": state}, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        f"{base_url}/api/state",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
            return 200 <= response.status < 300
    except (OSError, urllib.error.URLError, urllib.error.HTTPError):
        return False


def send_serial_with_pyserial(port: str, baud: int, command: str) -> bool:
    try:
        import serial  # type: ignore
    except ImportError:
        return False

    try:
        with serial.Serial(port, baudrate=baud, timeout=SERIAL_TIMEOUT_SECONDS) as handle:
            handle.write((command + "\n").encode("utf-8"))
            handle.flush()
            response = handle.readline().decode("utf-8", errors="replace").strip()
            return response in {"OK", "PONG"} or response.startswith("{")
    except Exception:
        return False


def baud_constant(termios_module, baud: int) -> int:
    mapping = {
        9600: termios_module.B9600,
        19200: termios_module.B19200,
        38400: termios_module.B38400,
        57600: termios_module.B57600,
        115200: termios_module.B115200,
    }
    return mapping.get(baud, termios_module.B115200)


def send_serial_posix(port: str, baud: int, command: str) -> bool:
    if os.name != "posix":
        return False

    try:
        import termios
    except ImportError:
        return False

    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError:
        return False

    try:
        attrs = termios.tcgetattr(fd)
        speed = baud_constant(termios, baud)

        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = attrs[2] | termios.CLOCAL | termios.CREAD
        attrs[2] = attrs[2] & ~termios.PARENB
        attrs[2] = attrs[2] & ~termios.CSTOPB
        attrs[2] = attrs[2] & ~termios.CSIZE
        attrs[2] = attrs[2] | termios.CS8
        attrs[3] = 0
        attrs[4] = speed
        attrs[5] = speed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 2

        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        os.write(fd, (command + "\n").encode("utf-8"))

        deadline = time.monotonic() + SERIAL_TIMEOUT_SECONDS
        response = bytearray()
        while time.monotonic() < deadline:
            try:
                chunk = os.read(fd, 128)
                if chunk:
                    response.extend(chunk)
                    if b"\n" in response:
                        break
            except BlockingIOError:
                pass
            time.sleep(0.02)

        text = response.decode("utf-8", errors="replace").strip()
        return text in {"OK", "PONG"} or text.startswith("{")
    except OSError:
        return False
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def send_usb_state(state: str) -> bool:
    port = os.environ.get("CODINGLIGHT_SERIAL_PORT", "").strip()
    if not port:
        return False

    baud_text = os.environ.get("CODINGLIGHT_SERIAL_BAUD", str(DEFAULT_BAUD)).strip()
    try:
        baud = int(baud_text)
    except ValueError:
        baud = DEFAULT_BAUD

    command = f"STATE {state}"
    return send_serial_with_pyserial(port, baud, command) or send_serial_posix(port, baud, command)


async def send_ble_state_async(state: str) -> bool:
    try:
        from bleak import BleakClient, BleakScanner  # type: ignore
    except ImportError:
        return False

    address = os.environ.get("CODINGLIGHT_BLE_ADDRESS", "").strip()
    name = os.environ.get("CODINGLIGHT_BLE_NAME", DEFAULT_BLE_NAME).strip()

    try:
        if address:
            device = await BleakScanner.find_device_by_address(address, timeout=BLE_TIMEOUT_SECONDS)
        else:
            device = await BleakScanner.find_device_by_filter(
                lambda found, _: found.name == name,
                timeout=BLE_TIMEOUT_SECONDS,
            )

        if device is None:
            return False

        async with BleakClient(device, timeout=BLE_TIMEOUT_SECONDS) as client:
            await client.write_gatt_char(NUS_RX_UUID, f"STATE {state}\n".encode("utf-8"), response=False)
        return True
    except Exception:
        return False


def send_ble_state(state: str) -> bool:
    return asyncio.run(send_ble_state_async(state))


def enabled_transports() -> list[str]:
    requested = os.environ.get("CODINGLIGHT_TRANSPORT", "auto").strip().lower()
    if requested in {"http", "usb", "ble"}:
        return [requested]
    if requested == "auto":
        return ["http", "usb", "ble"]
    return ["http", "usb", "ble"]


def set_light_state(state: str) -> bool:
    for transport in enabled_transports():
        if transport == "http" and send_http_state(state):
            return True
        if transport == "usb" and send_usb_state(state):
            return True
        if transport == "ble" and send_ble_state(state):
            return True
    return False


def main() -> int:
    if len(sys.argv) < 2:
        return 0

    event = sys.argv[1].strip().lower()
    state = load_state()
    turn = int(state.get("turn", 0))

    if event == "user_prompt_submit":
        turn += 1

    desired_state = EVENT_TO_STATE.get(event)
    if desired_state is None:
        return 0

    state.update({"turn": turn, "event": event, "updated_ms": now_ms()})
    save_state(state)
    set_light_state(desired_state)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

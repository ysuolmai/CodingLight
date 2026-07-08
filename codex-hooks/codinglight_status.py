#!/usr/bin/env python3
"""Codex hook adapter for the ESP32-C3 CodingLight.

Supported transports:
  - HTTP: fastest and recommended when the ESP32 is on WiFi.
  - USB Serial: auto-discovers common ports, or uses configured ports.
  - BLE NUS: optional, requires the third-party Python package "bleak".

The script intentionally exits 0 even when the light is unreachable. Codex hooks
should never block coding work just because the physical indicator is offline.
"""

import asyncio
import glob
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request


DEFAULT_LIGHT_HOST = "codinglight.local"
DEFAULT_BLE_NAME = "CodingLight"
DEFAULT_BAUD = 115200
STATE_FILE = os.path.expanduser("~/.codex/tmp/codinglight_state.json")
WATCHER_LOG_FILE = os.path.expanduser("~/.codex/tmp/codinglight_error_watcher.log")
SESSION_GLOB = os.path.expanduser("~/.codex/sessions/**/*.jsonl")
HTTP_TIMEOUT_SECONDS = 0.7
SERIAL_TIMEOUT_SECONDS = 0.7
BLE_TIMEOUT_SECONDS = 1.2

SERIAL_PORT_PATTERNS = (
    "/dev/serial/by-id/*",
    "/dev/ttyACM*",
    "/dev/ttyUSB*",
    "/dev/cu.usbmodem*",
    "/dev/cu.usbserial*",
)


def env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, str(default)).strip())
    except ValueError:
        return default


def env_float(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, str(default)).strip())
    except ValueError:
        return default


HTTP_TIMEOUT_SECONDS = env_float("CODINGLIGHT_HTTP_TIMEOUT_SECONDS", HTTP_TIMEOUT_SECONDS)
SERIAL_TIMEOUT_SECONDS = env_float("CODINGLIGHT_SERIAL_TIMEOUT_SECONDS", SERIAL_TIMEOUT_SECONDS)
BLE_TIMEOUT_SECONDS = env_float("CODINGLIGHT_BLE_TIMEOUT_SECONDS", BLE_TIMEOUT_SECONDS)
ERROR_WATCH_SECONDS = env_int("CODINGLIGHT_ERROR_WATCH_SECONDS", 900)

NUS_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"


EVENT_TO_STATE = {
    "session_start": "IDLE",
    "user_prompt_submit": "THINKING",
    "pre_tool_use": "BUILD",
    "permission_request": "WARNING",
    "post_tool_use": "CODING",
    "stop": "SUCCESS",
    "api_error": "ERROR",
}


ERROR_EVENT_TYPES = {
    "error",
    "turn.failed",
    "turn_failed",
    "task_failed",
}

TEXT_ERROR_MARKERS = (
    "api request failed",
    "api error",
    "request failed",
    "stream error",
    "stream disconnected",
    "connection reset",
    "connection refused",
    "connection closed",
    "timed out",
    "timeout",
    "rate limit",
    "too many requests",
    "unauthorized",
    "forbidden",
    "bad gateway",
    "gateway timeout",
    "service unavailable",
    "upstream",
    "provider error",
    "status 401",
    "status 403",
    "status 429",
    "status 500",
    "status 502",
    "status 503",
    "status 504",
)


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


def append_watcher_log(message: str) -> None:
    try:
        os.makedirs(os.path.dirname(WATCHER_LOG_FILE), exist_ok=True)
        with open(WATCHER_LOG_FILE, "a", encoding="utf-8") as handle:
            handle.write(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} {message}\n")
    except OSError:
        pass


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


def split_env_list(value: str) -> list[str]:
    normalized = value.replace(";", ",").replace(os.pathsep, ",")
    return [item.strip() for item in normalized.split(",") if item.strip()]


def expand_serial_port_token(token: str) -> list[str]:
    expanded = os.path.expanduser(token)
    matches = glob.glob(expanded)
    return matches if matches else [expanded]


def is_likely_usb_serial_port(port) -> bool:
    device = str(getattr(port, "device", "") or "")
    if (
        device.startswith("/dev/ttyACM")
        or device.startswith("/dev/ttyUSB")
        or device.startswith("/dev/cu.usbmodem")
        or device.startswith("/dev/cu.usbserial")
    ):
        return True

    if getattr(port, "vid", None) is not None or getattr(port, "pid", None) is not None:
        return True

    text = " ".join(
        str(getattr(port, attr, "") or "")
        for attr in ("description", "hwid", "manufacturer", "product")
    ).lower()
    return any(marker in text for marker in ("usb", "acm", "cp210", "ch340", "esp32", "espressif"))


def serial_ports_from_pyserial() -> list[str]:
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        return []

    try:
        return [
            port.device
            for port in list_ports.comports()
            if port.device and is_likely_usb_serial_port(port)
        ]
    except Exception:
        return []


def candidate_serial_ports() -> list[str]:
    configured = (
        os.environ.get("CODINGLIGHT_SERIAL_PORTS", "").strip()
        or os.environ.get("CODINGLIGHT_SERIAL_PORT", "").strip()
    )

    candidates: list[str] = []
    if configured:
        for token in split_env_list(configured):
            candidates.extend(expand_serial_port_token(token))
    else:
        candidates.extend(serial_ports_from_pyserial())
        for pattern in SERIAL_PORT_PATTERNS:
            candidates.extend(glob.glob(pattern))

    result: list[str] = []
    seen: set[str] = set()
    for port in candidates:
        if port not in seen:
            result.append(port)
            seen.add(port)
    return result


def send_usb_state(state: str) -> bool:
    ports = candidate_serial_ports()
    if not ports:
        return False

    baud_text = os.environ.get("CODINGLIGHT_SERIAL_BAUD", str(DEFAULT_BAUD)).strip()
    try:
        baud = int(baud_text)
    except ValueError:
        baud = DEFAULT_BAUD

    command = f"STATE {state}"
    for port in ports:
        if send_serial_with_pyserial(port, baud, command) or send_serial_posix(port, baud, command):
            return True
    return False


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

    result: list[str] = []
    for item in split_env_list(requested):
        if item in {"http", "usb", "ble"} and item not in result:
            result.append(item)
    return result if result else ["http", "usb", "ble"]


def set_light_state(state: str) -> bool:
    for transport in enabled_transports():
        if transport == "http" and send_http_state(state):
            return True
        if transport == "usb" and send_usb_state(state):
            return True
        if transport == "ble" and send_ble_state(state):
            return True
    return False


def newest_session_file() -> str:
    candidates = glob.glob(SESSION_GLOB, recursive=True)
    if not candidates:
        return ""
    return max(candidates, key=lambda path: os.path.getmtime(path))


def payload_type(payload: object) -> str:
    if isinstance(payload, dict):
        value = payload.get("type")
        return value if isinstance(value, str) else ""
    return ""


def payload_has_error_shape(obj: dict) -> bool:
    top_type = obj.get("type")
    payload = obj.get("payload")
    nested_type = payload_type(payload)

    for value in (top_type, nested_type):
        if isinstance(value, str):
            lowered = value.lower()
            if lowered in ERROR_EVENT_TYPES or "error" in lowered or "failed" in lowered:
                return True

    if top_type != "event_msg" or not isinstance(payload, dict):
        return False

    if nested_type in {"agent_message", "user_message", "token_count", "task_started", "task_complete"}:
        return False

    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).lower()
    return any(marker in text for marker in TEXT_ERROR_MARKERS)


def line_indicates_api_error(line: str) -> bool:
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return False
    return isinstance(obj, dict) and payload_has_error_shape(obj)


def current_turn_changed_or_finished(watched_turn: int) -> bool:
    state = load_state()
    current_turn = int(state.get("turn", 0))
    current_event = str(state.get("event", ""))
    return current_turn != watched_turn or current_event in {"stop", "api_error"}


def watch_for_api_errors(watched_turn: int, session_file: str, start_offset: int) -> int:
    deadline = time.monotonic() + ERROR_WATCH_SECONDS
    offset = start_offset

    append_watcher_log(f"watch turn={watched_turn} file={session_file} offset={start_offset}")

    while time.monotonic() < deadline:
        if current_turn_changed_or_finished(watched_turn):
            return 0

        try:
            size = os.path.getsize(session_file)
            if size < offset:
                offset = 0
            if size > offset:
                with open(session_file, "r", encoding="utf-8", errors="replace") as handle:
                    handle.seek(offset)
                    for line in handle:
                        if line_indicates_api_error(line):
                            state = load_state()
                            if int(state.get("turn", 0)) == watched_turn:
                                state.update({"turn": watched_turn, "event": "api_error", "updated_ms": now_ms()})
                                save_state(state)
                                set_light_state("ERROR")
                                append_watcher_log(f"api_error turn={watched_turn}")
                            return 0
                    offset = handle.tell()
        except OSError as exc:
            append_watcher_log(f"watch_error turn={watched_turn} error={exc}")
            return 0

        time.sleep(0.35)

    return 0


def start_api_error_watcher(turn: int) -> None:
    if ERROR_WATCH_SECONDS <= 0:
        return

    session_file = newest_session_file()
    if not session_file:
        return

    try:
        start_offset = os.path.getsize(session_file)
        subprocess.Popen(
            [
                sys.executable,
                __file__,
                "__watch_api_errors",
                str(turn),
                session_file,
                str(start_offset),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
            start_new_session=True,
        )
    except (OSError, ValueError):
        return


def main() -> int:
    if len(sys.argv) < 2:
        return 0

    event = sys.argv[1].strip().lower()

    if event == "__watch_api_errors":
        if len(sys.argv) < 5:
            return 0
        try:
            watched_turn = int(sys.argv[2])
            start_offset = int(sys.argv[4])
        except ValueError:
            return 0
        return watch_for_api_errors(watched_turn, sys.argv[3], start_offset)

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

    if event == "user_prompt_submit":
        start_api_error_watcher(turn)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Interactive installer for CodingLight Codex hooks.

One-line install:
  curl -fsSL https://raw.githubusercontent.com/ysuolmai/CodingLight/main/codex-hooks/install.py | python3
"""

import asyncio
import concurrent.futures
import glob
import ipaddress
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


RAW_BASE_URL = "https://raw.githubusercontent.com/ysuolmai/CodingLight/main"
STATUS_SCRIPT_URL = f"{RAW_BASE_URL}/codex-hooks/codinglight_status.py"
EVENTS = [
    ("SessionStart", "session_start", "startup|resume|clear|compact"),
    ("UserPromptSubmit", "user_prompt_submit", None),
    ("PreToolUse", "pre_tool_use", "*"),
    ("PermissionRequest", "permission_request", "*"),
    ("PostToolUse", "post_tool_use", "*"),
    ("Stop", "stop", None),
]
USB_PATTERNS = (
    "/dev/serial/by-id/*",
    "/dev/ttyACM*",
    "/dev/ttyUSB*",
    "/dev/cu.usbmodem*",
    "/dev/cu.usbserial*",
)


def ask(prompt: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    try:
        value = input(f"{prompt}{suffix}: ").strip()
    except EOFError:
        value = ""
    return value or default


def ask_yes_no(prompt: str, default: bool = True) -> bool:
    default_text = "Y/n" if default else "y/N"
    while True:
        value = ask(f"{prompt} ({default_text})").lower()
        if not value:
            return default
        if value in {"y", "yes"}:
            return True
        if value in {"n", "no", "s", "skip"}:
            return False
        print("Please enter y, n, or skip.")


def local_status_script() -> Path | None:
    try:
        path = Path(__file__).resolve().with_name("codinglight_status.py")
    except NameError:
        return None
    return path if path.exists() else None


def fetch_status_script() -> str:
    local = local_status_script()
    if local:
        return local.read_text(encoding="utf-8")
    with urllib.request.urlopen(STATUS_SCRIPT_URL, timeout=15) as response:
        return response.read().decode("utf-8")


def install_status_script(codex_home: Path) -> Path:
    hooks_dir = codex_home / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    script_path = hooks_dir / "codinglight_status.py"
    script_path.write_text(fetch_status_script(), encoding="utf-8")
    script_path.chmod(0o755)
    return script_path


def read_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def backup_file(path: Path) -> None:
    if not path.exists():
        return
    stamp = time.strftime("%Y%m%d-%H%M%S")
    shutil.copy2(path, path.with_name(f"{path.name}.bak.{stamp}"))


def local_ipv4_addresses() -> list[str]:
    addresses: set[str] = set()

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as handle:
            handle.connect(("8.8.8.8", 80))
            addresses.add(handle.getsockname()[0])
    except OSError:
        pass

    try:
        for value in socket.gethostbyname_ex(socket.gethostname())[2]:
            addresses.add(value)
    except OSError:
        pass

    try:
        result = subprocess.run(
            ["ip", "-o", "-4", "addr", "show", "scope", "global"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2,
            check=False,
        )
        for match in re.findall(r"inet (\d+\.\d+\.\d+\.\d+)/", result.stdout):
            addresses.add(match)
    except (OSError, subprocess.SubprocessError):
        pass

    return sorted(
        ip
        for ip in addresses
        if not ip.startswith("127.") and not ip.startswith("169.254.")
    )


def probe_http_host(host: str, timeout: float = 0.45) -> dict | None:
    url = host if host.startswith(("http://", "https://")) else f"http://{host}"
    try:
        with urllib.request.urlopen(f"{url.rstrip('/')}/api/info", timeout=timeout) as response:
            data = json.loads(response.read().decode("utf-8"))
            if isinstance(data, dict) and "state" in data:
                data["_host"] = host
                return data
    except (OSError, urllib.error.URLError, json.JSONDecodeError):
        return None
    return None


def scan_lan_for_lights() -> list[dict]:
    ips = local_ipv4_addresses()
    targets: list[str] = []
    for ip in ips:
        network = ipaddress.ip_network(f"{ip}/24", strict=False)
        targets.extend(str(host) for host in network.hosts())

    targets = sorted(set(targets))
    if not targets:
        return []

    print(f"Scanning {len(targets)} LAN addresses for CodingLight...")
    found: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=96) as executor:
        futures = [executor.submit(probe_http_host, target) for target in targets]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            if result:
                found.append(result)

    return sorted(found, key=lambda item: item.get("_host", ""))


def configure_http(env: dict[str, str], transports: list[str]) -> None:
    if not ask_yes_no("Configure HTTP/LAN transport? Type n or skip to skip", True):
        return

    selected = ""
    if ask_yes_no("Scan local LAN for CodingLight first?", True):
        found = scan_lan_for_lights()
        if found:
            for idx, item in enumerate(found, 1):
                host = item.get("_host", "")
                variant = item.get("variant", "wired")
                state = item.get("state", "?")
                brightness = item.get("brightness", "?")
                print(f"{idx}. {host}  variant={variant} state={state} brightness={brightness}")
            choice = ask("Choose a device number, m for manual, or skip", "1")
            if choice.lower() not in {"s", "skip"}:
                if choice.lower() in {"m", "manual"}:
                    selected = ask("Enter CodingLight IP or host")
                else:
                    try:
                        selected = str(found[int(choice) - 1].get("_host", ""))
                    except (ValueError, IndexError):
                        selected = ""
        else:
            print("No CodingLight found on LAN scan.")

    if not selected:
        selected = ask("Enter CodingLight IP/host, or skip", "skip")
        if selected.lower() in {"s", "skip", "n", "no"}:
            return

    env["CODINGLIGHT_HOST"] = selected
    transports.append("http")


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


def detected_usb_ports() -> list[str]:
    candidates: list[str] = []
    try:
        from serial.tools import list_ports  # type: ignore
        candidates.extend(
            port.device
            for port in list_ports.comports()
            if port.device and is_likely_usb_serial_port(port)
        )
    except ImportError:
        pass
    except Exception:
        pass

    for pattern in USB_PATTERNS:
        candidates.extend(glob.glob(pattern))

    result: list[str] = []
    seen: set[str] = set()
    for port in candidates:
        if port not in seen:
            result.append(port)
            seen.add(port)
    return result


def configure_usb(env: dict[str, str], transports: list[str]) -> None:
    if not ask_yes_no("Configure USB Serial transport? Type n or skip to skip", True):
        return

    ports = detected_usb_ports()
    if ports:
        print("Detected likely USB serial ports:")
        for idx, port in enumerate(ports, 1):
            print(f"{idx}. {port}")
    else:
        print("No USB serial port detected now. Runtime auto-detect can still be enabled.")

    choice = ask("Use auto-detect, choose a number, enter a port, or skip", "auto")
    if choice.lower() in {"s", "skip", "n", "no"}:
        return
    if choice.lower() not in {"", "a", "auto"}:
        try:
            env["CODINGLIGHT_SERIAL_PORTS"] = ports[int(choice) - 1]
        except (ValueError, IndexError):
            env["CODINGLIGHT_SERIAL_PORTS"] = choice

    transports.append("usb")


async def scan_ble_devices(timeout: float = 6.0):
    from bleak import BleakScanner  # type: ignore
    return await BleakScanner.discover(timeout=timeout)


def ensure_bleak() -> bool:
    try:
        import bleak  # noqa: F401
        return True
    except ImportError:
        pass

    if not ask_yes_no("Python package 'bleak' is missing. Install it with pip --user now?", False):
        return False

    try:
        subprocess.run(
            [sys.executable, "-m", "pip", "install", "--user", "bleak"],
            check=True,
        )
        return True
    except (OSError, subprocess.CalledProcessError):
        print("Failed to install bleak. Skipping BLE.")
        return False


def configure_ble(env: dict[str, str], transports: list[str]) -> None:
    if not ask_yes_no("Configure BLE transport? Type n or skip to skip", True):
        return
    if not ensure_bleak():
        return

    print("Scanning BLE devices...")
    try:
        devices = asyncio.run(scan_ble_devices())
    except Exception as exc:
        print(f"BLE scan failed: {exc}")
        devices = []

    rows = []
    for device in devices:
        name = getattr(device, "name", None) or ""
        address = getattr(device, "address", None) or ""
        if name or address:
            rows.append((name, address))

    rows.sort(
        key=lambda item: (
            0 if item[0] in {"CodingLight", "CodingLight-Battery", "VibeCodingLight"} else 1,
            item[0],
            item[1],
        )
    )
    if rows:
        for idx, (name, address) in enumerate(rows, 1):
            label = name or "(no name)"
            print(f"{idx}. {label}  {address}")
    else:
        print("No BLE devices found.")

    choice = ask("Choose a BLE device number, enter address manually, or skip", "skip")
    if choice.lower() in {"s", "skip", "n", "no"}:
        return

    name = ""
    address = ""
    try:
        name, address = rows[int(choice) - 1]
    except (ValueError, IndexError):
        address = choice

    if address:
        env["CODINGLIGHT_BLE_ADDRESS"] = address
    if name:
        env["CODINGLIGHT_BLE_NAME"] = name
    elif "CODINGLIGHT_BLE_ADDRESS" not in env:
        env["CODINGLIGHT_BLE_NAME"] = ask(
            "Enter BLE device name",
            "CodingLight,CodingLight-Battery,VibeCodingLight",
        )

    transports.append("ble")


def shell_env(env: dict[str, str]) -> str:
    return " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())


def hook_command(script_path: Path, event: str, env: dict[str, str]) -> str:
    prefix = shell_env(env)
    command = f"python3 {shlex.quote(str(script_path))} {shlex.quote(event)}"
    return f"{prefix} {command}" if prefix else command


def remove_old_codinglight_hooks(data: dict) -> dict:
    hooks = data.get("hooks")
    if not isinstance(hooks, dict):
        data["hooks"] = {}
        return data

    for event, groups in list(hooks.items()):
        if not isinstance(groups, list):
            continue
        kept_groups = []
        for group in groups:
            if not isinstance(group, dict):
                kept_groups.append(group)
                continue
            handlers = group.get("hooks")
            if not isinstance(handlers, list):
                kept_groups.append(group)
                continue
            kept_handlers = [
                handler
                for handler in handlers
                if "codinglight_status.py" not in str(handler.get("command", ""))
            ]
            if kept_handlers:
                group = dict(group)
                group["hooks"] = kept_handlers
                kept_groups.append(group)
        if kept_groups:
            hooks[event] = kept_groups
        else:
            hooks.pop(event, None)
    return data


def add_codinglight_hooks(data: dict, script_path: Path, env: dict[str, str]) -> dict:
    hooks = data.setdefault("hooks", {})
    for hook_event, script_event, matcher in EVENTS:
        group: dict = {
            "hooks": [
                {
                    "type": "command",
                    "command": hook_command(script_path, script_event, env),
                    "timeout": 5,
                    "statusMessage": "Updating CodingLight",
                }
            ]
        }
        if matcher is not None:
            group["matcher"] = matcher
        hooks.setdefault(hook_event, []).append(group)
    return data


def write_hooks_json(codex_home: Path, script_path: Path, env: dict[str, str]) -> Path:
    hooks_path = codex_home / "hooks.json"
    data = remove_old_codinglight_hooks(read_json(hooks_path))
    data = add_codinglight_hooks(data, script_path, env)
    backup_file(hooks_path)
    hooks_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return hooks_path


def main() -> int:
    print("CodingLight Codex hook installer")
    print("Each transport can be skipped. Enabled transports are tried in this order: HTTP, USB, BLE.")

    codex_home = Path(os.environ.get("CODEX_HOME", "~/.codex")).expanduser()
    codex_home.mkdir(parents=True, exist_ok=True)
    (codex_home / "tmp").mkdir(parents=True, exist_ok=True)

    env: dict[str, str] = {}
    transports: list[str] = []

    configure_http(env, transports)
    configure_usb(env, transports)
    configure_ble(env, transports)

    if not transports:
        print("No transports selected. Nothing was installed.")
        return 1

    env = {"CODINGLIGHT_TRANSPORT": ",".join(transports), **env}
    script_path = install_status_script(codex_home)
    hooks_path = write_hooks_json(codex_home, script_path, env)

    print("")
    print(f"Installed hook script: {script_path}")
    print(f"Updated hook config:   {hooks_path}")
    print(f"Enabled transports:   {', '.join(transports)}")
    print("Restart Codex, then run /hooks and trust the CodingLight hooks.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
OTA Release Script

Erhöht die Firmware-Version, baut das Projekt und lädt die Firmware
per SFTP auf den Webserver hoch.

Umgebungsvariablen (oder .env-Datei):
  FTP_PROTOCOL    sftp|ftp (Default: sftp)
  FTP_HOST        Server-Hostname
  FTP_PORT        SSH-Port (Default: 22)
  FTP_USER        SSH-Benutzer
  FTP_PASSWORD    SSH-Passwort (optional, wenn Schlüssel verwendet wird)
  FTP_KEY         Pfad zum SSH-Privatschlüssel (optional)
  FTP_FW_PATH     Remote-Pfad für firmware.bin (Default: /tx94/firmware/firmware.bin)
  FTP_MANIFEST_PATH Remote-Pfad für manifest.json (Default: /tx94/firmware/manifest.json)
  VERSION_BUMP    major|minor|patch (Default: patch)
  DRY_RUN         true|false (nur Version erhöhen, kein Build/Upload, Default: false)
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

try:
    from dotenv import load_dotenv
except ImportError:
    def load_dotenv():  # type: ignore[no-redef]
        pass


PROJECT_DIR = Path(__file__).resolve().parent.parent
SDKCONFIG = PROJECT_DIR / "sdkconfig"
SDKCONFIG_DEFAULTS = PROJECT_DIR / "sdkconfig.defaults"
IDF_PY = r"D:\Development\ESP32\v6.1\v6.1\esp-idf\tools\idf.py"
IDF_VENV_PYTHON = r"C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"


def load_env() -> None:
    """Lädt .env falls vorhanden."""
    env_file = PROJECT_DIR / ".env"
    if env_file.exists():
        load_dotenv(env_file)


def get_current_version() -> tuple[int, int, int] | None:
    """Liest die aktuelle Version aus sdkconfig (CONFIG_APP_PROJECT_VER_STR)."""
    if not SDKCONFIG.exists():
        return None
    text = SDKCONFIG.read_text(encoding="utf-8")
    m = re.search(r'^CONFIG_APP_PROJECT_VER_STR="([^"]+)"$', text, re.MULTILINE)
    if not m:
        return None
    ver = m.group(1)
    parts = re.match(r"^(\d+)\.(\d+)\.(\d+)$", ver)
    if parts:
        return tuple(int(x) for x in parts.groups())  # type: ignore[return-value]
    return None


def parse_version(ver: str) -> tuple[int, int, int] | None:
    parts = re.match(r"^(\d+)\.(\d+)\.(\d+)$", ver.strip())
    if parts:
        return tuple(int(x) for x in parts.groups())  # type: ignore[return-value]
    return None


def bump_version(ver: tuple[int, int, int], mode: str = "patch") -> tuple[int, int, int]:
    major, minor, patch = ver
    if mode == "major":
        return (major + 1, 0, 0)
    if mode == "minor":
        return (major, minor + 1, 0)
    return (major, minor, patch + 1)


def version_str(ver: tuple[int, int, int]) -> str:
    return f"{ver[0]}.{ver[1]}.{ver[2]}"


def set_version_in_sdkconfig(new_ver: str) -> None:
    """Setzt CONFIG_APP_PROJECT_VER und den Versionsstring in sdkconfig."""
    text = SDKCONFIG.read_text(encoding="utf-8")

    # Entferne alte Einträge falls vorhanden
    text = re.sub(r'^#? CONFIG_APP_PROJECT_VER_FROM_CONFIG is not set$',
                  '', text, flags=re.MULTILINE)
    text = re.sub(r'^CONFIG_APP_PROJECT_VER_STR=.*$', '', text, flags=re.MULTILINE)
    text = re.sub(r'^# CONFIG_APP_PROJECT_VER_STR is not set$', '', text, flags=re.MULTILINE)

    # Füge neue Einträge hinzu
    text += f'\nCONFIG_APP_PROJECT_VER_FROM_CONFIG=y\n'
    text += f'CONFIG_APP_PROJECT_VER_STR="{new_ver}"\n'

    SDKCONFIG.write_text(text, encoding="utf-8")


def run_cmd(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    print(f"$ {' '.join(cmd)}")
    env = os.environ.copy()
    env["IDF_PATH"] = os.environ.get("IDF_PATH", "D:\\Development\\ESP32\\v6.1\\v6.1\\esp-idf")
    env["IDF_TOOLS_PATH"] = os.environ.get("IDF_TOOLS_PATH", "C:\\Espressif\\tools")
    env["IDF_PYTHON_ENV_PATH"] = os.environ.get("IDF_PYTHON_ENV_PATH", "C:\\Espressif\\tools\\python\\v6.1\\venv")
    env["ESP_IDF_VERSION"] = os.environ.get("ESP_IDF_VERSION", "6.1.0")
    env["PYTHONUTF8"] = "1"
    env["PATH"] = env.get("PATH", "") + ";" + r"C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin"
    proc = subprocess.run(cmd, env=env, cwd=str(PROJECT_DIR), text=True)
    if check and proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd)
    return proc


def build_firmware() -> Path:
    """Baut das Projekt und gibt den Pfad zur firmware.bin zurück."""
    print("Baue Firmware ...")
    run_cmd([IDF_VENV_PYTHON, IDF_PY, "build"], check=True)

    fw_path = PROJECT_DIR / "build" / "texel_info_display.bin"
    if not fw_path.exists():
        raise FileNotFoundError(f"Firmware nicht gefunden: {fw_path}")
    print(f"Firmware gebaut: {fw_path} ({fw_path.stat().st_size} Bytes)")
    return fw_path


def _ensure_sftp_dir(sftp, remote_dir: str) -> None:
    """Erstellt ein Remote-Verzeichnis samt übergeordneten Verzeichnissen, falls nötig."""
    if not remote_dir:
        return
    # Remove leading/trailing slashes for processing
    parts = remote_dir.strip("/").split("/")
    current = ""
    for part in parts:
        if not part:
            continue
        current = f"{current}/{part}" if current else f"/{part}"
        try:
            sftp.chdir(current)
        except FileNotFoundError:
            sftp.mkdir(current)
            sftp.chdir(current)


def sftp_upload(
    host: str,
    port: int,
    user: str,
    password: str | None,
    key: str | None,
    local_path: Path,
    remote_path: str,
) -> None:
    """Lädt eine Datei per SFTP hoch."""
    import paramiko

    print(f"Verbinde mit {host}:{port} als {user} ...")
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    if key:
        client.connect(host, port=port, username=user, key_filename=key)
    else:
        client.connect(host, port=port, username=user, password=password)

    sftp = client.open_sftp()
    print(f"Lade {local_path.name} hoch nach {remote_path} ...")
    # Stelle sicher, dass das Remote-Verzeichnis existiert
    _ensure_sftp_dir(sftp, os.path.dirname(remote_path))
    sftp.put(str(local_path), remote_path)
    sftp.close()
    client.close()
    print("Upload abgeschlossen.")


def update_manifest(
    host: str,
    port: int,
    user: str,
    password: str | None,
    key: str | None,
    new_version: str,
    sha256_hex: str,
    fw_size: int,
    manifest_path: str,
    base_url: str = "https://familie-riegel.de/tx94/firmware/",
    dry_run: bool = False,
) -> None:
    """Lädt die aktualisierte manifest.json hoch."""
    manifest = {
        "version": new_version,
        "sha256": sha256_hex,
        "file": "firmware.bin",
        "size": fw_size,
    }

    content = json.dumps(manifest, indent=2) + "\n"
    print("Neues Manifest:")
    print(content)

    if dry_run:
        print("[DRY RUN] Manifest-Upload übersprungen.")
        return

    import io
    import paramiko

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    if key:
        client.connect(host, port=port, username=user, key_filename=key)
    else:
        client.connect(host, port=port, username=user, password=password)

    sftp = client.open_sftp()
    with sftp.file(manifest_path, "w") as f:
        f.write(content)
    sftp.close()
    client.close()
    print("Manifest hochgeladen.")


def compute_sha256(file_path: Path) -> str:
    import hashlib
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    load_env()

    dry_run = os.getenv("DRY_RUN", "false").lower() == "true"
    bump_mode = os.getenv("VERSION_BUMP", "patch")

    print(f"OTA Release Script")
    print(f"  Projekt: {PROJECT_DIR}")
    print(f"  Version-Bump: {bump_mode}")
    print(f"  Dry Run: {dry_run}")
    print()

    # Aktuelle Version ermitteln
    current = get_current_version()
    if current:
        print(f"Aktuelle Version: {version_str(current)}")
    else:
        print("Keine Version in sdkconfig gefunden, starte bei 1.0.0")
        current = (1, 0, 0)

    new_ver = bump_version(current, bump_mode)
    new_str = version_str(new_ver)
    print(f"Neue Version: {new_str}")

    if dry_run:
        set_version_in_sdkconfig(new_str)
        print(f"[DRY RUN] Version auf {new_str} gesetzt (kein Build/Upload).")
        return 0

    # Version in sdkconfig schreiben
    set_version_in_sdkconfig(new_str)

    # Firmware bauen
    fw_path = build_firmware()

    # SHA-256 berechnen
    sha256_hex = compute_sha256(fw_path)
    fw_size = fw_path.stat().st_size
    print(f"SHA-256: {sha256_hex}")
    print(f"Größe: {fw_size} Bytes")

    # FTP-Upload
    host = os.getenv("FTP_HOST")
    port = int(os.getenv("FTP_PORT", "22"))
    user = os.getenv("FTP_USER")
    password = os.getenv("FTP_PASSWORD")
    key = os.getenv("FTP_KEY")
    fw_remote = os.getenv("FTP_FW_PATH", "/tx94/firmware/firmware.bin")
    manifest_remote = os.getenv("FTP_MANIFEST_PATH", "/tx94/firmware/manifest.json")

    if not host or not user:
        print("FEHLER: FTP_HOST und FTP_USER müssen gesetzt sein.")
        return 1

    sftp_upload(host, port, user, password, key, fw_path, fw_remote)

    # Manifest aktualisieren und hochladen
    update_manifest(
        host, port, user, password, key,
        new_str, sha256_hex, fw_size,
        manifest_remote,
    )

    print(f"\nRelease {new_str} fertiggestellt.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"FEHLER: {exc}", file=sys.stderr)
        sys.exit(1)

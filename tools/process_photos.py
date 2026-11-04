#!/usr/bin/env python3

import argparse
import ftplib
import hashlib
import io
import json
import os
import re
import ssl
import sys
import time
import unicodedata
from datetime import datetime
from pathlib import Path
from urllib.error import URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from PIL import ExifTags, Image, ImageOps

PHOTO_WIDTH = 256
PHOTO_HEIGHT = 137
DEFAULT_MAX_BYTES = 12 * 1024
SUPPORTED_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".heic", ".heif"}


def register_heif(source_root):
    has_heif = any(path.suffix.lower() in {".heic", ".heif"}
                   for path in source_root.rglob("*") if path.is_file())
    if not has_heif:
        return
    try:
        from pillow_heif import register_heif_opener
    except ImportError as error:
        raise ValueError("HEIC photos require pillow-heif; install tools/requirements-photos.txt") from error
    register_heif_opener()


def output_name(source_path, source_root):
    relative_name = source_path.relative_to(source_root).as_posix()
    stem = unicodedata.normalize("NFKD", source_path.stem).encode("ascii", "ignore").decode()
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", stem).strip("._-") or "photo"
    suffix = hashlib.sha256(relative_name.encode("utf-8")).hexdigest()[:8]
    return f"{stem[:48]}-{suffix}.jpg"


def rational_value(value):
    if isinstance(value, tuple):
        return float(value[0]) / float(value[1])
    return float(value)


def gps_coordinates(exif):
    try:
        gps_ifd = exif.get_ifd(ExifTags.IFD.GPSInfo)
    except (AttributeError, KeyError, TypeError):
        return None
    gps = {ExifTags.GPSTAGS.get(tag, tag): value for tag, value in gps_ifd.items()}
    try:
        latitude = sum(rational_value(part) / (60 ** index)
                       for index, part in enumerate(gps["GPSLatitude"]))
        longitude = sum(rational_value(part) / (60 ** index)
                        for index, part in enumerate(gps["GPSLongitude"]))
        if gps.get("GPSLatitudeRef") == "S":
            latitude = -latitude
        if gps.get("GPSLongitudeRef") == "W":
            longitude = -longitude
        return latitude, longitude
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        return None


def exif_date(exif):
    values = dict(exif.items())
    try:
        values.update(exif.get_ifd(ExifTags.IFD.Exif))
    except (AttributeError, KeyError, TypeError):
        pass
    names = {tag: name for tag, name in ExifTags.TAGS.items()}
    for preferred_name in ("DateTimeOriginal", "DateTimeDigitized", "DateTime"):
        for tag, value in values.items():
            if names.get(tag) != preferred_name:
                continue
            try:
                return datetime.strptime(str(value), "%Y:%m:%d %H:%M:%S").date().isoformat()
            except ValueError:
                continue
    return None


def creation_date(source_path):
    stat = source_path.stat()
    timestamp = getattr(stat, "st_birthtime", stat.st_ctime)
    return datetime.fromtimestamp(timestamp).date().isoformat()


def fallback_coordinates(coordinates):
    latitude, longitude = coordinates
    return f"GPS {latitude:.4f}, {longitude:.4f}"


def geocode_place(coordinates, user_agent, cache, last_request):
    if not coordinates:
        return ""
    latitude, longitude = coordinates
    cache_key = (round(latitude, 3), round(longitude, 3))
    if cache_key in cache:
        return cache[cache_key]

    wait = 1.1 - (time.monotonic() - last_request[0])
    if wait > 0:
        time.sleep(wait)
    query = urlencode({
        "format": "jsonv2",
        "lat": f"{latitude:.6f}",
        "lon": f"{longitude:.6f}",
        "zoom": 10,
        "addressdetails": 1,
    })
    request = Request(
        f"https://nominatim.openstreetmap.org/reverse?{query}",
        headers={"User-Agent": user_agent, "Accept": "application/json"},
    )
    last_request[0] = time.monotonic()
    try:
        with urlopen(request, timeout=12) as response:
            result = json.load(response)
        address = result.get("address", {})
        place = next((address[key] for key in (
            "island", "village", "town", "city", "hamlet", "municipality",
            "county", "state",
        ) if address.get(key)), None)
        if not place:
            place = result.get("name") or result.get("display_name", "").split(",")[0]
        cache[cache_key] = place or fallback_coordinates(coordinates)
    except (OSError, URLError, TimeoutError, ValueError, json.JSONDecodeError) as error:
        print(f"Warning: location lookup failed for {latitude:.5f},{longitude:.5f}: {error}",
              file=sys.stderr)
        cache[cache_key] = fallback_coordinates(coordinates)
    return cache[cache_key]


def image_metadata(source_path, user_agent, geocode_cache, last_request):
    with Image.open(source_path) as source:
        exif = source.getexif()
        date = exif_date(exif) or creation_date(source_path)
        coordinates = gps_coordinates(exif)
    if coordinates:
        title = geocode_place(coordinates, user_agent, geocode_cache, last_request)
    else:
        title = source_path.stem.replace("_", " ").replace("-", " ")
    return {"date": date, "title": title}


def encode_photo(source_path, max_bytes):
    with Image.open(source_path) as source:
        image = ImageOps.exif_transpose(source).convert("RGB")
        image = ImageOps.fit(
            image,
            (PHOTO_WIDTH, PHOTO_HEIGHT),
            method=Image.Resampling.LANCZOS,
        )

    for quality in range(84, 29, -6):
        output = io.BytesIO()
        image.save(output, format="JPEG", quality=quality, optimize=True)
        encoded = output.getvalue()
        if len(encoded) <= max_bytes:
            return encoded
    raise ValueError(
        f"{source_path.name}: cannot meet the {max_bytes}-byte limit at {PHOTO_WIDTH}x{PHOTO_HEIGHT}"
    )


def read_old_manifest(manifest_path):
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    entries = manifest.get("images", [])
    if not isinstance(entries, list):
        return {}
    return {
        entry["path"]: entry
        for entry in entries
        if isinstance(entry, dict) and isinstance(entry.get("path"), str)
    }


def write_if_changed(path, content):
    encoded = content.encode("utf-8")
    if path.exists() and path.read_bytes() == encoded:
        return False
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(encoded)
    temporary.replace(path)
    return True


def manifest_json(records):
    return json.dumps({"version": 1, "images": records}, ensure_ascii=False,
                      indent=2) + "\n"


def process(source_root, output_root, max_bytes, user_agent):
    source_root = source_root.resolve()
    register_heif(source_root)
    output_root.mkdir(parents=True, exist_ok=True)
    manifest_path = output_root / "manifest.json"
    old_entries = read_old_manifest(manifest_path)
    geocode_cache = {}
    last_request = [0.0]
    records = []
    processed = 0

    sources = sorted(
        path for path in source_root.rglob("*")
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS
    )
    for source_path in sources:
        name = output_name(source_path, source_root)
        output_path = output_root / name
        previous = old_entries.get(name)
        source_stat = source_path.stat()
        output_stat = output_path.stat() if output_path.is_file() else None
        source_changed = output_stat is None or source_stat.st_mtime_ns > output_stat.st_mtime_ns
        needs_processing = source_changed or output_stat.st_size > max_bytes

        if needs_processing:
            encoded = encode_photo(source_path, max_bytes)
            output_path.write_bytes(encoded)
            os.utime(output_path, ns=(source_stat.st_atime_ns, source_stat.st_mtime_ns))
            if source_changed or not previous or not previous.get("date") or not previous.get("title"):
                metadata = image_metadata(source_path, user_agent, geocode_cache, last_request)
            else:
                metadata = {"date": previous["date"], "title": previous["title"]}
            processed += 1
        elif previous and previous.get("date") and previous.get("title"):
            metadata = {"date": previous["date"], "title": previous["title"]}
        else:
            metadata = image_metadata(source_path, user_agent, geocode_cache, last_request)

        records.append({"path": name, "date": metadata["date"], "title": metadata["title"],
                "size": output_path.stat().st_size})
        write_if_changed(manifest_path, manifest_json(records))
        print(f"[{len(records)}/{len(sources)}] {name}", flush=True)

    write_if_changed(manifest_path, manifest_json(records))
    print(f"Photos: {len(records)} manifest entries; {processed} images processed")


def upload_images(output_root, env_file):
    try:
        from dotenv import dotenv_values
    except ImportError as error:
        raise ValueError("FTP uploads require python-dotenv; install tools/requirements-photos.txt") from error

    settings = dotenv_values(env_file)
    host = settings.get("FTP_HOST")
    user = settings.get("FTP_USER")
    password = settings.get("FTP_PASSWORD")
    remote_dir = settings.get("FTP_REMOTE_DIR", "/tx94/photos/")
    try:
        port = int(settings.get("FTP_PORT", "21"))
    except (TypeError, ValueError) as error:
        raise ValueError("FTP_PORT in .env must be an integer") from error
    tls_value = str(settings.get("FTP_USE_TLS", "false")).strip().lower()
    if tls_value not in {"true", "false", "1", "0", "yes", "no"}:
        raise ValueError("FTP_USE_TLS in .env must be true or false")
    use_tls = tls_value in {"true", "1", "yes"}
    missing = [key for key, value in (
        ("FTP_HOST", host), ("FTP_USER", user), ("FTP_PASSWORD", password)
    ) if not value]
    if missing:
        raise ValueError(f"Missing FTP settings in {env_file}: {', '.join(missing)}")

    manifest_path = output_root / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError(f"Photo manifest not found: {manifest_path}")
    files = [manifest_path] + sorted(
        path for path in output_root.iterdir()
        if path.is_file() and path.suffix.lower() in {".jpg", ".jpeg"}
    )
    ftp = ftplib.FTP_TLS(context=ssl.create_default_context()) if use_tls else ftplib.FTP()
    if not use_tls:
        print("Warning: plain FTP sends credentials and files without encryption.", file=sys.stderr)
    try:
        ftp.connect(host, port, timeout=30)
        ftp.login(user, password)
        if use_tls:
            ftp.prot_p()
        ftp.set_pasv(True)
        ftp.cwd(remote_dir)
        for index, path in enumerate(files, 1):
            with path.open("rb") as image_file:
                ftp.storbinary(f"STOR {path.name}", image_file, blocksize=8192)
            print(f"[{index}/{len(files)}] Uploaded {path.name}", flush=True)
        ftp.quit()
    except (OSError, EOFError, ftplib.Error) as error:
        try:
            ftp.close()
        except OSError:
            pass
        raise RuntimeError(f"FTP upload failed: {error}") from error


def main():
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Scale photos and generate their web manifest")
    parser.add_argument("--source", type=Path, default=repo_root / "assets/photos/source")
    parser.add_argument("--output", type=Path, default=repo_root / "assets/photos/images")
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    parser.add_argument("--upload", action="store_true", help="upload the output folder via FTP")
    parser.add_argument("--env-file", type=Path, default=repo_root / ".env",
                        help="FTP credentials file used with --upload")
    parser.add_argument(
        "--user-agent",
        default="TexelInfoDisplay-photo-processor/1.0 (personal photo metadata processor)",
    )
    args = parser.parse_args()
    if args.max_bytes <= 0:
        parser.error("--max-bytes must be greater than zero")
    try:
        process(args.source, args.output, args.max_bytes, args.user_agent)
        if args.upload:
            upload_images(args.output.resolve(), args.env_file)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

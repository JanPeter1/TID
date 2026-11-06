#!/usr/bin/env python3

import argparse
import errno
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

PHOTO_PROFILE_CONFIG = Path(__file__).with_name("photo_profiles.json")
PHOTO_PROFILES = {
    name: (profile["width"], profile["height"], profile["max_bytes"])
    for name, profile in json.loads(
        PHOTO_PROFILE_CONFIG.read_text(encoding="utf-8")).items()
}
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


def manifest_date(value):
    for date_format in ("%Y-%m-%d", "%d.%m.%Y"):
        try:
            return datetime.strptime(value, date_format).strftime("%d.%m.%Y")
        except ValueError:
            continue
    raise ValueError(f"Invalid photo date in manifest metadata: {value}")


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


def encode_photo(source_path, width, height, max_bytes):
    with Image.open(source_path) as source:
        image = ImageOps.exif_transpose(source).convert("RGB")
        image = ImageOps.fit(
            image,
            (width, height),
            method=Image.Resampling.LANCZOS,
        )

    for quality in range(84, 29, -6):
        output = io.BytesIO()
        image.save(output, format="JPEG", quality=quality, optimize=True)
        encoded = output.getvalue()
        if len(encoded) <= max_bytes:
            return encoded
    raise ValueError(
        f"{source_path.name}: cannot meet the {max_bytes}-byte limit at {width}x{height}"
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


def process(source_root, output_root, max_bytes, user_agent, profile_names):
    source_root = source_root.resolve()
    register_heif(source_root)
    output_root.mkdir(parents=True, exist_ok=True)
    geocode_cache = {}
    last_request = [0.0]

    sources = sorted(
        path for path in source_root.rglob("*")
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS
    )
    for profile_name in profile_names:
        width, height, default_max_bytes = PHOTO_PROFILES[profile_name]
        profile_output = output_root / profile_name
        profile_output.mkdir(parents=True, exist_ok=True)
        manifest_path = profile_output / "manifest.json"
        old_entries = read_old_manifest(manifest_path)
        if not old_entries:
            old_entries = read_old_manifest(output_root / "manifest.json")
        records = []
        processed = 0

        for source_path in sources:
            name = output_name(source_path, source_root)
            output_path = profile_output / name
            previous = old_entries.get(name)
            source_stat = source_path.stat()
            output_stat = output_path.stat() if output_path.is_file() else None
            source_changed = (output_stat is None or
                              source_stat.st_mtime_ns > output_stat.st_mtime_ns)
            target_max_bytes = max_bytes or default_max_bytes
            needs_processing = (source_changed or
                                output_stat.st_size > target_max_bytes)

            if needs_processing:
                encoded = encode_photo(source_path, width, height, target_max_bytes)
                output_path.write_bytes(encoded)
                os.utime(output_path, ns=(source_stat.st_atime_ns, source_stat.st_mtime_ns))
                if (source_changed or not previous or not previous.get("date") or
                        not previous.get("title")):
                    metadata = image_metadata(
                        source_path, user_agent, geocode_cache, last_request)
                else:
                    metadata = {"date": previous["date"], "title": previous["title"]}
                processed += 1
            elif previous and previous.get("date") and previous.get("title"):
                metadata = {"date": previous["date"], "title": previous["title"]}
            else:
                metadata = image_metadata(
                    source_path, user_agent, geocode_cache, last_request)

            records.append({
                "path": name,
                "date": manifest_date(metadata["date"]),
                "title": metadata["title"],
                "size": output_path.stat().st_size,
            })
            write_if_changed(manifest_path, manifest_json(records))
            print(f"[{profile_name}] [{len(records)}/{len(sources)}] {name}", flush=True)

        write_if_changed(manifest_path, manifest_json(records))
        print(f"{profile_name}: {len(records)} manifest entries; "
              f"{processed} images processed")


def profile_upload_files(output_root, profile_name):
    profile_dir = output_root / profile_name
    manifest_path = profile_dir / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError(f"Photo manifest not found: {manifest_path}")
    return [manifest_path] + sorted(
        path for path in profile_dir.iterdir()
        if path.is_file() and path.suffix.lower() in {".jpg", ".jpeg"}
    )


def upload_ftp_images(output_root, profile_names, host, port, user, password,
                      remote_dir):
    ftp = ftplib.FTP_TLS(context=ssl.create_default_context())
    try:
        ftp.connect(host, port, timeout=30)
        ftp.login(user, password)
        ftp.prot_p()
        ftp.set_pasv(True)
        ftp.cwd(remote_dir)
        for profile_name in profile_names:
            try:
                ftp.cwd(profile_name)
            except ftplib.error_perm as error:
                if not str(error).startswith("550"):
                    raise
                ftp.mkd(profile_name)
                ftp.cwd(profile_name)

            files = profile_upload_files(output_root, profile_name)
            for index, path in enumerate(files, 1):
                with path.open("rb") as image_file:
                    ftp.storbinary(f"STOR {path.name}", image_file, blocksize=8192)
                print(f"[{profile_name}] [{index}/{len(files)}] "
                      f"Uploaded {path.name}", flush=True)
            ftp.cwd("..")
        ftp.quit()
    except (OSError, EOFError, ftplib.Error, UnicodeError) as error:
        ftp.close()
        raise RuntimeError(f"FTPS upload failed: {error}") from error


def upload_sftp_images(output_root, profile_names, host, port, user, password,
                       remote_dir):
    try:
        import paramiko
    except ImportError as error:
        raise ValueError(
            "SFTP uploads require paramiko; install tools/requirements-photos.txt"
        ) from error

    known_hosts = Path.home() / ".ssh" / "known_hosts"
    if not known_hosts.is_file():
        raise ValueError(f"SFTP known_hosts file not found: {known_hosts}")

    client = paramiko.SSHClient()
    client.load_system_host_keys(str(known_hosts))
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    sftp = None
    try:
        client.connect(
            hostname=host,
            port=port,
            username=user,
            password=password,
            timeout=30,
            banner_timeout=30,
            auth_timeout=30,
            allow_agent=False,
            look_for_keys=False,
        )
        sftp = client.open_sftp()
        sftp.chdir(remote_dir)
        for profile_name in profile_names:
            try:
                sftp.stat(profile_name)
            except OSError as error:
                if error.errno != errno.ENOENT:
                    raise
                sftp.mkdir(profile_name)
            sftp.chdir(profile_name)

            files = profile_upload_files(output_root, profile_name)
            for index, path in enumerate(files, 1):
                sftp.put(str(path), path.name, confirm=True)
                print(f"[{profile_name}] [{index}/{len(files)}] "
                      f"Uploaded {path.name}", flush=True)
            sftp.chdir("..")
    except (OSError, EOFError, paramiko.SSHException) as error:
        raise RuntimeError(f"SFTP upload failed: {error}") from error
    finally:
        if sftp is not None:
            sftp.close()
        client.close()


def upload_images(output_root, env_file, profile_names):
    try:
        from dotenv import dotenv_values
    except ImportError as error:
        raise ValueError("Photo uploads require python-dotenv; install tools/requirements-photos.txt") from error

    settings = dotenv_values(env_file)
    host = settings.get("FTP_HOST")
    user = settings.get("FTP_USER")
    password = settings.get("FTP_PASSWORD")
    remote_dir = settings.get("FTP_REMOTE_DIR", "/tx94/photos/")
    protocol = str(settings.get("FTP_PROTOCOL", "")).strip().lower()
    try:
        port = int(settings.get("FTP_PORT", "22" if protocol == "sftp" else "21"))
    except (TypeError, ValueError) as error:
        raise ValueError("FTP_PORT in .env must be an integer") from error
    tls_value = str(settings.get("FTP_USE_TLS", "false")).strip().lower()
    if tls_value not in {"true", "false", "1", "0", "yes", "no"}:
        raise ValueError("FTP_USE_TLS in .env must be true or false")
    if not protocol:
        protocol = "ftps" if tls_value in {"true", "1", "yes"} else "ftp"
    if protocol not in {"sftp", "ftps", "ftp"}:
        raise ValueError("FTP_PROTOCOL in .env must be sftp or ftps")
    if protocol == "ftp":
        raise ValueError("Unencrypted FTP is not supported; configure SFTP or FTPS")
    missing = [key for key, value in (
        ("FTP_HOST", host), ("FTP_USER", user), ("FTP_PASSWORD", password)
    ) if not value]
    if missing:
        raise ValueError(f"Missing upload settings in {env_file}: {', '.join(missing)}")

    if protocol == "sftp":
        upload_sftp_images(
            output_root, profile_names, host, port, user, password, remote_dir)
    else:
        upload_ftp_images(
            output_root, profile_names, host, port, user, password, remote_dir)


def main():
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Generate photo variants and manifests for supported displays")
    parser.add_argument("--source", type=Path, default=repo_root / "assets/photos/source")
    parser.add_argument("--output", type=Path, default=repo_root / "assets/photos/images")
    parser.add_argument("--max-bytes", type=int,
                        help="override each profile's maximum JPEG size")
    parser.add_argument("--profile", choices=["all", *PHOTO_PROFILES],
                        default="all", help="generate/upload only one display profile")
    parser.add_argument("--upload", action="store_true", help="upload the output folder via SFTP or FTPS")
    parser.add_argument("--env-file", type=Path, default=repo_root / ".env",
                        help="upload credentials file used with --upload")
    parser.add_argument(
        "--user-agent",
        default="TexelInfoDisplay-photo-processor/1.0 (personal photo metadata processor)",
    )
    args = parser.parse_args()
    if args.max_bytes is not None and args.max_bytes <= 0:
        parser.error("--max-bytes must be greater than zero")
    profile_names = (list(PHOTO_PROFILES) if args.profile == "all"
                     else [args.profile])
    try:
        process(args.source, args.output, args.max_bytes, args.user_agent,
                profile_names)
        if args.upload:
            upload_images(args.output.resolve(), args.env_file, profile_names)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

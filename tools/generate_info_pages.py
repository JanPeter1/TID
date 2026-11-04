#!/usr/bin/env python3

import argparse
import json
import os
import sys
from pathlib import Path

from PIL import Image, ImageOps

IMAGE_WIDTH = 80
IMAGE_HEIGHT = 60
MAX_LINES = 10


class InputError(Exception):
    pass


def parse_pages(path):
    pages = []
    current = None

    def finish():
        nonlocal current
        if current is None:
            return
        if not current["url"]:
            raise InputError(f"page {current['title']!r} has no @url")
        if not current["body"]:
            raise InputError(f"page {current['title']!r} has no text")
        pages.append(current)
        current = None

    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.rstrip()
        if line == "---":
            finish()
        elif line.startswith("# "):
            finish()
            title = line[2:].strip()
            if not title:
                raise InputError(f"line {line_number}: empty page title")
            current = {"title": title, "url": "", "image": "", "body": []}
        elif line.startswith("@url "):
            if current is None:
                raise InputError(f"line {line_number}: @url must follow a page title")
            if current["url"]:
                raise InputError(f"line {line_number}: duplicate @url")
            current["url"] = line[5:].strip()
        elif line.startswith("@image "):
            if current is None:
                raise InputError(f"line {line_number}: @image must follow a page title")
            if current["image"]:
                raise InputError(f"line {line_number}: duplicate @image")
            image_parts = line[7:].split()
            if not image_parts:
                raise InputError(f"line {line_number}: @image path is empty")
            if len(image_parts) > 2:
                raise InputError(f"line {line_number}: @image accepts a path and optional fit/contain mode")
            mode = image_parts[1] if len(image_parts) == 2 else "fit"
            if mode not in ("fit", "contain"):
                raise InputError(f"line {line_number}: @image mode must be fit or contain")
            current["image"] = {"source": image_parts[0], "mode": mode}
        elif line.startswith("@"):
            raise InputError(f"line {line_number}: unknown directive")
        elif current is None:
            if line.strip():
                raise InputError(f"line {line_number}: expected a page title")
        else:
            current["body"].append(line.strip())

    finish()
    if not pages:
        raise InputError("no pages found")
    if len(pages) > 255:
        raise InputError("at most 255 pages are supported")
    return pages


def wrap_page(page):
    rows = []
    for paragraph in page["body"]:
        if not paragraph:
            if rows and rows[-1]:
                rows.append("")
            continue

        is_bullet = paragraph.startswith("- ")
        text = paragraph[2:] if is_bullet else paragraph
        words = text.split()
        first_prefix = "- " if is_bullet else ""
        continuation_prefix = "  " if is_bullet else ""

        while words:
            prefix = first_prefix
            width = 24 if page["image"] and len(rows) < 5 else 38
            available = width - len(prefix)
            line_words = []
            used = 0
            while words:
                word = words[0]
                needed = len(word) + (1 if line_words else 0)
                if line_words and used + needed > available:
                    break
                if not line_words and len(word) > available:
                    raise InputError(f"page {page['title']!r}: word does not fit the display: {word!r}")
                line_words.append(words.pop(0))
                used += needed
            rows.append(prefix + " ".join(line_words))
            first_prefix = continuation_prefix

    while rows and rows[-1] == "":
        rows.pop()
    if len(rows) > MAX_LINES:
        raise InputError(
            f"page {page['title']!r} needs {len(rows)} display lines; maximum is {MAX_LINES}"
        )
    return rows


def c_string(value):
    return json.dumps(value, ensure_ascii=False)


def write_if_changed(path, contents):
    if path.exists() and path.read_bytes() == contents:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = path.with_suffix(path.suffix + ".tmp")
    temporary_path.write_bytes(contents)
    temporary_path.replace(path)


def scale_source_image(asset_root, image):
    relative_path = image["source"]
    source_dir = (asset_root / "source").resolve()
    source_path = (asset_root / relative_path).resolve()
    if not source_path.is_relative_to(source_dir) or source_path == source_dir:
        raise InputError(f"image path must point inside assets/info/source: {relative_path}")
    if not relative_path.startswith("source/") and not relative_path.startswith("source\\"):
        raise InputError(f"image path must start with source/: {relative_path}")
    output_path = asset_root / "images" / f"{source_path.stem}.rgb565"
    expected_size = IMAGE_WIDTH * IMAGE_HEIGHT * 2
    source_stat = source_path.stat()

    if output_path.exists():
        if output_path.stat().st_size != expected_size:
            output_path.unlink()
        elif output_path.stat().st_mtime_ns >= source_stat.st_mtime_ns:
            return output_path

    try:
        from PIL import Image, ImageOps

        with Image.open(source_path) as source:
            source = source.convert("RGB")
            if image["mode"] == "fit":
                converted = ImageOps.fit(
                    source,
                    (IMAGE_WIDTH, IMAGE_HEIGHT),
                    method=Image.Resampling.LANCZOS,
                )
            else:
                converted = Image.new("RGB", (IMAGE_WIDTH, IMAGE_HEIGHT), (8, 8, 12))
                source.thumbnail(
                    (IMAGE_WIDTH, IMAGE_HEIGHT),
                    Image.Resampling.LANCZOS,
                )
                converted.paste(
                    source,
                    ((IMAGE_WIDTH - source.width) // 2, (IMAGE_HEIGHT - source.height) // 2),
                )
    except ImportError as error:
        raise InputError("Pillow is required to scale info page images") from error
    except OSError as error:
        raise InputError(f"could not process image {relative_path!r}: {error}") from error

    raw = bytearray()
    for red, green, blue in converted.get_flattened_data():
        pixel = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        raw.extend(pixel.to_bytes(2, "little"))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_suffix(output_path.suffix + ".tmp")
    temporary_path.write_bytes(raw)
    temporary_path.replace(output_path)
    os.utime(output_path, ns=(source_stat.st_atime_ns, source_stat.st_mtime_ns))
    return output_path


def image_data(image_path):
    expected_size = IMAGE_WIDTH * IMAGE_HEIGHT * 2
    data = image_path.read_bytes()
    if len(data) != expected_size:
        raise InputError(
            f"image {image_path.name!r} must be {IMAGE_WIDTH}x{IMAGE_HEIGHT} RGB565 "
            f"({expected_size} bytes), got {len(data)}"
        )
    return data


def generate(pages, asset_root):
    image_names = []
    image_bytes = {}
    image_sources = {}
    for page in pages:
        if not page["url"].startswith("https://"):
            raise InputError(f"page {page['title']!r} @url must use HTTPS")
        if page["image"]:
            source_name = page["image"]["source"]
            output_name = Path(source_name).stem + ".rgb565"
            previous_source = image_sources.get(output_name)
            if previous_source and previous_source != source_name:
                raise InputError(
                    f"image sources {previous_source!r} and {source_name!r} "
                    f"would both generate {output_name!r}"
                )
            image_sources[output_name] = source_name
            if output_name not in image_bytes:
                scaled_path = scale_source_image(asset_root, page["image"])
                image_bytes[output_name] = image_data(scaled_path)
                image_names.append(output_name)
            page["image_output"] = output_name
        page["lines"] = wrap_page(page)

    image_symbols = {
        name: f"info_image_{index}" for index, name in enumerate(image_names)
    }
    output = [
        '#include "info_pages.h"',
        "",
    ]

    for name in image_names:
        symbol = image_symbols[name]
        data = image_bytes[name]
        output.append(f"static const uint8_t {symbol}_data[] = {{")
        for offset in range(0, len(data), 16):
            chunk = data[offset:offset + 16]
            output.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
        output.extend([
            "};",
            f"static const lv_image_dsc_t {symbol} = {{",
            "    .header = {",
            "        .magic = LV_IMAGE_HEADER_MAGIC,",
            "        .cf = LV_COLOR_FORMAT_RGB565,",
            f"        .w = {IMAGE_WIDTH},",
            f"        .h = {IMAGE_HEIGHT},",
            f"        .stride = {IMAGE_WIDTH * 2},",
            "    },",
            f"    .data_size = {len(data)},",
            f"    .data = {symbol}_data,",
            "};",
            "",
        ])

    output.append("const info_page_t info_pages[] = {")
    for page in pages:
        image = f"&{image_symbols[page['image_output']]}" if page["image"] else "NULL"
        output.extend([
            "    {",
            f"        .title = {c_string(page['title'])},",
            f"        .url = {c_string(page['url'])},",
            f"        .image = {image},",
            "        .lines = {",
        ])
        output.extend(
            f"            {c_string(line) if line is not None else 'NULL'},"
            for line in page["lines"] + [None] * (MAX_LINES - len(page["lines"]))
        )
        output.extend(["        },", "    },"])
    output.extend([
        "};",
        "",
        "const size_t info_page_count = sizeof(info_pages) / sizeof(info_pages[0]);",
        "",
    ])
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser(description="Generate static Texel info pages")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        pages = parse_pages(args.input)
        generated = generate(pages, args.assets)
        write_if_changed(args.output, generated.encode("utf-8"))
    except (InputError, OSError) as error:
        print(f"info page generation failed: {error}", file=sys.stderr)
        return 1
    print(f"Generated {len(pages)} info pages in {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Mono8 raw -> PNG. По умолчанию 9344x7000, либо width/height из соседнего .txt."""

import argparse
import sys
from pathlib import Path

from PIL import Image

DEFAULT_W = 9344
DEFAULT_H = 7000


def load_meta(path: Path):
    width = height = None
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            key = key.strip().lower()
            if key == "width":
                width = int(value)
            elif key == "height":
                height = int(value)
            continue
        parts = line.split()
        if len(parts) >= 3 and all(part.lstrip("-").isdigit() for part in parts[:3]):
            width = int(parts[1])
            height = int(parts[2])
    return width, height


def main():
    parser = argparse.ArgumentParser(description="Mono8 raw в PNG")
    parser.add_argument("raw", type=Path)
    parser.add_argument("-o", "--output", type=Path, help="файл PNG (один кадр) или каталог (--all)")
    parser.add_argument("-W", "--width", type=int)
    parser.add_argument("-H", "--height", type=int)
    parser.add_argument("--meta", type=Path, help="txt: width=/height= или «кадры ширина высота fps»")
    parser.add_argument("--frame", type=int, default=0, help="номер кадра с нуля")
    parser.add_argument("--all", action="store_true", help="все кадры, имена stem_0000.png …")
    args = parser.parse_args()

    width, height = args.width, args.height
    meta = args.meta
    if meta is None:
        sibling = args.raw.with_suffix(".txt")
        if sibling.is_file():
            meta = sibling
    if meta is not None:
        if not meta.is_file():
            sys.exit(f"нет метаданных: {meta}")
        meta_w, meta_h = load_meta(meta)
        width = width or meta_w
        height = height or meta_h
    width = width or DEFAULT_W
    height = height or DEFAULT_H
    if width <= 0 or height <= 0:
        sys.exit("ширина и высота должны быть больше 0")

    frame_bytes = width * height
    size = args.raw.stat().st_size
    if size < frame_bytes or size % frame_bytes != 0:
        sys.exit(f"{args.raw}: {size} байт не кратно {width}x{height} ({frame_bytes})")
    count = size // frame_bytes

    if args.all:
        indices = range(count)
    else:
        if args.frame < 0 or args.frame >= count:
            sys.exit(f"кадра {args.frame} нет, в файле {count}")
        indices = range(args.frame, args.frame + 1)

    with args.raw.open("rb") as raw:
        for index in indices:
            raw.seek(index * frame_bytes)
            pixels = raw.read(frame_bytes)
            image = Image.frombytes("L", (width, height), pixels)
            if len(indices) == 1:
                path = args.output or args.raw.with_suffix(".png")
            elif args.output and args.output.suffix.lower() != ".png":
                path = args.output / f"{args.raw.stem}_{index:04d}.png"
            else:
                base = args.output or args.raw.with_suffix(".png")
                path = base.with_name(f"{base.stem}_{index:04d}{base.suffix}")
            path.parent.mkdir(parents=True, exist_ok=True)
            image.save(path)
            print(path)


if __name__ == "__main__":
    main()

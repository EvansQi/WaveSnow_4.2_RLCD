from __future__ import annotations

import collections
import sys
from pathlib import Path

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parents[1]
OUT_HEADER = ROOT / "main" / "penguin_bitmap.h"
OUT_PREVIEW = ROOT / "penguin_preview.png"
OUT_BLINK_PREVIEW = ROOT / "penguin_blink_preview.png"
MAX_W = 208
MAX_H = 220
WHITE_THRESHOLD = 245


def largest_component_bbox(image: Image.Image) -> tuple[int, int, int, int]:
    rgb = image.convert("RGB")
    width, height = rgb.size
    pixels = rgb.load()
    visited = bytearray(width * height)
    best_area = 0
    best_bbox = (0, 0, width - 1, height - 1)

    def idx(x: int, y: int) -> int:
        return y * width + x

    for y in range(height):
        for x in range(width):
            i = idx(x, y)
            if visited[i]:
                continue
            visited[i] = 1
            r, g, b = pixels[x, y]
            if r > WHITE_THRESHOLD and g > WHITE_THRESHOLD and b > WHITE_THRESHOLD:
                continue

            queue = collections.deque([(x, y)])
            area = 0
            min_x = max_x = x
            min_y = max_y = y

            while queue:
                cx, cy = queue.popleft()
                area += 1
                min_x = min(min_x, cx)
                max_x = max(max_x, cx)
                min_y = min(min_y, cy)
                max_y = max(max_y, cy)

                for nx, ny in ((cx - 1, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)):
                    if nx < 0 or ny < 0 or nx >= width or ny >= height:
                        continue
                    ni = idx(nx, ny)
                    if visited[ni]:
                        continue
                    visited[ni] = 1
                    nr, ng, nb = pixels[nx, ny]
                    if nr > WHITE_THRESHOLD and ng > WHITE_THRESHOLD and nb > WHITE_THRESHOLD:
                        continue
                    queue.append((nx, ny))

            if area > best_area:
                best_area = area
                best_bbox = (min_x, min_y, max_x, max_y)

    return best_bbox


def pack_bits(image_1bit: Image.Image) -> list[int]:
    width, height = image_1bit.size
    pixels = image_1bit.load()
    packed: list[int] = []
    for y in range(height):
        byte = 0
        bits = 0
        for x in range(width):
            black = 1 if pixels[x, y] == 0 else 0
            byte = (byte << 1) | black
            bits += 1
            if bits == 8:
                packed.append(byte)
                byte = 0
                bits = 0
        if bits:
            byte <<= 8 - bits
            packed.append(byte)
    return packed


def create_blink_frame(image_1bit: Image.Image) -> Image.Image:
    blink = image_1bit.copy()
    pixels = blink.load()

    def fill_rect(x0: int, y0: int, x1: int, y1: int, value: int) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if 0 <= x < blink.width and 0 <= y < blink.height:
                    pixels[x, y] = value

    fill_rect(74, 74, 99, 106, 255)
    fill_rect(122, 74, 149, 106, 255)
    fill_rect(76, 88, 97, 92, 0)
    fill_rect(124, 88, 147, 92, 0)
    return blink


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python tools/generate_penguin_bitmap.py <image_path>")
        return 1

    src = Path(sys.argv[1])
    image = Image.open(src).convert("RGB")

    min_x, min_y, max_x, max_y = largest_component_bbox(image)
    pad = 18
    crop = image.crop(
        (
            max(0, min_x - pad),
            max(0, min_y - pad),
            min(image.width, max_x + pad + 1),
            min(image.height, max_y + pad + 1),
        )
    )

    if crop.width <= MAX_W and crop.height <= MAX_H:
        scale = max(1, min(MAX_W // crop.width, MAX_H // crop.height))
        if scale > 1:
            crop = crop.resize((crop.width * scale, crop.height * scale), Image.Resampling.NEAREST)
    else:
        crop.thumbnail((MAX_W, MAX_H), Image.Resampling.NEAREST)
    gray = ImageOps.autocontrast(crop.convert("L"), cutoff=1)
    mono = gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)

    blink = create_blink_frame(mono)
    packed_open = pack_bits(mono)
    packed_blink = pack_bits(blink)
    width, height = mono.size
    bytes_per_row = (width + 7) // 8

    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"constexpr int kPenguinWidth = {width};")
    lines.append(f"constexpr int kPenguinHeight = {height};")
    lines.append(f"constexpr int kPenguinBytesPerRow = {bytes_per_row};")
    lines.append("static const uint8_t kPenguinBitmapOpen[] = {")

    for i in range(0, len(packed_open), 12):
        chunk = packed_open[i : i + 12]
        lines.append("    " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")

    lines.append("};")
    lines.append("")
    lines.append("static const uint8_t kPenguinBitmapBlink[] = {")

    for i in range(0, len(packed_blink), 12):
        chunk = packed_blink[i : i + 12]
        lines.append("    " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")

    lines.append("};")
    lines.append("")
    OUT_HEADER.write_text("\n".join(lines), encoding="ascii")
    mono.save(OUT_PREVIEW)
    blink.save(OUT_BLINK_PREVIEW)
    print(f"generated {OUT_HEADER}")
    print(f"preview   {OUT_PREVIEW}")
    print(f"blink     {OUT_BLINK_PREVIEW}")
    print(f"bitmap    {width}x{height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import math
import sys
from pathlib import Path
from collections import deque

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUT_PIXEL = ROOT / "bead_pixel_art.png"
OUT_PREVIEW = ROOT / "bead_pixel_art_preview.png"

# Tuned for the provided 40x39 bead pattern screenshot.
GRID_LEFT = 56
GRID_TOP = 373
GRID_RIGHT = 1215
GRID_BOTTOM = 1504
COLS = 40
ROWS = 39
CELL_INSET = 5
PREVIEW_SCALE = 18

# Approximate palette sampled from the legend / cells.
PALETTE = [
    (255, 255, 255),
    (28, 25, 29),
    (85, 87, 103),
    (205, 195, 167),
    (175, 160, 131),
    (252, 221, 197),
    (226, 181, 125),
    (145, 84, 62),
    (190, 110, 70),
    (240, 160, 168),
]


def average_cell_color(image: Image.Image, x0: int, y0: int, x1: int, y1: int) -> tuple[int, int, int]:
    pixels = image.load()
    total_r = total_g = total_b = count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = pixels[x, y]
            # Ignore grid lines and text strokes as much as possible.
            if r < 20 and g < 20 and b < 20:
                continue
            if r > 245 and g > 245 and b > 245:
                continue
            total_r += r
            total_g += g
            total_b += b
            count += 1
    if count == 0:
        return (255, 255, 255)
    return (round(total_r / count), round(total_g / count), round(total_b / count))


def nearest_palette_color(color: tuple[int, int, int]) -> tuple[int, int, int]:
    best = PALETTE[0]
    best_distance = float("inf")
    for swatch in PALETTE:
        distance = math.sqrt(
            (color[0] - swatch[0]) ** 2
            + (color[1] - swatch[1]) ** 2
            + (color[2] - swatch[2]) ** 2
        )
        if distance < best_distance:
            best_distance = distance
            best = swatch
    return best


def keep_largest_component(image: Image.Image) -> Image.Image:
    width, height = image.size
    pixels = image.load()
    visited = [[False] * width for _ in range(height)]
    white = (255, 255, 255)
    best_component: list[tuple[int, int]] = []

    for y in range(height):
        for x in range(width):
            if visited[y][x] or pixels[x, y] == white:
                continue
            queue = deque([(x, y)])
            visited[y][x] = True
            component: list[tuple[int, int]] = []

            while queue:
                cx, cy = queue.popleft()
                component.append((cx, cy))
                for nx, ny in ((cx - 1, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)):
                    if nx < 0 or ny < 0 or nx >= width or ny >= height:
                        continue
                    if visited[ny][nx] or pixels[nx, ny] == white:
                        continue
                    visited[ny][nx] = True
                    queue.append((nx, ny))

            if len(component) > len(best_component):
                best_component = component

    cleaned = Image.new("RGB", image.size, white)
    cleaned_pixels = cleaned.load()
    for x, y in best_component:
        cleaned_pixels[x, y] = pixels[x, y]
    return cleaned


def main() -> int:
    if len(sys.argv) != 2:
      print("usage: python tools/extract_bead_pattern.py <image_path>")
      return 1

    src = Path(sys.argv[1])
    image = Image.open(src).convert("RGB")
    grid_width = GRID_RIGHT - GRID_LEFT
    grid_height = GRID_BOTTOM - GRID_TOP
    cell_w = grid_width / COLS
    cell_h = grid_height / ROWS

    pixel_art = Image.new("RGB", (COLS, ROWS), (255, 255, 255))
    for row in range(ROWS):
        for col in range(COLS):
            left = GRID_LEFT + int(round(col * cell_w)) + CELL_INSET
            top = GRID_TOP + int(round(row * cell_h)) + CELL_INSET
            right = GRID_LEFT + int(round((col + 1) * cell_w)) - CELL_INSET
            bottom = GRID_TOP + int(round((row + 1) * cell_h)) - CELL_INSET
            color = average_cell_color(image, left, top, right, bottom)
            pixel_art.putpixel((col, row), nearest_palette_color(color))

    pixel_art = keep_largest_component(pixel_art)
    preview = pixel_art.resize((COLS * PREVIEW_SCALE, ROWS * PREVIEW_SCALE), Image.Resampling.NEAREST)

    # Add a tiny border so the shape does not stick to preview edges.
    framed = Image.new("RGB", (preview.width + 24, preview.height + 24), (255, 255, 255))
    framed.paste(preview, (12, 12))

    pixel_art.save(OUT_PIXEL)
    framed.save(OUT_PREVIEW)
    print(f"pixel   {OUT_PIXEL}")
    print(f"preview {OUT_PREVIEW}")
    print(f"size    {pixel_art.size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

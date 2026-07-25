#!/usr/bin/env python3
"""
SPDX-License-Identifier: MIT

Generates assets/images/title_logo.png: a 3D isometric-block ASCII-art
rendering of "LORA" / "MESSENGER" for the title screen brand panel
(src/view/screens/messenger_screen.cpp's render_menu()), replacing the
plain lv_label wordmark and the app-icon image that used to sit next to it.

This mirrors the Battleship sibling project's own
tools/generate_title_logo.py (same repo family, same CardputerZero
320x170 target): each letter is authored as a 5x7 dot-matrix glyph
(FONT_5X7 below), and every "on" dot is drawn as a small cube with 3
faces (front, top, right-side) using isometric-style parallelogram
bevels. Adjacent "on" dots fuse into solid blocks because the top/side
bevels are only drawn on the *silhouette edge* of the glyph, not on every
individual cell -- see render_line() for the exact algorithm.

Why pre-rendered PNG instead of an LVGL label: LVGL's built-in fonts are
proportional and have no built-in 3D/bevel rendering, so this look can
only be produced offline. Pillow output is displayed via lv_image
(LV_USE_LIBPNG=1, LV_USE_FS_STDIO drive letter 'A' -- both already
enabled in the app's lv_conf), same mechanism already used for the
messenger envelope icon this logo replaces.

Two lines ("LORA" then "MESSENGER") are rendered independently at the
same letter scale, then stacked and centered so the wordmark reads
naturally in the brand panel's portrait-ish leftover space once the icon
is removed -- a single-line "LORAMESSENGER" would be too long to stay
legible at 320x170.

Color choice: the face color (101, 214, 180) matches view::app_palette()'s
accent (0x65D6B4, src/view/theme.cpp) so the logo reads as an intentional
accent against the app's fixed dark-navy body (0x0B1020); a heavy
near-black outline keeps every glyph edge crisp against the panel
background (0x151D31) too.

Requirements (installed on demand into a local venv, not vendored):
    pip install pillow

Usage:
    python3 tools/generate_title_logo.py [--preview]
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT_PATH = REPO_ROOT / "assets" / "images" / "title_logo.png"

LINES = ["LORA", "MESSENGER"]
GLYPH_COLS, GLYPH_ROWS = 5, 7

# Hand-authored 5x7 dot-matrix bitmap font (rows top->bottom, '1' = lit
# cell), classic LED-sign style. Only the 9 distinct letters needed by
# "LORA" + "MESSENGER" are defined (L O R A M E S N G).
FONT_5X7 = {
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "N": ["10001", "11001", "10101", "10101", "10011", "10001", "10001"],
    "G": ["01111", "10000", "10000", "10011", "10001", "10001", "01110"],
}

# Max PNG dimensions the brand panel budgets for the logo (see
# messenger_screen.cpp: brand panel is 306x78 within a 320x170 screen,
# with the "OFFLINE BROADCAST" tagline rendered below it).
MAX_W, MAX_H = 286, 46

PALETTE = dict(
    face=(101, 214, 180, 255),
    top=(200, 245, 232, 255),
    side=(24, 90, 74, 255),
    outline=(5, 14, 12, 255),
)


def _on(grid: list[str], r: int, c: int) -> bool:
    if 0 <= r < GLYPH_ROWS and 0 <= c < GLYPH_COLS:
        return grid[r][c] == "1"
    return False


def render_line(
    text: str,
    face: tuple[int, int, int, int],
    top: tuple[int, int, int, int],
    side: tuple[int, int, int, int],
    outline: tuple[int, int, int, int],
    cell: int = 6,
    depth: int = 3,
    letter_gap: int = 1,
    supersample: int = 4,
) -> Image.Image:
    ss = supersample
    cell_px = cell * ss
    depth_px = depth * ss

    letter_col_offsets = []
    total_cols = 0
    for _ in text:
        letter_col_offsets.append(total_cols)
        total_cols += GLYPH_COLS + letter_gap

    width = total_cols * cell_px + depth_px + 4 * ss
    height = GLYPH_ROWS * cell_px + depth_px + 4 * ss
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    origin_x, origin_y = depth_px + 2 * ss, depth_px + 2 * ss

    def cell_origin(base_col: int, r: int, c: int) -> tuple[int, int]:
        return origin_x + (base_col + c) * cell_px, origin_y + r * cell_px

    # Pass 1: heavy outline -- redraw every face/bevel offset in a ring of
    # directions so a crisp dark border surrounds the whole glyph silhouette.
    for li, ch in enumerate(text):
        grid = FONT_5X7[ch]
        base_col = letter_col_offsets[li]
        for r in range(GLYPH_ROWS):
            for c in range(GLYPH_COLS):
                if grid[r][c] != "1":
                    continue
                x0, y0 = cell_origin(base_col, r, c)
                for dx in (-ss, 0, ss):
                    for dy in (-ss, 0, ss):
                        draw.rectangle(
                            [x0 + dx, y0 + dy, x0 + cell_px - 1 + dx, y0 + cell_px - 1 + dy],
                            fill=outline,
                        )
                if not _on(grid, r - 1, c):
                    for dx in (-ss, 0, ss):
                        for dy in (-ss, 0, ss):
                            draw.polygon(
                                [
                                    (x0 + dx, y0 + dy),
                                    (x0 + depth_px + dx, y0 - depth_px + dy),
                                    (x0 + cell_px - 1 + depth_px + dx, y0 - depth_px + dy),
                                    (x0 + cell_px - 1 + dx, y0 + dy),
                                ],
                                fill=outline,
                            )
                if not _on(grid, r, c + 1):
                    for dx in (-ss, 0, ss):
                        for dy in (-ss, 0, ss):
                            draw.polygon(
                                [
                                    (x0 + cell_px - 1 + dx, y0 + dy),
                                    (x0 + cell_px - 1 + depth_px + dx, y0 - depth_px + dy),
                                    (x0 + cell_px - 1 + depth_px + dx, y0 + cell_px - 1 - depth_px + dy),
                                    (x0 + cell_px - 1 + dx, y0 + cell_px - 1 + dy),
                                ],
                                fill=outline,
                            )

    # Pass 2: front faces (solid fill).
    for li, ch in enumerate(text):
        grid = FONT_5X7[ch]
        base_col = letter_col_offsets[li]
        for r in range(GLYPH_ROWS):
            for c in range(GLYPH_COLS):
                if grid[r][c] == "1":
                    x0, y0 = cell_origin(base_col, r, c)
                    draw.rectangle([x0, y0, x0 + cell_px - 1, y0 + cell_px - 1], fill=face)

    # Pass 3: top bevels, only on the silhouette's top edge (no 'on'
    # neighbor directly above) so adjoining cells fuse into solid blocks.
    for li, ch in enumerate(text):
        grid = FONT_5X7[ch]
        base_col = letter_col_offsets[li]
        for r in range(GLYPH_ROWS):
            for c in range(GLYPH_COLS):
                if grid[r][c] == "1" and not _on(grid, r - 1, c):
                    x0, y0 = cell_origin(base_col, r, c)
                    draw.polygon(
                        [
                            (x0, y0),
                            (x0 + depth_px, y0 - depth_px),
                            (x0 + cell_px - 1 + depth_px, y0 - depth_px),
                            (x0 + cell_px - 1, y0),
                        ],
                        fill=top,
                    )

    # Pass 4: right-side bevels, only on the silhouette's right edge.
    for li, ch in enumerate(text):
        grid = FONT_5X7[ch]
        base_col = letter_col_offsets[li]
        for r in range(GLYPH_ROWS):
            for c in range(GLYPH_COLS):
                if grid[r][c] == "1" and not _on(grid, r, c + 1):
                    x0, y0 = cell_origin(base_col, r, c)
                    draw.polygon(
                        [
                            (x0 + cell_px - 1, y0),
                            (x0 + cell_px - 1 + depth_px, y0 - depth_px),
                            (x0 + cell_px - 1 + depth_px, y0 + cell_px - 1 - depth_px),
                            (x0 + cell_px - 1, y0 + cell_px - 1),
                        ],
                        fill=side,
                    )

    return img.resize((width // ss, height // ss), Image.LANCZOS)


def render_wordmark(lines: list[str], palette: dict, line_gap: int = 3) -> Image.Image:
    """Renders each line at a shared letter scale, then stacks and
    center-aligns them, downscaling the whole group together (not each
    line separately) so 'LORA' and 'MESSENGER' keep the same glyph size."""
    raw = [render_line(text, **palette) for text in lines]
    total_h = sum(im.height for im in raw) + line_gap * (len(raw) - 1)
    max_line_w = max(im.width for im in raw)

    scale = min(MAX_W / max_line_w, MAX_H / total_h, 1.0)
    if scale < 1.0:
        raw = [
            im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.LANCZOS)
            for im in raw
        ]
        total_h = sum(im.height for im in raw) + line_gap * (len(raw) - 1)
        max_line_w = max(im.width for im in raw)

    canvas = Image.new("RGBA", (max_line_w, total_h), (0, 0, 0, 0))
    y = 0
    for im in raw:
        x = (max_line_w - im.width) // 2
        canvas.alpha_composite(im, (x, y))
        y += im.height + line_gap
    return canvas


def save_preview(img: Image.Image, path: Path) -> None:
    """Composite over the app's fixed dark-navy body/panel colors
    (view::app_palette(), src/view/theme.cpp: 0x0B1020 body, 0x151D31
    panel) for a legibility check against the real background."""
    body_bg = Image.new("RGBA", img.size, (0x0B, 0x10, 0x20, 255))
    panel_bg = Image.new("RGBA", img.size, (0x15, 0x1D, 0x31, 255))
    gap = 10
    preview = Image.new("RGBA", (img.width, img.height * 2 + gap), (40, 40, 40, 255))
    body_bg.alpha_composite(img)
    panel_bg.alpha_composite(img)
    preview.paste(body_bg, (0, 0))
    preview.paste(panel_bg, (0, img.height + gap))
    preview.save(path)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true", help="also write a body/panel preview PNG")
    args = parser.parse_args()

    img = render_wordmark(LINES, PALETTE)
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUTPUT_PATH)
    print(f"{OUTPUT_PATH} ({img.width}x{img.height})")

    if args.preview:
        preview_path = OUTPUT_PATH.with_name("title_logo_preview.png")
        save_preview(img, preview_path)
        print(f"preview -> {preview_path}")


if __name__ == "__main__":
    main()

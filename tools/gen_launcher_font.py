#!/usr/bin/env python3
"""
Generate an embedded 512x512 8-bit alpha texture atlas C header for the pre-game
OpenGL 2.1 Launcher UI supporting ASCII (0x20..0x7E) and Russian Cyrillic
(U+0401, U+0451, U+0410..U+044F).
"""

import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT)
    parser.add_argument("--output", type=Path,
                        default=PROJECT_ROOT / "src/runtime/launcher_font_atlas.h")
    args = parser.parse_args()
    atlas_w, atlas_h = 512, 512
    font_size = 24
    font = ImageFont.truetype(str(args.font), font_size)
    ascent, descent = font.getmetrics()

    codepoints = list(range(0x20, 0x7F)) + [0x0401, 0x0451] + list(range(0x0410, 0x0450))
    atlas = Image.new("L", (atlas_w, atlas_h), 0)
    draw = ImageDraw.Draw(atlas)

    cx, cy = 2, 2
    row_h = 0
    glyphs = []

    for cp in codepoints:
        ch = chr(cp)
        bbox = font.getbbox(ch)
        adv = font.getlength(ch)
        if bbox is None:
            glyphs.append((cp, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, float(adv)))
            continue
        x0, y0, x1, y1 = bbox
        gw = max(0, int(x1 - x0))
        gh = max(0, int(y1 - y0))
        if gw == 0 or gh == 0:
            glyphs.append((cp, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, float(adv)))
            continue

        if cx + gw + 4 >= atlas_w:
            cx = 2
            cy += row_h + 4
            row_h = 0
        assert cy + gh + 2 < atlas_h, "Atlas overflow"

        # Draw character cleanly with 1px padding baked via x0/y0 baseline origin
        draw.text((cx - x0, cy - y0), ch, font=font, fill=255)
        u0 = cx / float(atlas_w)
        v0 = cy / float(atlas_h)
        u1 = (cx + gw) / float(atlas_w)
        v1 = (cy + gh) / float(atlas_h)
        glyphs.append((cp, u0, v0, u1, v1, float(x0), float(y0), float(gw), float(gh), float(adv)))

        cx += gw + 4
        if gh > row_h:
            row_h = gh

    raw = atlas.tobytes()

    with args.output.open("w", encoding="utf-8") as f:
        f.write("#ifndef RECOMP_LAUNCHER_FONT_ATLAS_H\n")
        f.write("#define RECOMP_LAUNCHER_FONT_ATLAS_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define LAUNCHER_ATLAS_W {atlas_w}\n")
        f.write(f"#define LAUNCHER_ATLAS_H {atlas_h}\n")
        f.write(f"#define LAUNCHER_FONT_SIZE {font_size}\n")
        f.write(f"#define LAUNCHER_FONT_ASCENT {ascent}\n")
        f.write(f"#define LAUNCHER_FONT_LINE_H {ascent + descent}\n\n")
        f.write("typedef struct LauncherGlyph {\n")
        f.write("    uint32_t cp;\n")
        f.write("    float u0, v0, u1, v1;\n")
        f.write("    float xoff, yoff, w, h, xadv;\n")
        f.write("} LauncherGlyph;\n\n")
        f.write(f"static const LauncherGlyph k_launcher_glyphs[{len(glyphs)}] = {{\n")
        for g in glyphs:
            cp, u0, v0, u1, v1, xoff, yoff, w, h, adv = g
            f.write(f"    {{ 0x{cp:04x}u, {u0:.6f}f, {v0:.6f}f, {u1:.6f}f, {v1:.6f}f, {xoff:.1f}f, {yoff:.1f}f, {w:.1f}f, {h:.1f}f, {adv:.2f}f }},\n")
        f.write("};\n\n")
        f.write(f"static const int k_launcher_glyph_count = {len(glyphs)};\n\n")
        f.write(f"static const uint8_t k_launcher_atlas[{len(raw)}] = {{\n")
        for i in range(0, len(raw), 16):
            chunk = ",".join(str(b) for b in raw[i:i+16])
            f.write(f"    {chunk},\n")
        f.write("};\n\n")
        f.write("#endif /* RECOMP_LAUNCHER_FONT_ATLAS_H */\n")

    print(f"Generated {args.output}: {len(glyphs)} glyphs, packed height {cy + row_h}/{atlas_h}")


if __name__ == "__main__":
    main()

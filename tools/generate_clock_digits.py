from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT_C = ROOT / "main" / "clock_digit_images.c"
PREVIEW = ROOT / "tools" / "clock_digits_preview.png"
FONT_PATH = Path(r"C:\Windows\Fonts\bahnschrift.ttf")
FONT_VARIATION = "Bold"

DIGIT_W = 160
DIGIT_H = 414
GLYPH_MAX_W = 150
GLYPH_H = 400
FONT_MAX_WIDTH = 296
POSITIONS = (-20, 100, 220, 340)
Y_POSITIONS = (-8, 56, 8, 56)

BACKGROUND = (0xE7, 0xED, 0xF0)
DIGIT_COLOR = (0x08, 0x7A, 0x5B)


def render_digit(digit: str) -> Image.Image:
    font = ImageFont.truetype(str(FONT_PATH), 520)
    font.set_variation_by_name(FONT_VARIATION)
    scratch = Image.new("L", (620, 620), 0)
    draw = ImageDraw.Draw(scratch)
    bbox = draw.textbbox((0, 0), digit, font=font, stroke_width=0)
    draw.text((-bbox[0], -bbox[1]), digit, font=font, fill=255)
    glyph = scratch.crop(scratch.getbbox())
    proportional_w = glyph.width / FONT_MAX_WIDTH * GLYPH_MAX_W
    glyph_w = round(proportional_w * 0.5 + GLYPH_MAX_W * 0.5)
    glyph = glyph.resize((glyph_w, GLYPH_H), Image.Resampling.LANCZOS)

    canvas = Image.new("L", (DIGIT_W, DIGIT_H), 0)
    canvas.paste(glyph, ((DIGIT_W - glyph_w) // 2, (DIGIT_H - GLYPH_H) // 2))
    return canvas


def format_bytes(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 20):
        chunk = data[offset : offset + 20]
        rows.append("    " + ",".join(f"0x{value:02x}" for value in chunk) + ",")
    return "\n".join(rows)


def build_palette() -> tuple[list[tuple[int, int, int, int]], bytes]:
    colors = [(0, 0, 0, 0)]
    for level in range(1, 8):
        alpha = round(level / 7 * 255)
        colors.append((255, 255, 255, alpha))
    for level in range(1, 9):
        mix = level / 8
        red = round(255 + (DIGIT_COLOR[0] - 255) * mix)
        green = round(255 + (DIGIT_COLOR[1] - 255) * mix)
        blue = round(255 + (DIGIT_COLOR[2] - 255) * mix)
        colors.append((red, green, blue, 255))

    raw = bytearray()
    for red, green, blue, alpha in colors:
        raw.extend((blue * alpha // 255, green * alpha // 255, red * alpha // 255, alpha))
    return colors, bytes(raw)


def to_i4(mask: Image.Image) -> tuple[Image.Image, bytes]:
    outline = mask.filter(ImageFilter.MaxFilter(7))
    colors, palette = build_palette()
    indices = bytearray()
    mask_px = mask.load()
    outline_px = outline.load()
    preview = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    preview_px = preview.load()
    for y in range(mask.height):
        row = []
        for x in range(mask.width):
            core = mask_px[x, y]
            edge = outline_px[x, y]
            if core:
                index = 8 + min(7, round(core / 255 * 7))
            elif edge >= 96:
                index = 7
            else:
                index = 0
            row.append(index)
            preview_px[x, y] = colors[index]
        for x in range(0, mask.width, 2):
            indices.append((row[x] << 4) | row[x + 1])
    return preview, palette + bytes(indices)


def main() -> None:
    masks = [render_digit(str(value)) for value in range(10)]
    digits = [to_i4(mask) for mask in masks]
    parts = [
        '#include "lvgl.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
    ]
    for value, (_, data) in enumerate(digits):
        parts.extend(
            [
                "static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST",
                f"uint8_t clock_digit_{value}_map[] = {{",
                format_bytes(data),
                "};",
                "",
                f"static const lv_image_dsc_t clock_digit_{value} = {{",
                "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
                "    .header.cf = LV_COLOR_FORMAT_I4,",
                "    .header.flags = LV_IMAGE_FLAGS_PREMULTIPLIED,",
                f"    .header.w = {DIGIT_W},",
                f"    .header.h = {DIGIT_H},",
                f"    .header.stride = {DIGIT_W // 2},",
                f"    .data_size = sizeof(clock_digit_{value}_map),",
                f"    .data = clock_digit_{value}_map,",
                "};",
                "",
            ]
        )
    parts.extend(
        [
            "const lv_image_dsc_t *const clock_digit_images[10] = {",
            "    &clock_digit_0, &clock_digit_1, &clock_digit_2, &clock_digit_3, &clock_digit_4,",
            "    &clock_digit_5, &clock_digit_6, &clock_digit_7, &clock_digit_8, &clock_digit_9,",
            "};",
            "",
        ]
    )
    OUT_C.write_text("\n".join(parts), encoding="ascii")

    preview = Image.new("RGB", (480, 480), BACKGROUND)
    for digit, x, y in zip((0, 8, 1, 6), POSITIONS, Y_POSITIONS):
        preview.paste(digits[digit][0], (x, y), digits[digit][0])
    PREVIEW.parent.mkdir(parents=True, exist_ok=True)
    preview.save(PREVIEW)


if __name__ == "__main__":
    main()

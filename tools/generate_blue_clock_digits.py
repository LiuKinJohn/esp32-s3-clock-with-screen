import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT_C = ROOT / "main" / "blue_clock_digit_images.c"
PREVIEW = ROOT / "tools" / "blue_clock_preview.png"
FONT_PATH = Path(os.environ["BLUE_CLOCK_FONT"])

DIGIT_W = 140
DIGIT_H = 240
GLYPH_MAX_W = 130
GLYPH_MAX_H = 228
DIGIT_X = (4, 104, 240, 336)
DIGIT_Y = 120
LIGHT_BLUE = (0x51, 0xA2, 0xED)
DARK_BLUE = (0x01, 0x62, 0xBA)
COLON_MIN = 0xB8
COLON_MAX = 0xD8


def render_digit(value: str) -> Image.Image:
    font = ImageFont.truetype(str(FONT_PATH), 520)
    scratch = Image.new("L", (700, 700), 0)
    draw = ImageDraw.Draw(scratch)
    stroke_width = 30
    bbox = draw.textbbox((0, 0), value, font=font, stroke_width=stroke_width)
    draw.text((-bbox[0], -bbox[1]), value, font=font, fill=255,
              stroke_width=stroke_width, stroke_fill=255)
    glyph = scratch.crop(scratch.getbbox())
    scale = GLYPH_MAX_H / glyph.height
    target_width = min(GLYPH_MAX_W, round(glyph.width * scale * 0.90))
    glyph = glyph.resize(
        (target_width, GLYPH_MAX_H),
        Image.Resampling.LANCZOS,
    )
    # Slight expansion gives the broad, friendly weight used by the reference.
    glyph = glyph.filter(ImageFilter.MaxFilter(3))
    canvas = Image.new("L", (DIGIT_W, DIGIT_H), 0)
    canvas.paste(
        glyph,
        ((DIGIT_W - glyph.width) // 2, (DIGIT_H - glyph.height) // 2),
    )
    return canvas


def format_bytes(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 20):
        chunk = data[offset : offset + 20]
        rows.append("    " + ",".join(f"0x{value:02x}" for value in chunk) + ",")
    return "\n".join(rows)


def premultiplied_palette(color: tuple[int, int, int]) -> bytes:
    raw = bytearray((0, 0, 0, 0))
    for level in range(1, 16):
        alpha = round(level / 15 * 255)
        red, green, blue = color
        raw.extend(
            (
                blue * alpha // 255,
                green * alpha // 255,
                red * alpha // 255,
                alpha,
            )
        )
    return bytes(raw)


def mask_to_i4(mask: Image.Image, color: tuple[int, int, int]) -> tuple[Image.Image, bytes]:
    indices = bytearray()
    pixels = mask.load()
    preview = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    preview_pixels = preview.load()
    for y in range(mask.height):
        row = []
        for x in range(mask.width):
            alpha = pixels[x, y]
            index = min(15, round(alpha / 255 * 15))
            row.append(index)
            preview_pixels[x, y] = (*color, round(index / 15 * 255))
        for x in range(0, mask.width, 2):
            indices.append((row[x] << 4) | row[x + 1])
    return preview, premultiplied_palette(color) + bytes(indices)


def render_colon() -> Image.Image:
    scale = 4
    width, height = 58, 200
    image = Image.new("RGBA", (width * scale, height * scale), (0, 0, 0, 0))
    pixels = image.load()
    centers = ((29 * scale, 48 * scale), (29 * scale, 152 * scale))
    radius = 27 * scale
    for cx, cy in centers:
        for y in range(max(0, cy - radius), min(image.height, cy + radius + 1)):
            for x in range(max(0, cx - radius), min(image.width, cx + radius + 1)):
                dx = (x - cx) / radius
                dy = (y - cy) / radius
                distance = (dx * dx + dy * dy) ** 0.5
                if distance > 1:
                    continue
                shade = max(0.0, min(1.0, 0.5 + 0.5 * (-dx - dy) / 2))
                edge = max(0.0, min(1.0, (1.0 - distance) * 10))
                value = round(COLON_MIN + (COLON_MAX - COLON_MIN) * shade)
                pixels[x, y] = (value, value, value, round(255 * edge))
    return image.resize((width, height), Image.Resampling.LANCZOS)


def rgba_to_i4(image: Image.Image) -> bytes:
    colors = [(0, 0, 0, 0)]
    for level in range(1, 16):
        value = round(COLON_MIN + (COLON_MAX - COLON_MIN) * level / 15)
        colors.append((value, value, value, 255))
    palette = bytearray()
    for red, green, blue, alpha in colors:
        palette.extend((blue * alpha // 255, green * alpha // 255, red * alpha // 255, alpha))
    indices = bytearray()
    pixels = image.load()
    for y in range(image.height):
        row = []
        for x in range(image.width):
            red, green, blue, alpha = pixels[x, y]
            if alpha < 8:
                index = 0
            else:
                brightness = (red + green + blue) / 3
                index = max(1, min(15, round((brightness - COLON_MIN) / (COLON_MAX - COLON_MIN) * 14) + 1))
            row.append(index)
        for x in range(0, image.width, 2):
            indices.append((row[x] << 4) | row[x + 1])
    return bytes(palette) + bytes(indices)


def descriptor(name: str, width: int, height: int, data: bytes) -> list[str]:
    return [
        "static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST",
        f"uint8_t {name}_map[] = {{",
        format_bytes(data),
        "};",
        "",
        f"static const lv_image_dsc_t {name} = {{",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        "    .header.cf = LV_COLOR_FORMAT_I4,",
        "    .header.flags = LV_IMAGE_FLAGS_PREMULTIPLIED,",
        f"    .header.w = {width},",
        f"    .header.h = {height},",
        f"    .header.stride = {width // 2},",
        f"    .data_size = sizeof({name}_map),",
        f"    .data = {name}_map,",
        "};",
        "",
    ]


def main() -> None:
    masks = [render_digit(str(value)) for value in range(10)]
    light = [mask_to_i4(mask, LIGHT_BLUE) for mask in masks]
    dark = [mask_to_i4(mask, DARK_BLUE) for mask in masks]
    colon = render_colon()
    colon_data = rgba_to_i4(colon)

    parts = [
        '#include "lvgl.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
    ]
    for value, (_, data) in enumerate(light):
        parts.extend(descriptor(f"blue_clock_light_{value}", DIGIT_W, DIGIT_H, data))
    for value, (_, data) in enumerate(dark):
        parts.extend(descriptor(f"blue_clock_dark_{value}", DIGIT_W, DIGIT_H, data))
    parts.extend(descriptor("blue_clock_colon", colon.width, colon.height, colon_data))
    parts.extend(
        [
            "const lv_image_dsc_t *const blue_clock_light_images[10] = {",
            "    &blue_clock_light_0, &blue_clock_light_1, &blue_clock_light_2, &blue_clock_light_3, &blue_clock_light_4,",
            "    &blue_clock_light_5, &blue_clock_light_6, &blue_clock_light_7, &blue_clock_light_8, &blue_clock_light_9,",
            "};",
            "",
            "const lv_image_dsc_t *const blue_clock_dark_images[10] = {",
            "    &blue_clock_dark_0, &blue_clock_dark_1, &blue_clock_dark_2, &blue_clock_dark_3, &blue_clock_dark_4,",
            "    &blue_clock_dark_5, &blue_clock_dark_6, &blue_clock_dark_7, &blue_clock_dark_8, &blue_clock_dark_9,",
            "};",
            "",
            "const lv_image_dsc_t *const blue_clock_colon_image = &blue_clock_colon;",
            "",
        ]
    )
    OUT_C.write_text("\n".join(parts), encoding="ascii")

    preview_canvas = Image.new("RGB", (480, 480), (0, 0, 0))
    for index, (digit, x) in enumerate(zip((0, 9, 4, 1), DIGIT_X)):
        layer = light[digit][0] if index % 2 == 0 else dark[digit][0]
        preview_canvas.paste(layer, (x, DIGIT_Y), layer)
    preview_canvas.paste(colon, (211, 140), colon)
    preview_canvas.save(PREVIEW)


if __name__ == "__main__":
    main()

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FONT_DIR = Path.home() / "AppData/Local/Microsoft/Windows/Fonts"
FONTS = [
    ("LINE Seed Sans XBd", FONT_DIR / "LINESeedSans_XBd.ttf", 0),
    ("Montserrat Black", FONT_DIR / "Montserrat-Black.ttf", 0),
    ("Cal Sans SemiBold", FONT_DIR / "CalSans-SemiBold.otf", 0),
    ("Chill Expanded Bold", FONT_DIR / "ChillDuanSans_ExpandedBold.otf", 0),
    ("Google Sans synthetic", FONT_DIR / "[Black]苹方 GoogleSans常规(+15).ttf", 13),
    ("Poppins Bold", FONT_DIR / "Poppins-Bold.ttf", 0),
]
OUT = Path(__file__).with_name("blue_clock_font_comparison.png")


def main() -> None:
    canvas = Image.new("RGB", (960, 750), "black")
    draw = ImageDraw.Draw(canvas)
    for index, (name, path, stroke) in enumerate(FONTS):
        x = (index % 2) * 480
        y = (index // 2) * 250
        font = ImageFont.truetype(str(path), 140)
        draw.text((x + 12, y + 8), name, font=ImageFont.truetype(str(path), 24), fill="white")
        draw.text((x + 10, y + 45), "23:50", font=font, fill=(0x58, 0xA8, 0xE8),
                  stroke_width=stroke, stroke_fill=(0x58, 0xA8, 0xE8), spacing=0)
    canvas.save(OUT)


if __name__ == "__main__":
    main()

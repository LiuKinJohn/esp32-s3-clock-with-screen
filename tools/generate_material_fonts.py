#!/usr/bin/env python3
"""Generate compact LVGL fonts for the Material You clock interface."""

import argparse
import subprocess
from pathlib import Path


UI_CHINESE = (
    "时间仪表盘设置无线网络校时左右滑动切换页面屏幕亮度界面语言显示主题格式自动息屏"
    "间隔时区当前下次清除启动手机配网立即长按重启恢复已连接中离线等待选择刷新取消密码永不"
    "浅色深色昼夜小时分钟年月日星期一二三四五六农历正冬腊初十廿闰"
    "金鐘港島綫往堅尼地城柴灣站台即將到分鐘服務暫停資料已過期最後更新天氣香港公園港鐵天文台"
    "正在暫無列車到站方向來源晴多雲驟雨雷暴"
)

ETA_DISPLAY_CHINESE = "金鐘港島綫往堅尼地城柴灣站台即將到站分鐘服務暫停"
PIDS_DISPLAY_GLYPHS = "金鐘上水港島綫東鐵綫往堅尼地城柴灣羅湖落馬洲即將抵達分鐘暫無列車資料尾班已開出天氣晴多雲驟雨雷暴／☀☁☂"


def generate(node: Path, converter: Path, roboto: Path, noto: Path, output: Path,
             size: int, chinese: str) -> None:
    command = [
        str(node), str(converter),
        "--font", str(roboto), "-r", "0x20-0x7E",
    ]
    if chinese:
        command += ["--font", str(noto), "--symbols", "".join(sorted(set(chinese)))]
    command += [
        "--size", str(size),
        "--bpp", "2",
        "--no-compress",
        "--no-prefilter",
        "--format", "lvgl",
        "--output", str(output),
        "--force-fast-kern-format",
    ]
    subprocess.run(command, check=True)

    source = output.read_text(encoding="utf-8")
    source = source.replace('#include "lvgl.h"', '#include "lvgl.h"\n\nLV_FONT_DECLARE(lv_font_chinese_16);', 1)
    source = source.replace(".fallback = NULL,", ".fallback = &lv_font_chinese_16,")
    output.write_text(source, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--node", required=True, type=Path)
    parser.add_argument("--converter", required=True, type=Path)
    parser.add_argument("--roboto", required=True, type=Path)
    parser.add_argument("--noto", required=True, type=Path)
    parser.add_argument("--pids-serif", type=Path)
    parser.add_argument("--pids-latin", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    specs = (
        (14, UI_CHINESE),
        (16, UI_CHINESE),
        (20, UI_CHINESE),
        (28, ETA_DISPLAY_CHINESE),
        (48, ""),
    )
    for size, chinese in specs:
        generate(args.node, args.converter, args.roboto, args.noto,
                 args.output_dir / f"lv_font_ui_{size}.c", size, chinese)
    if args.pids_serif:
        pids_latin = args.pids_latin or args.roboto
        for size in (16, 20, 28):
            generate(args.node, args.converter, pids_latin, args.pids_serif,
                     args.output_dir / f"lv_font_pids_{size}.c", size, PIDS_DISPLAY_GLYPHS)


if __name__ == "__main__":
    main()

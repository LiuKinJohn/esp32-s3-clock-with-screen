#!/usr/bin/env python3
"""Generate the LVGL GB2312 UI font used by the clock firmware."""

import argparse
import subprocess
from pathlib import Path


def gb2312_characters() -> str:
    characters = set()
    for lead in range(0xA1, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                characters.add(bytes((lead, trail)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(sorted(characters))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--node", required=True, type=Path)
    parser.add_argument("--converter", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--symbols-font", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    symbols = gb2312_characters() + "℃•"
    lvgl_symbols = (
        "61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,"
        "61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,"
        "61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,"
        "61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,"
        "61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,"
        "62018,62019,62020,62087,62099,62189,62212,62810,63426,63650"
    )
    command = [
        str(args.node),
        str(args.converter),
        "--font",
        str(args.font),
        "-r",
        "0x20-0x7E",
        "--symbols",
        symbols,
        "--font",
        str(args.symbols_font),
        "-r",
        lvgl_symbols,
        "--size",
        "16",
        "--bpp",
        "2",
        "--no-compress",
        "--no-prefilter",
        "--format",
        "lvgl",
        "--output",
        str(args.output),
        "--force-fast-kern-format",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()

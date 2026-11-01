#!/usr/bin/env python3
"""Builds the bundled icon font from Material Symbols: pins every axis except FILL, which the host sets per icon.
It also checks that each icon SfSymbols.kt names exists in the font.

usage: tools/build-icon-font.py FONT.ttf FONT.codepoints
The inputs are MaterialSymbolsOutlined[FILL,GRAD,opsz,wght] from github.com/google/material-design-icons. Needs fonttools.
"""
import pathlib
import re
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLE = ROOT / 'app/src/main/java/moe/tenshii/iosrunner/SfSymbols.kt'
OUTPUT = ROOT / 'app/src/main/assets/fonts/MaterialSymbolsOutlined.ttf'


def main():
    font_path, codepoints_path = sys.argv[1:3]
    names = {line.split()[0] for line in open(codepoints_path)}
    missing = sorted(set(re.findall(r' to "([a-z0-9_]+)"', TABLE.read_text())) - names)
    if missing:
        sys.exit('not in the font: ' + ', '.join(missing))
    font = instancer.instantiateVariableFont(TTFont(font_path), {'wght': 400, 'GRAD': 0, 'opsz': 24})
    font.save(OUTPUT)
    print(f'{OUTPUT.stat().st_size} bytes')


if __name__ == '__main__':
    main()

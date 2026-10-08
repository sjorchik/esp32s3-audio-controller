#!/usr/bin/env python3
"""Генератор великого цифрового шрифту (GFXfont) для спливного вікна параметра.

Символи: пробіл, '+', '-', '0'..'9'. Цифри DejaVu Sans табличні (однакова
ширина), але xAdvance ВСІХ символів примусово вирівнюється до ширини цифри,
щоб значення не «стрибало». Використовує build() із tools/gen_gfxfont.py.

Використання (з кореня проєкту, одним рядком):
    python3 tools/gen_digits_font.py --ttf tools/fonts/DejaVuSans.ttf --out src/ui/font_digits_data.h --em 80
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_gfxfont as g  # noqa: E402

CODES = [0x20, 0x2B, 0x2D] + list(range(0x30, 0x3A))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--em", type=int, default=80)
    args = ap.parse_args()

    g.RANGES = [(c, c) for c in CODES]
    f = g.build(args.ttf, "Digits", args.em)

    # Моноширинність: xAdvance усіх символів = xAdvance цифри '0'.
    digit_adv = f["glyphs"][0x30 - f["first"]][3]
    glyphs = []
    for i, (off, w, h, adv, xo, yo) in enumerate(f["glyphs"]):
        code = f["first"] + i
        if code in CODES:
            # центруємо вузькі гліфи ('+', '-') в комірці
            xo = max(0, (digit_adv - w) // 2) if w else 0
            adv = digit_adv
        glyphs.append((off, w, h, adv, xo, yo))
    f["glyphs"] = glyphs

    g.emit([f], args.ttf.replace("\\", "/").split("/")[-1], args.out)
    # Шапка: emit() пише ім'я базового генератора й «лише з ui/fonts.cpp» — уточнюємо.
    txt = Path(args.out).read_text(encoding="utf-8")
    txt = txt.replace("ЗГЕНЕРОВАНО tools/gen_gfxfont.py", "ЗГЕНЕРОВАНО tools/gen_digits_font.py", 1)
    Path(args.out).write_text(txt, encoding="utf-8")
    zero = glyphs[0x30 - f["first"]]
    asc, _ = g.ImageFont.truetype(args.ttf, args.em).getmetrics()
    print("Digits: em=%d yAdvance=%d cellW=%d digitH=%d ascent=%d topInset=%d bitmap=%d B" %
          (args.em, f["y_advance"], digit_adv, zero[2], asc, asc + zero[5], len(f["bitmap"])))


if __name__ == "__main__":
    sys.exit(main())

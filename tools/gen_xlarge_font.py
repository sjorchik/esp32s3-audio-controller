#!/usr/bin/env python3
"""Генератор шрифту FontSize::XLarge (назва станції на екрані Radio, Prompt 35).

Той самий набір символів, що й у Large/Small/Tiny (ASCII, Latin-1, Latin Extended-A,
кирилиця U+0400..U+045F, Ґ ґ): діапазони беруться з tools/gen_gfxfont.py без змін.
Наявний ui/font_data.h НЕ чіпається — результат іде в окремий ui/font_xlarge_data.h
(ті самі правила, що й для tools/gen_digits_font.py). Використовує build() і emit()
із tools/gen_gfxfont.py.

Використання (з кореня проєкту, одним рядком):
    python3 tools/gen_xlarge_font.py --ttf tools/fonts/DejaVuSans.ttf --out src/ui/font_xlarge_data.h --em 30

Звірити надрукований yAdvance із display_cfg::kFontXLargePx (за замовчуванням em=30 -> 36).
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_gfxfont as g  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--em", type=int, default=30)
    ap.add_argument("--preview")
    args = ap.parse_args()

    f = g.build(args.ttf, "XLarge", args.em)
    g.emit([f], args.ttf.replace("\\", "/").split("/")[-1], args.out)
    # Шапка: emit() пише ім'я базового генератора — уточнюємо.
    txt = Path(args.out).read_text(encoding="utf-8")
    txt = txt.replace("ЗГЕНЕРОВАНО tools/gen_gfxfont.py", "ЗГЕНЕРОВАНО tools/gen_xlarge_font.py", 1)
    Path(args.out).write_text(txt, encoding="utf-8")
    if args.preview:
        g.make_preview([f], args.preview)
    print("XLarge: em=%d yAdvance=%d bitmap=%d B glyph-table=%d entries (~%d B)" %
          (f["em"], f["y_advance"], len(f["bitmap"]), len(f["glyphs"]), len(f["glyphs"]) * 7))


if __name__ == "__main__":
    sys.exit(main())

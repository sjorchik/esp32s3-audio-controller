#!/usr/bin/env python3
"""Генератор растрових шрифтів у форматі Adafruit/LovyanGFX GFXfont з TTF.

Аналог fontconvert, але з підтримкою Unicode-діапазонів (кирилиця) і без
залежності від FreeType-CLI: потрібні лише Python 3 та Pillow.

Використання (з кореня проєкту):
    python3 tools/gen_gfxfont.py \
        --ttf tools/fonts/DejaVuSans.ttf \
        --out src/ui/font_data.h \
        --preview tools/font_preview.png

Розміри (em у пікселях) підбираються так, щоб висота рядка (yAdvance)
приблизно збігалась із display_cfg::kFont{Large,Small,Tiny}Px.

Шрифт: DejaVu Sans (Bitstream Vera / Arev, вільна ліцензія, дозволяє
вбудовування й розповсюдження за умови збереження повідомлення про ліцензію —
воно вставляється в шапку згенерованого файлу).
Покриття: ASCII, Latin-1, Latin Extended-A, вся кирилиця U+0400..U+045F
(включно з Ґ ґ Є є І і Ї ї Ў ў), плюс U+0490/0491 (Ґ ґ).
"""

import argparse
import sys

from PIL import Image, ImageDraw, ImageFont

# (ім'я, em-розмір у пікселях)
SIZES = [("Large", 20), ("Small", 14), ("Tiny", 11)]

# Діапазони кодів, які реально малюємо; решта в таблиці [first..last] — порожні.
RANGES = [
    (0x20, 0x7E),      # ASCII
    (0xA0, 0x17F),     # Latin-1 + Latin Extended-A
    (0x400, 0x45F),    # кирилиця (Ё, Є, І, Ї, Ў, ... і малі)
    (0x490, 0x491),    # Ґ ґ
]

LICENSE_NOTE = """\
// DejaVu Fonts License (витяг). Шрифти DejaVu — похідні Bitstream Vera.
// Copyright (c) 2003 Bitstream, Inc.; зміни DejaVu (c) 2006 Tavmjong Bah.
// Дозволяється використовувати, копіювати, змінювати й розповсюджувати шрифти
// (у тому числі вбудованими в програмне забезпечення) без плати за умови, що
// це повідомлення про авторські права й ліцензію збережено в супровідних
// матеріалах; назви "Bitstream" і "Vera" не можна використовувати для
// похідних шрифтів без дозволу. Повний текст: https://dejavu-fonts.github.io/License.html
"""


def code_list():
    codes = []
    for lo, hi in RANGES:
        codes.extend(range(lo, hi + 1))
    return codes


def render_glyph(font, ch):
    """Повертає (bitmap_rows, xOffset, yOffset, xAdvance); bitmap_rows — список рядків 0/1
    або None для порожнього гліфа. Координати відносно точки baseline-left."""
    adv = int(round(font.getlength(ch)))
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return None, 0, 0, adv
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"  # без згладжування: чіткий 1-бітний рендер (з хінтингом)
    d.text((-x0, -y0), ch, font=font, anchor="ls", fill=255)
    bb = img.getbbox()
    if bb is None:
        return None, 0, 0, adv
    img = img.crop(bb)
    rows = []
    for y in range(img.height):
        rows.append([1 if img.getpixel((x, y)) >= 128 else 0 for x in range(img.width)])
    return rows, x0 + bb[0], y0 + bb[1], adv


def pack_bits(rows):
    bits = [b for row in rows for b in row]
    out = bytearray()
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8]
        v = 0
        for b in chunk:
            v = (v << 1) | b
        v <<= 8 - len(chunk)
        out.append(v)
    return out


def build(ttf, name, em):
    font = ImageFont.truetype(ttf, em)
    asc, desc = font.getmetrics()
    y_advance = asc + desc
    codes = set(code_list())
    first, last = min(codes), max(codes)
    space_adv = int(round(font.getlength(" ")))

    bitmap = bytearray()
    glyphs = []  # (offset, w, h, xAdv, xOff, yOff)
    preview = {}
    for code in range(first, last + 1):
        if code not in codes:
            glyphs.append((0, 0, 0, 0, 0, 0))
            continue
        ch = chr(code)
        rows, xo, yo, adv = render_glyph(font, ch)
        if rows is None:
            glyphs.append((0, 0, 0, adv, 0, 0))
            continue
        h, w = len(rows), len(rows[0])
        assert w < 256 and h < 256 and -128 <= xo < 128 and -128 <= yo < 128 and adv < 256
        glyphs.append((len(bitmap), w, h, adv, xo, yo))
        bitmap.extend(pack_bits(rows))
        preview[code] = rows
    assert len(bitmap) < 65536, "bitmapOffset does not fit uint16"
    return dict(name=name, em=em, first=first, last=last, y_advance=y_advance,
                bitmap=bitmap, glyphs=glyphs, space_adv=space_adv)


def emit(fonts, ttf_name, out):
    L = []
    L.append("#pragma once")
    L.append("")
    L.append("// ЗГЕНЕРОВАНО tools/gen_gfxfont.py — НЕ РЕДАГУВАТИ ВРУЧНУ.")
    L.append("// Джерело: %s. Формат: Adafruit/LovyanGFX GFXfont (1 біт на піксель)." % ttf_name)
    L.append("// Підключається ЛИШЕ з ui/fonts.cpp (масиви static).")
    L.append("//")
    L.extend(LICENSE_NOTE.rstrip("\n").split("\n"))
    L.append("")
    L.append("#include <LovyanGFX.hpp>")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("namespace font_data {")
    L.append("")
    for f in fonts:
        n = f["name"]
        L.append("// --- %s: em=%dpx, yAdvance=%d, коди 0x%04X..0x%04X ---" %
                 (n, f["em"], f["y_advance"], f["first"], f["last"]))
        L.append("constexpr uint16_t k%sFirst = 0x%04X;" % (n, f["first"]))
        L.append("constexpr uint16_t k%sLast = 0x%04X;" % (n, f["last"]))
        L.append("constexpr uint8_t k%sYAdvance = %d;" % (n, f["y_advance"]))
        L.append("")
        L.append("static const uint8_t k%sBitmaps[] PROGMEM = {" % n)
        bm = f["bitmap"]
        for i in range(0, len(bm), 16):
            L.append("    " + ", ".join("0x%02X" % b for b in bm[i:i + 16]) + ",")
        L.append("};")
        L.append("")
        L.append("static const lgfx::GFXglyph k%sGlyphs[] PROGMEM = {" % n)
        for i, g in enumerate(f["glyphs"]):
            code = f["first"] + i
            L.append("    {%d, %d, %d, %d, %d, %d}, // 0x%04X" % (*g, code))
        L.append("};")
        L.append("")
    L.append("}  // namespace font_data")
    L.append("")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L))


def make_preview(fonts, path):
    """Малює тестові рядки з БАЙТІВ згенерованих таблиць (а не з TTF) —
    перевіряє саме те, що піде в прошивку."""
    text_lines = [
        "Ґрунт Їжачок Єдність",
        "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ",
        "абвгґдеєжзиіїйклмнопрстуфхцчшщьюя",
        "Radio 101.5 FM — Abc xyz 0123456789",
    ]
    scale = 3
    W = 1000
    H = 20
    for f in fonts:
        H += (f["y_advance"] + 4) * len(text_lines)
    img = Image.new("RGB", (W, H), (0, 0, 0))
    px = img.load()
    y = 10
    for f in fonts:
        for line in text_lines:
            base = y + f["y_advance"] * 3 // 4 + 4
            x = 6
            for ch in line:
                code = ord(ch)
                if not (f["first"] <= code <= f["last"]):
                    continue
                off, w, h, adv, xo, yo = f["glyphs"][code - f["first"]]
                bits = []
                nbytes = (w * h + 7) // 8
                for b in f["bitmap"][off:off + nbytes]:
                    for k in range(7, -1, -1):
                        bits.append((b >> k) & 1)
                for gy in range(h):
                    for gx in range(w):
                        if bits[gy * w + gx]:
                            X, Y = x + xo + gx, base + yo + gy
                            if 0 <= X < W // scale * 0 + W and 0 <= Y < H:
                                px[X, Y] = (255, 255, 255)
                x += adv
            y += f["y_advance"] + 4
    img = img.resize((W * 1, H * 1), Image.NEAREST)
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview")
    args = ap.parse_args()

    fonts = [build(args.ttf, n, em) for n, em in SIZES]
    emit(fonts, args.ttf.split("/")[-1], args.out)
    if args.preview:
        make_preview(fonts, args.preview)
    for f in fonts:
        print("%s: em=%d yAdvance=%d bitmap=%d B glyph-table=%d entries" %
              (f["name"], f["em"], f["y_advance"], len(f["bitmap"]), len(f["glyphs"])))


if __name__ == "__main__":
    sys.exit(main())

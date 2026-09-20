#!/usr/bin/env python3
"""Конвертер Terminus BDF -> заголовок в духе формата Adafruit GFX.

Зачем свой конвертер, а не fontconvert. Штатная утилита Adafruit_GFX
(fontconvert) читает шрифты через FreeType, а FreeType растеризует контуры —
у Terminus контуров нет, только готовые растры пикселей в BDF. Пропустить BDF
через FreeType — значит дать рендерер повторно решить, куда лёг каждый пиксель,
то есть заново получить именно ту проблему, ради которой Terminus вообще
выбран (см. docs/decisions.md, п.1 и п.6: дробный масштаб рвёт штрихи, «В»
превращается в «D»). Поэтому здесь простой текстовый разбор BDF и прямая
переупаковка его битов.

Отличие от «чистого» Adafruit GFX. Стандартный GFXfont хранит один сплошной
диапазон кодов [first, last] и требует записи (пусть пустой) на каждый код
внутри диапазона. Кириллица (U+0400) и нужные с экрана символы вроде ▲▼→≈
(U+2190..U+FFFF) лежат от ASCII настолько далеко, что сплошной диапазон
означал бы десятки тысяч пустых записей ради полусотни нужных глифов. Поэтому
GFXglyph здесь — ровно тот же набор полей, что в Adafruit_GFX (bitmapOffset/
width/height/xAdvance/xOffset/yOffset), а GFXfont вместо first/last хранит
отсортированный массив кодов, параллельный массиву глифов, — бинарный поиск
вместо вычитания first. Про это же — комментарий в font.h и README.

Формат бит в bitmap — тот же, что у настоящего Adafruit GFX: сплошной поток
бит на весь глиф (без выравнивания по границе строки внутри глифа, в отличие
от BDF, где каждая строка растра дополняется нулями до байта), с довыравниванием
только в конце глифа, чтобы следующий глиф в общем массиве начинался с байта.
Так header остаётся честным «форматом Adafruit GFX», а не только похожим по
именам полей.

Запуск: python3 convert_font.py
Перегенерирует все 5 заголовков в firmware/assets/ из firmware/assets/terminus-src/*.bdf.
"""
from __future__ import annotations

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent
SRC = ROOT / "terminus-src"

# Кегли ровно из родной сетки Terminus (docs/decisions.md, п.6) — промежуточные
# размеры в BDF просто не существуют, поэтому взять «плохой» размер тут нельзя
# даже случайно.
SIZES = {
    12: "ter-u12n.bdf",
    14: "ter-u14n.bdf",
    16: "ter-u16n.bdf",
    20: "ter-u20n.bdf",
    24: "ter-u24n.bdf",
}

# Печатная латиница/цифры/пунктуация — то, чем набраны числа, единицы, "USB" и т.п.
ASCII_RANGE = range(0x20, 0x7F)
# Базовая кириллица: русские прописные/строчные + Ё/ё. Полный блок 0x400-0x45F
# берём целиком, а не точечно по буквам, — русский текст на экране не ограничен
# фиксированным списком слов (сообщения почты, названия событий календаря).
CYRILLIC_RANGE = range(0x400, 0x460)
# Точечные символы вербстки: тире и многоточие в подписях, градус и номер,
# средняя точка-разделитель ("КАБИНЕТ · ВОЗДУХ"), приближённое равно у
# устаревших значений, треугольники дельты, стрелка "кадр -> следующий".
EXTRA_CODEPOINTS = [
    0x00B0,  # °
    0x00B7,  # ·
    0x2013,  # –
    0x2014,  # —
    0x2019,  # ’
    0x2026,  # …
    0x2082,  # ₂ (подстрочная 2 — «CO₂», как в эталоне)
    0x2116,  # №
    0x2192,  # →
    0x2248,  # ≈
    0x25B2,  # ▲
    0x25BC,  # ▼
]

WANTED = sorted(set(ASCII_RANGE) | set(CYRILLIC_RANGE) | set(EXTRA_CODEPOINTS))


class Glyph:
    __slots__ = ("code", "dwidth", "bbx_w", "bbx_h", "bbx_xoff", "bbx_yoff", "rows")

    def __init__(self):
        self.code = None
        self.dwidth = 0
        self.bbx_w = self.bbx_h = self.bbx_xoff = self.bbx_yoff = 0
        self.rows: list[int] = []


def parse_bdf(path: pathlib.Path) -> tuple[int, dict[int, Glyph]]:
    """Возвращает (line_height, {codepoint: Glyph}). line_height — из
    FONTBOUNDINGBOX, это gfxFont.yAdvance — межстрочное расстояние."""
    text = path.read_text(encoding="latin-1")
    lines = text.splitlines()

    line_height = None
    glyphs: dict[int, Glyph] = {}
    cur: Glyph | None = None
    in_bitmap = False
    row_bytes = 0

    for raw in lines:
        line = raw.strip()
        if line.startswith("FONTBOUNDINGBOX"):
            _, w, h, xo, yo = line.split()
            line_height = int(h)
        elif line.startswith("STARTCHAR"):
            cur = Glyph()
            in_bitmap = False
        elif line.startswith("ENCODING"):
            cur.code = int(line.split()[1])
        elif line.startswith("DWIDTH"):
            cur.dwidth = int(line.split()[1])
        elif line.startswith("BBX"):
            _, w, h, xo, yo = line.split()
            cur.bbx_w, cur.bbx_h, cur.bbx_xoff, cur.bbx_yoff = int(w), int(h), int(xo), int(yo)
            row_bytes = (cur.bbx_w + 7) // 8
        elif line == "BITMAP":
            in_bitmap = True
        elif line == "ENDCHAR":
            if cur.code is not None and cur.code in WANTED:
                glyphs[cur.code] = cur
            cur = None
            in_bitmap = False
        elif in_bitmap and cur is not None:
            raw_value = int(line, 16) if line else 0
            # BDF дополняет каждую строку растра нулями до границы байта справа
            # (младшими битами) — значимы только старшие bbx_w бит.
            total_bits = row_bytes * 8
            row = raw_value >> (total_bits - cur.bbx_w) if cur.bbx_w else 0
            cur.rows.append(row)

    assert line_height is not None, f"{path}: не нашли FONTBOUNDINGBOX"
    return line_height, glyphs


def pack_bits(bits: list[int]) -> bytes:
    """Сплошной битовый поток -> байты, довыравнивание нулями в конце."""
    out = bytearray()
    for i in range(0, len(bits), 8):
        chunk = bits[i : i + 8]
        byte = 0
        for j, b in enumerate(chunk):
            byte |= (b & 1) << (7 - j)
        out.append(byte)
    return bytes(out)


def build_font(size: int, path: pathlib.Path):
    line_height, glyphs = parse_bdf(path)

    bitmap = bytearray()
    entries = []  # (code, bitmapOffset, width, height, xAdvance, xOffset, yOffset)

    for code in WANTED:
        g = glyphs.get(code)
        if g is None or g.bbx_w == 0 or g.bbx_h == 0:
            # Глиф не найден в исходнике — не рисуем вовсе (см. font.h:
            # draw_text молча пропускает такой код), а не подставляем плейсхолдер:
            # рамка «квадрат вместо буквы» на весь текст была бы хуже одной
            # пропущенной пометки контроля.
            continue
        bits = []
        for row in g.rows:
            for i in range(g.bbx_w - 1, -1, -1):
                bits.append((row >> i) & 1)
        packed = pack_bits(bits)
        offset = len(bitmap)
        bitmap.extend(packed)
        # Формула перевода начала координат BDF (низ-лево от базовой линии,
        # ось Y вверх) в формат Adafruit GFX (курсор на базовой линии, ось Y
        # вниз, yOffset — расстояние от курсора до верхней строки растра):
        # верх bbox в координатах BDF = bbx_yoff + height - 1, а перевод в
        # ось-вниз — смена знака.
        y_offset = -(g.bbx_yoff + g.bbx_h - 1)
        entries.append((code, offset, g.bbx_w, g.bbx_h, g.dwidth, g.bbx_xoff, y_offset))

    return line_height, bitmap, entries


def emit_header(size: int, line_height: int, bitmap: bytes, entries: list[tuple], out_path: pathlib.Path):
    sym = f"Terminus{size}"

    lines = []
    lines.append(f"// Автосгенерировано: firmware/assets/convert_font.py из terminus-src/ter-u{size:02d}n.bdf")
    lines.append("// Не редактировать руками — правки живут в конвертере или в исходном BDF.")
    lines.append("// Terminus, (C) Dimitar Toshkov Zhekov, SIL Open Font License 1.1 (см. OFL.TXT).")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "../src/font.h"')
    lines.append("")
    lines.append("namespace fonts {")
    lines.append("")
    lines.append(f"inline constexpr uint8_t {sym}Bitmap[] = {{")
    for i in range(0, len(bitmap), 16):
        chunk = bitmap[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr GFXglyph {sym}Glyphs[] = {{")
    for code, offset, w, h, adv, xo, yo in entries:
        lines.append(
            f"    {{ {offset}, {w}, {h}, {adv}, {xo}, {yo} }},  // U+{code:04X}"
        )
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr uint16_t {sym}Codepoints[] = {{")
    for i in range(0, len(entries), 12):
        chunk = entries[i : i + 12]
        lines.append("    " + ", ".join(f"0x{code:04X}" for code, *_ in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr GFXfont {sym} = {{")
    lines.append(f"    {sym}Bitmap, {sym}Glyphs, {sym}Codepoints,")
    lines.append(f"    {len(entries)}, {line_height}")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace fonts")
    lines.append("")

    out_path.write_text("\n".join(lines), encoding="utf-8")


def main():
    for size, filename in SIZES.items():
        path = SRC / filename
        line_height, bitmap, entries = build_font(size, path)
        out_path = ROOT / f"terminus_{size}.h"
        emit_header(size, line_height, bytes(bitmap), entries, out_path)
        print(f"{filename}: {len(entries)} глифов, {len(bitmap)} байт растра -> {out_path.name}")


if __name__ == "__main__":
    main()

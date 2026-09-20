#!/usr/bin/env python3
"""Конвертер IBM Plex Mono Bold (TrueType) -> заголовок в духе формата Adafruit GFX.

Зачем второй шрифт. Terminus (convert_font.py) — растровый, у него нет
промежуточных кеглей: сетка останавливается на 12/14/16/20/24, а эталону
(cockpit.html, класс `.num`) нужны 41/28/25/20/16/14px для крупных чисел —
курса BTC, курсов валют, CO₂/TVOC, температуры, процентов лимитов, времени
события. Ближайший кегль Terminus для этих мест либо мельче эталонного, либо
требует кратного увеличения (как BTC — Terminus24×2), которое не даёт точных
пиксельных размеров. Эталон использует для этих мест другой шрифт — IBM Plex
Mono, вес 700 (Bold), — здесь тот же выбор.

Почему растеризация через PIL, а не FreeType-обвязку fontconvert. У IBM Plex
Mono, в отличие от Terminus, есть настоящие векторные контуры — фактически
`fontconvert` тут вполне применим. Но: (1) его ещё нужно собрать (C-утилита из
Adafruit_GFX, не входит в PlatformIO-зависимости этого проекта), (2) PIL уже
стоит в системе и умеет ровно то же самое — растеризовать контур в битмап
заданного пиксельного кегля через FreeType под капотом. Рваные штрихи от
растеризации контура (docs/decisions.md, п.1 и п.6) — проблема ДРОБНОГО
масштаба и МЕЛКИХ кеглей, а не растеризации как таковой: все кегли здесь между
14 и 41px, целые и достаточно крупные, чтобы FreeType-хinting не искажал форму
буквы визуально заметно. Поэтому расширять список зависимостей ради
fontconvert смысла нет.

`ImageFont.getbbox(text)`/`ImageDraw.text(..., font=f)` с явным mode="1"
(1-битный, не 8-битный серый) дают то же самое, что было бы нужно от
`FT_LOAD_TARGET_MONO` в FreeType напрямую, — чёткий чёрно-белый растр без
полутонов, никакого порога/дизеринга приделывать не нужно.

Формат заголовков и упаковка бит — побайтово тот же, что у convert_font.py
(см. его комментарий и firmware/src/font.h): GFXglyph с теми же полями,
codepoints[] вместо сплошного диапазона [first,last] (здесь диапазон и так
короткий, но общий движок fonts::find_glyph/draw_text один на оба шрифта —
нет смысла заводить второй формат ради экономии одного бинарного поиска).

Набор символов — минимальный, ровно то, что печатает этим шрифтом layout.cpp
(класс `.num` в cockpit.html — только числа, не текст): цифры, `.,:+-%°/`,
пробел, ≈ (пометка устаревшего значения), — (длинное тире — прочерк пустого
слота, `format_percent`/`format_value` возвращают именно его: в cockpit.html
прочерк лежит внутри того же `<span class="num">`, что и число, значит и в
шрифте нужен его глиф, а не только Terminus'ов). Кириллица и латинские буквы
сюда не идут — весь текст вокруг чисел (подписи, единицы измерения, даты)
остаётся на Terminus. ▲▼ (дельты, U+25B2/U+25BC) в задании тоже упомянуты как
нужные числам символы, но в самом IBM Plex Mono их нет вовсе (проверено через
`fontTools.ttLib.TTFont.getBestCmap()` — оба кода отсутствуют в cmap): в
эталоне треугольники дельты и так рисуются классом `.delta`, а не `.num`, то
есть Terminus, а не этим шрифтом — совпадает с тем, что уже делает layout.cpp
(`format_delta`/`format_delta_per_hour` рисуются `fonts::Terminus14`, не
Plex). Не запрошенный здесь символ просто не встретится в реальном тексте.

Запуск: python3 convert_ibmplexmono.py
Перегенерирует все шесть заголовков в firmware/assets/ из
firmware/assets/ibmplexmono-src/IBMPlexMono-Bold.ttf.
"""
from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent
TTF_PATH = ROOT / "ibmplexmono-src" / "IBMPlexMono-Bold.ttf"

# Кегли — ровно те, что использует .num в cockpit.html (см. docs/architecture.md
# и Status Log задачи): 41 — курс BTC (доминанта кадра), 28 — CO₂/TVOC и
# температура погоды, 25 — курсы USD/RUB и EUR/RUB, 20 — процент лимита в
# одиночном окне (GPT), 16 — процент лимита в парных окнах (Claude, 5ч/неделя),
# 14 — время события в календаре. В отличие от Terminus, TrueType-контур можно
# растрировать в любой пиксельный размер без калек «промежуточного кегля» —
# сетки, привязанной к конкретному размеру, здесь просто нет.
SIZES = [14, 16, 20, 25, 28, 41]

# Коды символов, которые реально печатает числами layout.cpp этим шрифтом:
# пробел, цифры, точка/запятая (десятичный разделитель — format_decimal меняет
# точку на запятую, но точка оставлена про запас), двоеточие (время события
# "10:30"), плюс/минус (знак температуры), процент, градус, слэш (не
# используется на сегодня ни одним форматтером, но входит в явный список
# символов задачи про число), приближённое равно (пометка устаревшего значения
# — format_value/format_percent), длинное тире (прочерк пустого слота — тоже
# format_value/format_percent, U+2014, НЕ путать с hyphen-minus U+002D выше).
CODEPOINTS = sorted(
    {
        0x0020,  # пробел
        0x002E,  # .
        0x002C,  # ,
        0x003A,  # :
        0x002B,  # +
        0x002D,  # - (hyphen-minus)
        0x0025,  # %
        0x00B0,  # °
        0x002F,  # /
        0x2248,  # ≈
        0x2014,  # — (em dash, прочерк пустого слота)
    }
    | {ord(str(d)) for d in range(10)}
)


def pack_bits(bits: list[int]) -> bytes:
    """Сплошной битовый поток -> байты, довыравнивание нулями в конце.
    Тот же приём, что в convert_font.py — здесь не импортируется оттуда
    намеренно, чтобы конвертер TTF оставался самостоятельным скриптом, как и
    его BDF-собрат."""
    out = bytearray()
    for i in range(0, len(bits), 8):
        chunk = bits[i : i + 8]
        byte = 0
        for j, b in enumerate(chunk):
            byte |= (b & 1) << (7 - j)
        out.append(byte)
    return bytes(out)


def render_glyph(font: ImageFont.FreeTypeFont, ch: str):
    """Растеризует один символ в 1-битный растр. Возвращает (pixels, left, top,
    width, height) или None, если у символа нет чернил вовсе (например,
    пробел) — тогда глиф всё равно нужен в таблице (иначе draw_text молча
    пропустит его и потеряет продвижение курсора), просто без битов растра."""
    bbox = font.getbbox(ch)
    if bbox is None:
        return None
    left, top, right, bottom = bbox
    w, h = right - left, bottom - top
    if w <= 0 or h <= 0:
        return None
    img = Image.new("1", (w, h), 0)
    draw = ImageDraw.Draw(img)
    # mode="1" у самого шрифта не нужен отдельно — Image в режиме "1" и заливка
    # 1 дают то же самое: FreeType рисует без полутонов, порог не нужен.
    draw.text((-left, -top), ch, font=font, fill=1)
    # img.getdata() отмечена deprecated в Pillow 12+ (уйдёт в 14) — читаем
    # пиксели напрямую через PixelAccess, тот же построчный порядок
    # (строка за строкой, слева направо), которого ждёт blit_glyph в font.cpp.
    px = img.load()
    pixels = [px[x, y] for y in range(h) for x in range(w)]
    return pixels, left, top, w, h


def build_font(size: int):
    font = ImageFont.truetype(str(TTF_PATH), size)
    ascent, _descent = font.getmetrics()

    bitmap = bytearray()
    entries = []  # (code, offset, w, h, xAdvance, xOffset, yOffset)

    for code in CODEPOINTS:
        ch = chr(code)
        adv = round(font.getlength(ch))
        rendered = render_glyph(font, ch)
        if rendered is None:
            # Нет чернил (пробел) — глиф присутствует в таблице без битов
            # растра, только с продвижением курсора. font.h::draw_text
            # (blit_glyph) уже умеет это: `if (g.width == 0 ...) return;`
            # рисует ничего, но xAdvance наверху всё равно применяется.
            entries.append((code, len(bitmap), 0, 0, adv, 0, 0))
            continue
        pixels, left, top, w, h = rendered
        packed = pack_bits(pixels)
        offset = len(bitmap)
        bitmap.extend(packed)
        # top измеряется в системе координат PIL, где y=0 — линия аскендера
        # (верх кегля), а baseline лежит на y=ascent той же системы (проверено
        # эмпирически: для цифры без нижних выносных bbox.bottom == ascent).
        # Adafruit yOffset — расстояние от baseline до верхней строки растра,
        # отрицательное вверх, поэтому top переводится вычитанием ascent.
        y_offset = top - ascent
        entries.append((code, offset, w, h, adv, left, y_offset))

    return bytes(bitmap), entries


def emit_header(size: int, bitmap: bytes, entries: list[tuple], out_path: pathlib.Path):
    sym = f"PlexMono{size}"

    lines = []
    lines.append(f"// Автосгенерировано: firmware/assets/convert_ibmplexmono.py"
                 f" из ibmplexmono-src/IBMPlexMono-Bold.ttf, кегль {size}px.")
    lines.append("// Не редактировать руками — правки живут в конвертере или в исходном TTF.")
    lines.append("// IBM Plex Mono, (C) 2017 IBM Corp., SIL Open Font License 1.1")
    lines.append("// (см. OFL-IBMPlexMono.TXT — отдельный файл лицензии, не путать с OFL.TXT")
    lines.append("// у Terminus: тот же тип лицензии, но правообладатель другой).")
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
    if not bitmap:
        lines.append("    0,  // заглушка: во всех глифах этого кегля нет ни бита растра")
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
    # yAdvance (межстрочный интервал) в этом проекте нигде не читается кодом
    # рисования (layout.cpp сам расставляет строки в пикселях) — записывается
    # для полноты формата и на будущее, как у Terminus. Берём ascent+descent
    # шрифта на этом кегле, обычную метрику высоты строки.
    font = ImageFont.truetype(str(TTF_PATH), size)
    ascent, descent = font.getmetrics()
    lines.append(f"    {len(entries)}, {ascent + descent}")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace fonts")
    lines.append("")

    out_path.write_text("\n".join(lines), encoding="utf-8")


def main():
    for size in SIZES:
        bitmap, entries = build_font(size)
        out_path = ROOT / f"plexmono_{size}.h"
        emit_header(size, bitmap, entries, out_path)
        print(f"{size}px: {len(entries)}/{len(CODEPOINTS)} глифов, {len(bitmap)} байт растра"
              f" -> {out_path.name}")


if __name__ == "__main__":
    main()

#include "font.h"

namespace fonts {

const GFXglyph* find_glyph(const GFXfont& font, uint32_t codepoint) {
    if (codepoint > 0xFFFF) return nullptr;  // наш codepoints — uint16_t
    uint16_t code = static_cast<uint16_t>(codepoint);

    int lo = 0;
    int hi = static_cast<int>(font.count) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint16_t v = font.codepoints[mid];
        if (v == code) return &font.glyph[mid];
        if (v < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return nullptr;
}

uint32_t decode_utf8(const char*& p) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(p);
    unsigned char c = s[0];

    if (c < 0x80) {
        p += 1;
        return c;
    }

    int extra;
    uint32_t codepoint;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        codepoint = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        codepoint = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        codepoint = c & 0x07;
    } else {
        // Ведущий байт не по стандарту (например, продолжение 10xxxxxx без
        // начала) — не роняем разбор, отдаём символ замены и сдвигаемся на
        // один байт, чтобы не зациклиться на битой строке.
        p += 1;
        return 0xFFFD;
    }

    for (int i = 1; i <= extra; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            // Оборванная последовательность (конец строки или мусор) —
            // сдвигаемся на один байт, а не на предполагаемую длину, тем же
            // соображением, что и выше.
            p += 1;
            return 0xFFFD;
        }
        codepoint = (codepoint << 6) | (s[i] & 0x3F);
    }
    p += extra + 1;
    return codepoint;
}

// Кладёт один глиф на канву; общая часть draw_text/text_width, только рисующая
// половина не нужна text_width — поэтому вынесена отдельно и вызывается
// условно. scale=1 кладёт по одному пикселю на бит (обычный путь), scale>1 —
// блоком scale×scale: то же самое масштабирование "в лоб", каким Adafruit_GFX
// увеличивает встроенный шрифт через setTextSize(), и оно так же безопасно
// для растра — умножение целого числа пикселей не меняет форму штриха.
static void blit_glyph(canvas::Canvas& canvas, const GFXfont& font, const GFXglyph& g, int16_t x,
                        int16_t y, canvas::Color color, uint8_t scale) {
    if (g.width == 0 || g.height == 0) return;  // пробел и подобные — только advance
    uint32_t total_bits = static_cast<uint32_t>(g.width) * g.height;
    uint16_t byte_index = g.bitmapOffset;
    uint8_t current_byte = 0;

    for (uint32_t i = 0; i < total_bits; ++i) {
        if ((i & 7) == 0) {
            current_byte = font.bitmap[byte_index++];
        }
        if (current_byte & 0x80) {
            uint32_t xx = i % g.width;
            uint32_t yy = i / g.width;
            int16_t px = static_cast<int16_t>(x + (g.xOffset + static_cast<int32_t>(xx)) * scale);
            int16_t py = static_cast<int16_t>(y + (g.yOffset + static_cast<int32_t>(yy)) * scale);
            if (scale == 1) {
                canvas.pixel(px, py, color);
            } else {
                canvas.fill_rect(px, py, scale, scale, color);
            }
        }
        current_byte <<= 1;
    }
}

int16_t draw_text(canvas::Canvas& canvas, const GFXfont& font, int16_t x, int16_t y,
                   const char* utf8, canvas::Color color, uint8_t scale, bool bold) {
    int16_t cursor = x;
    const char* p = utf8;
    while (*p) {
        uint32_t cp = decode_utf8(p);
        const GFXglyph* g = find_glyph(font, cp);
        if (g == nullptr) continue;  // нет глифа — пропускаем символ целиком, без плейсхолдера
        blit_glyph(canvas, font, *g, cursor, y, color, scale);
        if (bold) {
            blit_glyph(canvas, font, *g, static_cast<int16_t>(cursor + 1), y, color, scale);
        }
        cursor = static_cast<int16_t>(cursor + g->xAdvance * scale);
    }
    return static_cast<int16_t>(cursor - x);
}

int16_t text_width(const GFXfont& font, const char* utf8, uint8_t scale) {
    int16_t width = 0;
    const char* p = utf8;
    while (*p) {
        uint32_t cp = decode_utf8(p);
        const GFXglyph* g = find_glyph(font, cp);
        if (g == nullptr) continue;
        width = static_cast<int16_t>(width + g->xAdvance * scale);
    }
    return width;
}

}  // namespace fonts

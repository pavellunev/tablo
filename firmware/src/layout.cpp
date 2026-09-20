// Раскладка — точное воспроизведение структуры cockpit.html (эталон:
// reference/cockpit-reference.png). Числа ниже — не оценка на глаз, а обмер
// эталона тем же способом, каким его потом проверяет tools/compare_frame.py
// (самый длинный сплошной пробег тёмных пикселей в строке/столбце — линия,
// текст такого пробега не даёт). Там же, где эталон один-единственный
// снимок, а не описание, — часть чисел (жёсткие межстрочные интервалы)
// получены как разница координат соседних элементов на НЁМ, и дальше
// применяются как константы шага, а не как абсолютные позиции: если
// какого-то элемента (скажем, BTC) нет, следующий подряд идущий просто
// встаёт на его место — так же, как в cockpit.html скрытый `{% if %}`-блок
// не оставляет пустоты в потоке документа. Подробный разбор — Status Log в
// .claude/plans/inkroam.md.
#include "layout.h"

#include <cstdio>
#include <string>

#include "../assets/terminus_14.h"
#include "../assets/terminus_16.h"
#include "../assets/terminus_20.h"
#include "../assets/terminus_24.h"
#include "../assets/plexmono_14.h"
#include "../assets/plexmono_16.h"
#include "../assets/plexmono_20.h"
#include "../assets/plexmono_25.h"
#include "../assets/plexmono_28.h"
#include "../assets/plexmono_41.h"
#include "font.h"
#include "wifi_qr.h"

namespace layout {

using canvas::Canvas;
using canvas::Color;
using fonts::draw_text;
using fonts::text_width;
using slots::Slot;
using slots::Store;

namespace {

// ── каркас (docs/architecture.md, обмер reference/cockpit-reference.png) ──
constexpr int16_t MARGIN = 15;
constexpr int16_t HEADER_RULE_Y = 49;   // горизонтальная линия под шапкой
constexpr int16_t BODY_TOP = 63;        // y верхнего ряда — эйброу-линия ляжет на 71
constexpr int16_t ROW_DIVIDER_Y = 298;  // линия между верхним и нижним рядом
constexpr int16_t ROW_GAP = 12;         // 310 - 298
constexpr int16_t RATES_WIDTH = 296;    // Рынки — flex:none в cockpit.html
constexpr int16_t TODAY_WIDTH = 202;    // Сегодня — тоже flex:none, но справа
constexpr int16_t TOP_GAP = 19;         // между Рынками и правой колонкой
constexpr int16_t BOTTOM_GAP = 17;      // между Почтой и Сегодня

constexpr int16_t EYEBROW_TEXT_OFFSET = 14;  // r.y -> базовая линия текста эйброу
constexpr int16_t EYEBROW_LINE_OFFSET = 8;   // r.y -> подчёркивающая линия (=71 при r.y=63)

const char* const WEEKDAYS[7] = {"ПН", "ВТ", "СР", "ЧТ", "ПТ", "СБ", "ВС"};
const char* const MONTHS[12] = {"ЯНВАРЯ", "ФЕВРАЛЯ", "МАРТА",   "АПРЕЛЯ", "МАЯ",    "ИЮНЯ",
                                 "ИЮЛЯ",   "АВГУСТА", "СЕНТЯБРЯ", "ОКТЯБРЯ", "НОЯБРЯ", "ДЕКАБРЯ"};

// ── арифметика календаря (алгоритм civil_from_days, H. Hinnant, public
// domain) — не тянем <ctime>/localtime ради пяти чисел и чтобы поведение не
// зависело от libc хоста и таргета одновременно. ──

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

void civil_from_days(int64_t z, int* year, unsigned* month, unsigned* day) {
    z += 719468;
    int64_t era = floor_div(z, 146097);
    unsigned doe = static_cast<unsigned>(z - era * 146097);              // [0, 146096]
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    unsigned mp = (5 * doy + 2) / 153;                       // [0, 11]
    *day = doy - (153 * mp + 2) / 5 + 1;                     // [1, 31]
    *month = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));  // [1, 12]
    *year = static_cast<int>(y + (*month <= 2 ? 1 : 0));
}

// ── форматирование чисел без конкатенации String (см. Arduino.h-шим тестов и
// прецедент connectors.cpp: строки собираются через snprintf/std::string, а
// не через operator+, которого нет в тестовом шиме). ──


// "▼0,37%" — стрелка направления плюс модуль изменения. Общая для BTC и
// USD/RUB, EUR/RUB: в эталоне дельта есть у каждой котировки.
String format_delta(float delta_pct) {
    char buf[24];
    const char* arrow = delta_pct >= 0 ? "▲" : "▼";
    float magnitude = delta_pct >= 0 ? delta_pct : -delta_pct;
    std::snprintf(buf, sizeof(buf), "%s%.2f%%", arrow, static_cast<double>(magnitude));
    for (char* p = buf; *p; ++p) {
        if (*p == '.') *p = ',';
    }
    return String(buf);
}

// Обрезает строку по ширине в пикселях, дописывая многоточие. Режем по
// кодовым точкам, а не по байтам, — иначе можно обрубить середину
// многобайтовой кириллицы.
String truncate_to_width(const fonts::GFXfont& font, const char* utf8, int16_t max_width) {
    if (text_width(font, utf8) <= max_width) return String(utf8);

    const char* ellipsis = "…";
    int16_t ellipsis_w = text_width(font, ellipsis);
    int16_t budget = static_cast<int16_t>(max_width - ellipsis_w);
    if (budget <= 0) return String(ellipsis);

    const char* p = utf8;
    const char* last_good = utf8;
    int16_t width = 0;
    while (*p) {
        uint32_t cp = fonts::decode_utf8(p);
        const fonts::GFXglyph* g = fonts::find_glyph(font, cp);
        int16_t adv = g ? g->xAdvance : 0;
        if (static_cast<int16_t>(width + adv) > budget) break;
        width = static_cast<int16_t>(width + adv);
        last_good = p;
    }
    std::string cut(utf8, static_cast<size_t>(last_good - utf8));
    cut += "…";
    return String(cut.c_str());
}

int16_t clampi(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Спарклайну нужен ряд точек, заканчивающийся текущим значением: history —
// то, что накопил Store::put() до этого замера, number — самый свежий.
uint8_t build_spark(const Slot& s, float* out, uint8_t max_len) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < s.history_len && n < max_len; ++i) out[n++] = s.history[i];
    if (n < max_len) out[n++] = s.number;
    return n;
}

// Сглаживание простым скользящим средним по трём точкам — эталон рисует
// плавную кривую (SVG polyline по большему числу отсчётов, чем у нас в
// истории), у сырых 5-8 точек истории с прямыми отрезками между ними кривая
// выглядит острой пилой. Сглаживание не меняет число точек и не искажает
// крайние (первую/последнюю не трогаем — иначе график «не доходил» бы до
// правого края, а последняя точка обязана быть текущим значением).
void smooth3(const float* in, uint8_t count, float* out) {
    if (count == 0) return;
    out[0] = in[0];
    for (uint8_t i = 1; i + 1 < count; ++i) {
        out[i] = (in[i - 1] + in[i] * 2.0f + in[i + 1]) / 4.0f;
    }
    if (count > 1) out[count - 1] = in[count - 1];
}

// Заливка под кривой регулярным растром. На 1-битной панели это единственный
// способ дать «полупрозрачную» область: сплошная заливка забила бы блок
// чёрным, а без заливки кривая теряется среди прочих линий кадра.
void fill_under_curve(Canvas& c, Rect r, int16_t x, int16_t curve_y) {
    for (int16_t y = static_cast<int16_t>(curve_y + 2); y < r.y + r.h; ++y) {
        // Точечный растр: каждая вторая точка в каждой второй строке.
        // Условие по сумме координат дало бы диагональную штриховку — она
        // спорит с самой кривой и читается как отдельная линия.
        if ((x & 1) == 0 && (y & 1) == 0) {
            c.pixel(x, y, Color::Black);
        }
    }
}

void draw_sparkline(Canvas& c, Rect r, const float* raw_values, uint8_t count,
                    bool fill_below = false) {
    if (count < 2 || r.w <= 1 || r.h <= 1) return;
    float smoothed[Slot::kHistoryCapacity + 1];
    smooth3(raw_values, count, smoothed);
    const float* values = smoothed;

    float lo = values[0], hi = values[0];
    for (uint8_t i = 1; i < count; ++i) {
        if (values[i] < lo) lo = values[i];
        if (values[i] > hi) hi = values[i];
    }
    float range = hi - lo;
    if (range < 1e-6f) range = 1.0f;

    int16_t prev_x = 0, prev_y = 0;
    for (uint8_t i = 0; i < count; ++i) {
        int16_t x = static_cast<int16_t>(r.x + (static_cast<float>(i) / (count - 1)) * (r.w - 1));
        int16_t y = static_cast<int16_t>(r.y + r.h - 1 -
                                          ((values[i] - lo) / range) * (r.h - 1));
        if (i > 0) {
            c.line(prev_x, prev_y, x, y, Color::Black);
            // Заливаем столбцы между предыдущей и текущей точкой, интерполируя
            // высоту кривой: иначе растр отстаёт от линии на крутых участках.
            for (int16_t px = fill_below ? prev_x : x + 1; px <= x; ++px) {
                float t = (x == prev_x) ? 0.0f
                                        : static_cast<float>(px - prev_x) / (x - prev_x);
                int16_t cy = static_cast<int16_t>(prev_y + t * (y - prev_y));
                fill_under_curve(c, r, px, cy);
            }
        }
        prev_x = x;
        prev_y = y;
    }
}

// Сеточная штриховка «в горошек» — как .seg.part (repeating-conic-gradient) у
// эталона: сегмент шкалы, заполненный не целиком, а на дробную часть,
// закрашивается не сплошным чёрным и не пустым контуром, а решёткой из точек.
// Так видно, что это НЕ полный процент и НЕ ноль, — третье состояние, а не
// просто округление.
void fill_hatched(Canvas& c, int16_t x, int16_t y, int16_t w, int16_t h) {
    for (int16_t py = y; py < y + h; py += 2) {
        for (int16_t px = x; px < x + w; px += 2) {
            c.pixel(px, py, Color::Black);
        }
    }
}

// Полоски вместо диаграммы — тот же приём, что .segbar/.seg в cockpit.html:
// сплошная заливка на полностью занятые деления, штриховка на дробный остаток
// (см. fill_hatched), пустой контур на остальные. Зазор между сегментами
// (3px) даёт видимый разрыв на любом разумном их числе.
void draw_segbar(Canvas& c, Rect r, float pct, uint8_t segments) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (segments == 0) return;
    float exact_filled = pct / 100.0f * segments;
    uint8_t full = static_cast<uint8_t>(exact_filled);
    float frac = exact_filled - full;
    constexpr int16_t kGap = 3;
    int16_t seg_w = static_cast<int16_t>(r.w / segments);
    for (uint8_t i = 0; i < segments; ++i) {
        int16_t x = static_cast<int16_t>(r.x + i * seg_w);
        // Все деления одной ширины. Раньше последнее добирало остаток от
        // деления — на глаз шкала выглядела кривой, а по её длине нельзя
        // посчитать заполнение: одно деление стоило больше остальных.
        int16_t w = static_cast<int16_t>(seg_w - kGap);
        if (w <= 0) continue;
        if (i < full) {
            c.fill_rect(x, r.y, w, r.h, Color::Black);
        } else if (i == full && frac > 0.05f) {
            c.rect(x, r.y, w, r.h, Color::Black);
            fill_hatched(c, x, r.y, w, r.h);
        } else {
            c.rect(x, r.y, w, r.h, Color::Black);
        }
    }
}

void draw_eyebrow(Canvas& c, Rect r, const char* label) {
    draw_text(c, fonts::Terminus16, r.x, static_cast<int16_t>(r.y + EYEBROW_TEXT_OFFSET), label,
              Color::Black, 1, /*bold=*/true);
    int16_t lw = text_width(fonts::Terminus16, label);
    int16_t line_x = static_cast<int16_t>(r.x + lw + 8);
    if (line_x < r.x + r.w) {
        c.hline(line_x, static_cast<int16_t>(r.y + EYEBROW_LINE_OFFSET),
                static_cast<int16_t>(r.x + r.w - line_x), Color::Black);
    }
}

// Инверсная плашка — белым по чёрному, как .inv в cockpit.html ("Почта").
// draw_text кладёт только «горящие» биты глифа — если под ними уже залитый
// чёрным прямоугольник, а цвет самого текста White, получаются белые буквы
// на чёрном без отдельной поддержки в движке шрифта.
int16_t draw_inverse_label(Canvas& c, int16_t x, int16_t baseline_y, const char* label) {
    int16_t text_w = text_width(fonts::Terminus16, label);
    constexpr int16_t kPadX = 7;
    constexpr int16_t kAbove = 14;
    constexpr int16_t kBelow = 4;
    int16_t plate_w = static_cast<int16_t>(text_w + 2 * kPadX);
    c.fill_rect(x, static_cast<int16_t>(baseline_y - kAbove), plate_w,
                static_cast<int16_t>(kAbove + kBelow), Color::Black);
    draw_text(c, fonts::Terminus16, static_cast<int16_t>(x + kPadX), baseline_y, label,
              Color::White, 1, /*bold=*/true);
    return plate_w;
}

void draw_wifi_bars(Canvas& c, int16_t x, int16_t y, int8_t bars) {
    constexpr int16_t kIconH = 16;
    for (int8_t i = 0; i < 4; ++i) {
        int16_t bar_h = static_cast<int16_t>(4 + i * 3);
        int16_t bx = static_cast<int16_t>(x + i * 7);
        int16_t by = static_cast<int16_t>(y + kIconH - bar_h);
        c.rect(bx, by, 5, bar_h, Color::Black);
        if (i < bars && bar_h > 2) {
            c.fill_rect(static_cast<int16_t>(bx + 1), static_cast<int16_t>(by + 1), 3,
                        static_cast<int16_t>(bar_h - 2), Color::Black);
        }
    }
}

void draw_battery_icon(Canvas& c, int16_t x, int16_t y, int8_t pct) {
    c.rect(x, y, 20, 12, Color::Black);
    c.fill_rect(static_cast<int16_t>(x + 20), static_cast<int16_t>(y + 3), 2, 6, Color::Black);
    int8_t clamped = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    int16_t fill_w = static_cast<int16_t>((20 - 4) * clamped / 100);
    if (fill_w > 0) {
        c.fill_rect(static_cast<int16_t>(x + 2), static_cast<int16_t>(y + 2), fill_w, 8,
                    Color::Black);
    }
}

// ── шапка ──

void draw_header(Canvas& c, const DeviceInfo& d) {
    Civil now_civil = to_civil(d.now, d.timezone_minutes);

    char date_buf[48];
    std::snprintf(date_buf, sizeof(date_buf), "%s · %d %s", WEEKDAYS[now_civil.weekday],
                  now_civil.day, MONTHS[now_civil.month - 1]);
    draw_text(c, fonts::Terminus24, MARGIN, 32, date_buf, Color::Black, 1, /*bold=*/true);

    char clock_buf[24];
    if (d.next_update_at > 0) {
        Civil next_civil = to_civil(d.next_update_at, d.timezone_minutes);
        std::snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d → %02d:%02d", now_civil.hour,
                      now_civil.minute, next_civil.hour, next_civil.minute);
    } else {
        std::snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", now_civil.hour, now_civil.minute);
    }

    int16_t x = static_cast<int16_t>(c.width() - MARGIN);
    int16_t clock_w = text_width(fonts::Terminus14, clock_buf);
    x = static_cast<int16_t>(x - clock_w);
    draw_text(c, fonts::Terminus14, x, 38, clock_buf, Color::Black, 1, /*bold=*/true);
    x = static_cast<int16_t>(x - 16);

    if (d.battery_pct >= 0) {
        char batt_buf[8];
        std::snprintf(batt_buf, sizeof(batt_buf), "%d%%", d.battery_pct);
        int16_t w = text_width(fonts::Terminus14, batt_buf);
        x = static_cast<int16_t>(x - w);
        draw_text(c, fonts::Terminus14, x, 38, batt_buf, Color::Black, 1, /*bold=*/true);
        x = static_cast<int16_t>(x - 4 - 22);
        draw_battery_icon(c, x, 24, d.battery_pct);
    } else {
        const char* usb = "USB";
        int16_t w = text_width(fonts::Terminus14, usb);
        x = static_cast<int16_t>(x - w);
        draw_text(c, fonts::Terminus14, x, 38, usb, Color::Black, 1, /*bold=*/true);
    }
    x = static_cast<int16_t>(x - 16 - 22);
    draw_wifi_bars(c, x, 20, wifi_bars(d.wifi_rssi));

    c.fill_rect(MARGIN, HEADER_RULE_Y, static_cast<int16_t>(c.width() - 2 * MARGIN), 2,
                Color::Black);
}

// ── рынки (левая колонка верхнего ряда, ширина фиксирована — RATES_WIDTH) ──
//
// Курс BTC несёт визуальную доминанту кадра, как в эталоне (.num, 41px) —
// рисуется IBM Plex Mono Bold в точном пиксельном кегле, без кратного
// увеличения Terminus (как было до появления второго шрифта): у векторного
// контура, в отличие от растра Terminus, есть кегль ровно 41, не только
// ближайший из фиксированной сетки.

void draw_rates(Canvas& c, const Store& store, const DeviceInfo& d, Rect r) {
    draw_eyebrow(c, r, "РЫНКИ");
    int16_t y = static_cast<int16_t>(r.y + EYEBROW_TEXT_OFFSET);  // базовая линия эйброу

    const Slot* btc = store.find("btc");
    const Slot* usd_rub = store.find("usd_rub");
    const Slot* eur_rub = store.find("eur_rub");

    if (has_data(btc)) {
        int16_t label_baseline = static_cast<int16_t>(y + 22);
        draw_text(c, fonts::Terminus14, r.x, label_baseline, "BTC / USD", Color::Black, 1, /*bold=*/true);

        String delta = format_delta(btc->delta);
        int16_t dw = text_width(fonts::Terminus14, delta.c_str());
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - dw), label_baseline,
                  delta.c_str(), Color::Black, 1, /*bold=*/true);
        const char* period = "ЗА 24 Ч";
        int16_t pw = text_width(fonts::Terminus14, period);
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - pw),
                  static_cast<int16_t>(label_baseline + 18), period, Color::Black, 1, /*bold=*/true);

        int16_t number_baseline = static_cast<int16_t>(label_baseline + 41);
        String value = format_value(btc, d.now, 0, "");
        draw_text(c, fonts::PlexMono41, r.x, number_baseline, value.c_str(), Color::Black);

        int16_t spark_top = static_cast<int16_t>(number_baseline + 10);
        constexpr int16_t kSparkH = 47;
        float spark[Slot::kHistoryCapacity + 1];
        uint8_t n = build_spark(*btc, spark, static_cast<uint8_t>(Slot::kHistoryCapacity + 1));
        draw_sparkline(c, Rect{r.x, spark_top, r.w, kSparkH}, spark, n, /*fill_below=*/true);

        int16_t hair_y = static_cast<int16_t>(spark_top + kSparkH + 10);
        c.hline(r.x, hair_y, r.w, Color::Black);
        y = static_cast<int16_t>(hair_y + 31 - 22);  // -22: компенсация общего шага ниже
    }
    y = static_cast<int16_t>(y + 22);  // общий шаг до первой строки курса

    struct Pair {
        const Slot* slot;
        const char* label;
    };
    const Pair pairs[2] = {{usd_rub, "USD / RUB"}, {eur_rub, "EUR / RUB"}};
    for (const Pair& p : pairs) {
        if (!has_data(p.slot)) continue;

        draw_text(c, fonts::Terminus14, r.x, y, p.label, Color::Black, 1, /*bold=*/true);

        String delta = format_delta(p.slot->delta);
        int16_t dw = text_width(fonts::Terminus14, delta.c_str());
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - dw), y, delta.c_str(),
                  Color::Black, 1, /*bold=*/true);

        String val = format_value(p.slot, d.now, 2, "");
        int16_t vw = text_width(fonts::PlexMono25, val.c_str());
        draw_text(c, fonts::PlexMono25, static_cast<int16_t>(r.x + r.w - dw - vw - 10), y,
                  val.c_str(), Color::Black);
        y = static_cast<int16_t>(y + 36);
    }
}

// ── лимиты + воздух (правая колонка верхнего ряда, гибкая ширина) ──
//
// Одна колонка с двумя рубриками, как в cockpit.html, — не два отдельных
// места сетки: так дословно повторяется эталон. "ЛИМИТЫ" и "КАБИНЕТ · ВОЗДУХ"
// делят между собой одну и ту же позицию верхнего эйброу колонки, если один
// из них отсутствует, — совсем как скрытый `{% if %}`-блок не оставляет
// пустоты в потоке документа.

void draw_limit_row(Canvas& c, Rect area, const char* window_label, const Slot* s, uint32_t now,
                    uint8_t segments) {
    draw_text(c, fonts::Terminus14, area.x, static_cast<int16_t>(area.y + 10), window_label,
              Color::Black, 1, /*bold=*/true);
    String pct = format_percent(s, now);
    // 16px — .num парного окна (Claude 5ч/неделя) в cockpit.html.
    int16_t pw = text_width(fonts::PlexMono16, pct.c_str());
    int16_t label_w = text_width(fonts::Terminus14, window_label);
    int16_t bar_x = static_cast<int16_t>(area.x + label_w + 8);
    int16_t bar_w = static_cast<int16_t>(area.w - label_w - 8 - pw - 8);
    if (bar_w > 0) {
        draw_segbar(c, Rect{bar_x, area.y, bar_w, 14}, has_data(s) ? s->number : 0, segments);
    }
    draw_text(c, fonts::PlexMono16, static_cast<int16_t>(area.x + area.w - pw),
              static_cast<int16_t>(area.y + 12), pct.c_str(), Color::Black);
}

bool co2_alarm(float v) { return v >= 1200.0f; }
bool co2_quiet(float v) { return v < 700.0f; }
bool tvoc_alarm(float v) { return v >= 660.0f; }
bool tvoc_quiet(float v) { return v < 220.0f; }

// "▲92/ч" — изменение в час, не в процентах (тот же Slot::delta, что и у
// котировок, но своя единица — коннектор воздуха кладёт туда именно её).
String format_delta_per_hour(float delta_per_hour) {
    char buf[24];
    const char* arrow = delta_per_hour >= 0 ? "▲" : "▼";
    float magnitude = delta_per_hour >= 0 ? delta_per_hour : -delta_per_hour;
    std::snprintf(buf, sizeof(buf), "%s%.0f/ч", arrow, static_cast<double>(magnitude));
    return String(buf);
}

// Индикатор нормы — тремя разными начертаниями, как .tag/.tag.quiet/.tag.alarm
// в cockpit.html: тревога — инверсная плашка (бросается в глаза сильнее
// всего), норма — рамка вокруг подписи, свежо/тихо — голый текст без рамки
// (спокойное состояние не нуждается в акценте).
void draw_state_tag(Canvas& c, int16_t right_x, int16_t baseline_y, const char* label,
                    bool alarm, bool quiet) {
    int16_t tw = text_width(fonts::Terminus14, label);
    if (alarm) {
        constexpr int16_t kPadX = 6;
        int16_t plate_w = static_cast<int16_t>(tw + 2 * kPadX);
        int16_t x = static_cast<int16_t>(right_x - plate_w);
        c.fill_rect(x, static_cast<int16_t>(baseline_y - 12), plate_w, 16, Color::Black);
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(x + kPadX), baseline_y, label,
                  Color::White, 1, /*bold=*/true);
    } else if (quiet) {
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(right_x - tw), baseline_y, label,
                  Color::Black, 1, /*bold=*/true);
    } else {
        constexpr int16_t kPadX = 6;
        int16_t plate_w = static_cast<int16_t>(tw + 2 * kPadX);
        int16_t x = static_cast<int16_t>(right_x - plate_w);
        c.rect(x, static_cast<int16_t>(baseline_y - 12), plate_w, 16, Color::Black);
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(x + kPadX), baseline_y, label,
                  Color::Black, 1, /*bold=*/true);
    }
}

void draw_air_metric(Canvas& c, Rect area, const char* label, const char* unit, const Slot* s,
                     uint32_t now, bool (*is_alarm)(float), bool (*is_quiet)(float)) {
    int16_t baseline = static_cast<int16_t>(area.y + 14);
    draw_text(c, fonts::Terminus14, area.x, baseline, label, Color::Black, 1, /*bold=*/true);
    String val = format_value(s, now, 0, "");
    int16_t label_w = text_width(fonts::Terminus14, label);
    int16_t value_x = static_cast<int16_t>(area.x + label_w + 8);
    draw_text(c, fonts::PlexMono28, value_x, static_cast<int16_t>(baseline + 4), val.c_str(),
              Color::Black);
    int16_t vw = text_width(fonts::PlexMono28, val.c_str());
    draw_text(c, fonts::Terminus14, static_cast<int16_t>(value_x + vw + 6),
              static_cast<int16_t>(baseline + 4), unit, Color::Black, 1, /*bold=*/true);

    if (has_data(s) && s->delta != 0.0f) {
        String delta = format_delta_per_hour(s->delta);
        int16_t dw = text_width(fonts::Terminus14, delta.c_str());
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(area.x + area.w - dw), baseline,
                  delta.c_str(), Color::Black, 1, /*bold=*/true);
    }

    if (!has_data(s)) return;

    // 11/14/18 (было 13/20/12) — обмер эталона после перехода значения на
    // PlexMono28 (docs/architecture.md, "Шрифты"): у эталона от базовой линии
    // значения до низа блока (график + тег) 53px (reference/cockpit-reference.png,
    // зона «воздух»), у нас с прежними отступами набегало 65 — блок упирался в
    // нижнюю границу ряда. Высота искры (18, не 20) — тоже обмер, ближе к
    // .num-графику эталона (svg height=18 в cockpit.html), не круглое число.
    int16_t spark_top = static_cast<int16_t>(baseline + 11);
    constexpr int16_t kSparkH = 18;
    float spark[Slot::kHistoryCapacity + 1];
    uint8_t n = build_spark(*s, spark, static_cast<uint8_t>(Slot::kHistoryCapacity + 1));
    draw_sparkline(c, Rect{area.x, spark_top, area.w, kSparkH}, spark, n);

    bool alarm = is_alarm(s->number);
    bool quiet = !alarm && is_quiet(s->number);
    const char* state = alarm ? "ПРОВЕТРИТЬ" : (quiet ? "СВЕЖО" : "НОРМА");
    draw_state_tag(c, static_cast<int16_t>(area.x + area.w),
                   static_cast<int16_t>(spark_top + kSparkH + 6), state, alarm, quiet);
}

void draw_limits_and_air(Canvas& c, const Store& store, const DeviceInfo& d, Rect r) {
    const Slot* claude_5h = store.find("limit.claude.5h");
    const Slot* claude_week = store.find("limit.claude.week");
    const Slot* codex = store.find("limit.codex");
    bool has_claude = has_data(claude_5h) || has_data(claude_week);
    bool has_codex = has_data(codex);
    bool has_limits = has_claude || has_codex;

    const Slot* co2 = store.find("co2");
    const Slot* tvoc = store.find("tvoc");
    bool has_co2 = has_data(co2);
    bool has_tvoc = has_data(tvoc);
    bool has_air = has_co2 || has_tvoc;

    // Эйброу текущей рубрики садится туда, куда довёл курсор y — если ЛИМИТЫ
    // отсутствуют вовсе, КАБИНЕТ·ВОЗДУХ окажется в самом верху колонки, ровно
    // как в потоке документа cockpit.html.
    int16_t y = r.y;

    if (has_limits) {
        draw_eyebrow(c, Rect{r.x, y, r.w, 0}, "ЛИМИТЫ");
        int16_t cursor = static_cast<int16_t>(y + EYEBROW_TEXT_OFFSET);

        if (has_claude) {
            int16_t title_baseline = static_cast<int16_t>(cursor + 26);
            draw_text(c, fonts::Terminus16, r.x, title_baseline, "Claude", Color::Black, 1, /*bold=*/true);
            const Slot* reset = store.find("limit.claude.reset");
            if (has_data(reset)) {
                int16_t rw = text_width(fonts::Terminus14, reset->text.c_str());
                draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - rw),
                          title_baseline, reset->text.c_str(), Color::Black, 1, /*bold=*/true);
            }
            int16_t bar_y = static_cast<int16_t>(title_baseline + 13);
            int16_t half = static_cast<int16_t>((r.w - 20) / 2);
            draw_limit_row(c, Rect{r.x, bar_y, half, 14}, "5 Ч", claude_5h, d.now, 10);
            c.vline(static_cast<int16_t>(r.x + half + 10), bar_y, 14, Color::Black);
            draw_limit_row(c, Rect{static_cast<int16_t>(r.x + half + 20), bar_y, half, 14},
                           "НЕДЕЛЯ", claude_week, d.now, 10);
            cursor = static_cast<int16_t>(bar_y + 14);
        }

        if (has_codex) {
            int16_t title_baseline =
                static_cast<int16_t>(cursor + (has_claude ? 27 : 26));
            draw_text(c, fonts::Terminus16, r.x, title_baseline, "GPT", Color::Black, 1, /*bold=*/true);
            const Slot* reset = store.find("limit.codex.reset");
            if (has_data(reset)) {
                int16_t rw = text_width(fonts::Terminus14, reset->text.c_str());
                draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - rw),
                          title_baseline, reset->text.c_str(), Color::Black, 1, /*bold=*/true);
            }
            int16_t bar_y = static_cast<int16_t>(title_baseline + 16);
            String pct = format_percent(codex, d.now);
            // 20px — .num одиночного окна (GPT) в cockpit.html, крупнее
            // парного (16px у Claude выше): в эталоне у одной строки больше
            // свободного места по высоте, чем у половины разделённого блока.
            int16_t pw = text_width(fonts::PlexMono20, pct.c_str());
            int16_t bar_w = static_cast<int16_t>(r.w - pw - 8);
            if (bar_w > 0) {
                draw_segbar(c, Rect{r.x, bar_y, bar_w, 14}, codex->number,
                            segments_for_width(bar_w));
            }
            draw_text(c, fonts::PlexMono20, static_cast<int16_t>(r.x + r.w - pw),
                      static_cast<int16_t>(bar_y + 12), pct.c_str(), Color::Black);
            cursor = static_cast<int16_t>(bar_y + 14);
        }

        if (has_air) {
            int16_t hair_y = static_cast<int16_t>(cursor + 15);
            c.hline(r.x, hair_y, r.w, Color::Black);
            y = static_cast<int16_t>(hair_y + 10);  // -> следующий эйброу
        } else {
            y = cursor;  // воздуха нет — колонка заканчивается на лимитах
        }
    }

    if (has_air) {
        draw_eyebrow(c, Rect{r.x, y, r.w, 0}, "КАБИНЕТ · ВОЗДУХ");
        // 26, не 20: у PlexMono28 (значение CO₂/TVOC) выносные части поднимают
        // верхний край чернил заметно выше базовой линии, чем у прежнего
        // Terminus24 — с прежним отступом строка значения почти касалась
        // эйброу сверху (эталон даёт видимый зазор ~10px между ними, обмер
        // reference/cockpit-reference.png).
        int16_t area_y = static_cast<int16_t>(y + 26);
        int16_t area_h = static_cast<int16_t>(r.y + r.h - area_y);

        if (has_co2 && has_tvoc) {
            int16_t half = static_cast<int16_t>((r.w - 20) / 2);
            draw_air_metric(c, Rect{r.x, area_y, half, area_h}, "CO₂", "ppm", co2, d.now,
                            co2_alarm, co2_quiet);
            c.vline(static_cast<int16_t>(r.x + half + 10), area_y, 53, Color::Black);
            draw_air_metric(c, Rect{static_cast<int16_t>(r.x + half + 20), area_y, half, area_h},
                            "TVOC", "ppb", tvoc, d.now, tvoc_alarm, tvoc_quiet);
        } else if (has_co2) {
            draw_air_metric(c, Rect{r.x, area_y, r.w, area_h}, "CO₂", "ppm", co2, d.now, co2_alarm,
                            co2_quiet);
        } else if (has_tvoc) {
            draw_air_metric(c, Rect{r.x, area_y, r.w, area_h}, "TVOC", "ppb", tvoc, d.now,
                            tvoc_alarm, tvoc_quiet);
        }
    }
}

// ── почта (гибкая ширина, левая часть нижнего ряда) ──

void draw_mail(Canvas& c, const Store& store, const DeviceInfo&, Rect r) {
    int16_t header_baseline = static_cast<int16_t>(r.y + 12);
    int16_t plate_w = draw_inverse_label(c, r.x, header_baseline, "ПОЧТА");

    const Slot* unread = store.find("mail.unread");
    char summary[32];
    int count = has_data(unread) ? static_cast<int>(unread->number) : 0;
    std::snprintf(summary, sizeof(summary), "%d НЕПРОЧИТАННЫХ", count);
    draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + plate_w + 10), header_baseline,
              summary, Color::Black, 1, /*bold=*/true);

    constexpr int16_t kFirstRowBaseline = 45;  // от r.y — компактно, как в эталоне
    constexpr int16_t kRowHeight = 34;
    constexpr int16_t kSeparatorGap = 12;  // baseline -> разделитель под строкой
    constexpr int16_t kSubjectX = 150;
    constexpr int16_t kTimeReserve = 60;

    int16_t y = static_cast<int16_t>(r.y + kFirstRowBaseline);
    for (int i = 1; i <= 4; ++i) {
        char from_id[24], subj_id[24], time_id[24];
        std::snprintf(from_id, sizeof(from_id), "mail.%d.from", i);
        std::snprintf(subj_id, sizeof(subj_id), "mail.%d.subject", i);
        std::snprintf(time_id, sizeof(time_id), "mail.%d.time", i);
        const Slot* from = store.find(String(from_id));
        const Slot* subject = store.find(String(subj_id));
        if (!has_data(from) && !has_data(subject)) continue;
        if (y > r.y + r.h) break;

        // Точка-маркер перед отправителем — как .dot в cockpit.html; здесь
        // без чтения/непрочитанного состояния на слот (в Store такого пока
        // нет), просто отметка «это письмо из стопки».
        constexpr int16_t kDotSize = 5;
        c.fill_rect(r.x, static_cast<int16_t>(y - kDotSize), kDotSize, kDotSize, Color::Black);
        int16_t text_x = static_cast<int16_t>(r.x + kDotSize + 8);

        if (has_data(from)) {
            String label = truncate_to_width(fonts::Terminus14, from->text.c_str(),
                                             static_cast<int16_t>(kSubjectX - kDotSize - 16));
            draw_text(c, fonts::Terminus14, text_x, y, label.c_str(), Color::Black, 1, /*bold=*/true);
        }
        if (has_data(subject)) {
            int16_t subject_w = static_cast<int16_t>(r.w - kSubjectX - kTimeReserve);
            String label =
                truncate_to_width(fonts::Terminus14, subject->text.c_str(), subject_w);
            draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + kSubjectX), y,
                      label.c_str(), Color::Black, 1, /*bold=*/true);
        }
        const Slot* at = store.find(String(time_id));
        if (has_data(at)) {
            int16_t tw = text_width(fonts::Terminus14, at->text.c_str());
            draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - tw), y,
                      at->text.c_str(), Color::Black, 1, /*bold=*/true);
        }
        c.hline(r.x, static_cast<int16_t>(y + kSeparatorGap), r.w, Color::Black);
        y = static_cast<int16_t>(y + kRowHeight);
    }
}

// ── сегодня (фиксированная ширина, правая часть нижнего ряда) ──

void draw_today(Canvas& c, const Store& store, const DeviceInfo&, Rect r) {
    draw_eyebrow(c, r, "СЕГОДНЯ");
    int16_t y = static_cast<int16_t>(r.y + EYEBROW_TEXT_OFFSET + 34);

    const Slot* temp = store.find("weather.temp");
    if (has_data(temp)) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%+d°", static_cast<int>(temp->number));
        draw_text(c, fonts::PlexMono28, r.x, y, buf, Color::Black);
        int16_t tw = text_width(fonts::PlexMono28, buf);
        const Slot* summary = store.find("weather.summary");
        if (has_data(summary)) {
            int16_t summary_w = static_cast<int16_t>(r.w - tw - 10);
            String label = truncate_to_width(fonts::Terminus14, summary->text.c_str(), summary_w);
            draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + tw + 10), y, label.c_str(),
                      Color::Black, 1, /*bold=*/true);
        }
        y = static_cast<int16_t>(y + 26);
    }

    for (int i = 1; i <= 3; ++i) {
        char at_id[24], title_id[24];
        std::snprintf(at_id, sizeof(at_id), "event.%d.at", i);
        std::snprintf(title_id, sizeof(title_id), "event.%d.title", i);
        const Slot* at = store.find(String(at_id));
        const Slot* title = store.find(String(title_id));
        if (!has_data(title)) continue;
        if (y > r.y + r.h) break;

        if (has_data(at)) {
            // 14px — .num времени события в cockpit.html (`ev.at_label`).
            draw_text(c, fonts::PlexMono14, r.x, y, at->text.c_str(), Color::Black);
        }
        String label = truncate_to_width(fonts::Terminus14, title->text.c_str(),
                                         static_cast<int16_t>(r.w - 56));
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + 56), y, label.c_str(),
                  Color::Black, 1, /*bold=*/true);
        y = static_cast<int16_t>(y + 20);
    }
}

}  // namespace

String format_decimal(float value, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(value));
    for (char* p = buf; *p; ++p) {
        if (*p == '.') *p = ',';
    }

    // Разряды целой части разделяем пробелом: «80 689» вместо «80689». На
    // пятизначном курсе без разделителя читатель считает нули глазами, а
    // эталон эту группировку делает. Работаем с буфером, а не со String:
    // хостовый шим не умеет индексацию, а вести две ветки ради форматирования
    // числа — лишнее.
    int int_end = 0;
    while (buf[int_end] != '\0' && buf[int_end] != ',') ++int_end;
    const int digits_start = (buf[0] == '-' || buf[0] == '+') ? 1 : 0;
    if (int_end - digits_start <= 4) return String(buf);  // до четырёх цифр группировка мешает

    char out[40];
    int w = 0;
    for (int i = 0; i < int_end && w < static_cast<int>(sizeof(out)) - 2; ++i) {
        if (i > digits_start && (int_end - i) % 3 == 0) out[w++] = ' ';
        out[w++] = buf[i];
    }
    for (int i = int_end; buf[i] != '\0' && w < static_cast<int>(sizeof(out)) - 1; ++i) {
        out[w++] = buf[i];
    }
    out[w] = '\0';
    return String(out);
}


int8_t wifi_bars(int32_t rssi_dbm) {
    if (rssi_dbm == 0) return 0;  // сентинел DeviceInfo::wifi_rssi — «не в сети»
    if (rssi_dbm >= -55) return 4;
    if (rssi_dbm >= -65) return 3;
    if (rssi_dbm >= -75) return 2;
    return 1;
}

uint8_t segments_for_width(int16_t w) {
    constexpr int16_t kMinSegWidth = 10;  // с зазором в 3px, см. draw_segbar
    int16_t fit = static_cast<int16_t>(w / kMinSegWidth);
    if (fit < 4) return 4;
    if (fit > 20) return 20;
    return static_cast<uint8_t>(fit);
}

Civil to_civil(uint32_t unix_time, int16_t timezone_minutes) {
    int64_t local = static_cast<int64_t>(unix_time) + static_cast<int64_t>(timezone_minutes) * 60;
    int64_t days = floor_div(local, 86400);
    int64_t secs_of_day = floor_mod(local, 86400);

    int year;
    unsigned month, day;
    civil_from_days(days, &year, &month, &day);

    Civil c;
    c.day = static_cast<int>(day);
    c.month = static_cast<int>(month);
    // 1970-01-01 (days == 0) — четверг, индекс 3 в WEEKDAYS (Пн=0).
    c.weekday = static_cast<int>(floor_mod(days + 3, 7));
    c.hour = static_cast<int>(secs_of_day / 3600);
    c.minute = static_cast<int>((secs_of_day % 3600) / 60);
    return c;
}

bool has_data(const Slot* s) { return s != nullptr && !s->empty(); }

String format_percent(const Slot* s, uint32_t now) {
    if (!has_data(s)) return String("—");
    int pct = static_cast<int>(s->number + (s->number >= 0 ? 0.5f : -0.5f));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char buf[16];
    if (s->stale(now)) {
        std::snprintf(buf, sizeof(buf), "≈%d%%", pct);
    } else {
        std::snprintf(buf, sizeof(buf), "%d%%", pct);
    }
    return String(buf);
}

String format_value(const Slot* s, uint32_t now, int decimals, const char* suffix) {
    if (!has_data(s)) return String("—");
    String num = format_decimal(s->number, decimals);
    char buf[48];
    if (s->stale(now)) {
        std::snprintf(buf, sizeof(buf), "≈%s%s", num.c_str(), suffix);
    } else {
        std::snprintf(buf, sizeof(buf), "%s%s", num.c_str(), suffix);
    }
    return String(buf);
}

bool rates_visible(const Store& store) {
    return has_data(store.find("btc")) || has_data(store.find("usd_rub")) ||
           has_data(store.find("eur_rub"));
}

bool limits_visible(const Store& store) {
    return has_data(store.find("limit.claude.5h")) || has_data(store.find("limit.claude.week")) ||
           has_data(store.find("limit.codex"));
}

bool air_visible(const Store& store) {
    return has_data(store.find("co2")) || has_data(store.find("tvoc"));
}

bool mail_visible(const Store& store) { return has_data(store.find("mail.unread")); }

bool today_visible(const Store& store) {
    return has_data(store.find("weather.temp")) || has_data(store.find("event.1.title"));
}

void compute_columns(Rect row, int16_t gap, const bool* visible, uint8_t total, Rect* out) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < total; ++i) {
        if (visible[i]) ++count;
    }
    if (count == 0) {
        for (uint8_t i = 0; i < total; ++i) out[i] = Rect{};
        return;
    }
    int16_t total_gap = static_cast<int16_t>(gap * (count - 1));
    int16_t col_w = static_cast<int16_t>((row.w - total_gap) / count);
    int16_t cursor = row.x;
    for (uint8_t i = 0; i < total; ++i) {
        if (!visible[i]) {
            out[i] = Rect{};
            continue;
        }
        out[i] = Rect{cursor, row.y, col_w, row.h};
        cursor = static_cast<int16_t>(cursor + col_w + gap);
    }
}

void compute_columns_fixed_first(Rect row, int16_t gap, bool first_visible, bool second_visible,
                                  int16_t first_width, Rect* first, Rect* second) {
    if (first_visible && second_visible) {
        *first = Rect{row.x, row.y, first_width, row.h};
        int16_t second_x = static_cast<int16_t>(row.x + first_width + gap);
        *second = Rect{second_x, row.y, static_cast<int16_t>(row.x + row.w - second_x), row.h};
    } else if (first_visible) {
        *first = row;
        *second = Rect{};
    } else if (second_visible) {
        *second = row;
        *first = Rect{};
    } else {
        *first = Rect{};
        *second = Rect{};
    }
}

void compute_columns_fixed_second(Rect row, int16_t gap, bool first_visible, bool second_visible,
                                   int16_t second_width, Rect* first, Rect* second) {
    if (first_visible && second_visible) {
        int16_t second_x = static_cast<int16_t>(row.x + row.w - second_width);
        *second = Rect{second_x, row.y, second_width, row.h};
        *first = Rect{row.x, row.y, static_cast<int16_t>(second_x - row.x - gap), row.h};
    } else if (first_visible) {
        *first = row;
        *second = Rect{};
    } else if (second_visible) {
        *second = row;
        *first = Rect{};
    } else {
        *first = Rect{};
        *second = Rect{};
    }
}

void compute_two_rows(Rect body, int16_t gap, bool row1_has, bool row2_has, Rect* row1,
                      Rect* row2) {
    if (row1_has && row2_has) {
        // Соотношение — не произвольное 50/50, а из обмера эталона: верхний
        // ряд (Рынки/Лимиты/Воздух) занимает заметно больше высоты нижнего
        // (Почта/Сегодня) — 235px против 154px на теле кадра 390px без зазора.
        int16_t h1 = static_cast<int16_t>((body.h - gap) * 0.603f);
        int16_t h2 = static_cast<int16_t>(body.h - gap - h1);
        *row1 = Rect{body.x, body.y, body.w, h1};
        *row2 = Rect{body.x, static_cast<int16_t>(body.y + h1 + gap), body.w, h2};
    } else if (row1_has) {
        *row1 = body;
        *row2 = Rect{};
    } else if (row2_has) {
        *row1 = Rect{};
        *row2 = body;
    } else {
        *row1 = Rect{};
        *row2 = Rect{};
    }
}

void draw_frame(Canvas& canvas, const Store& store, const DeviceInfo& device) {
    canvas.fill(Color::White);
    draw_header(canvas, device);

    Rect body{MARGIN, BODY_TOP, static_cast<int16_t>(canvas.width() - 2 * MARGIN),
              static_cast<int16_t>(canvas.height() - BODY_TOP - MARGIN)};

    bool rates_ok = rates_visible(store);
    bool combined_ok = limits_visible(store) || air_visible(store);
    bool mail_ok = mail_visible(store);
    bool today_ok = today_visible(store);
    bool row1_has = rates_ok || combined_ok;
    bool row2_has = mail_ok || today_ok;

    Rect row1, row2;
    compute_two_rows(body, ROW_GAP, row1_has, row2_has, &row1, &row2);

    if (row1_has && row2_has) {
        // Разделитель садится на границу рядов, а не в середину зазора — в
        // эталоне зазор целиком идёт ПОСЛЕ линии (298→310 у div, а не по 6px
        // с обеих сторон).
        const int16_t rule_w = static_cast<int16_t>(canvas.width() - 2 * MARGIN);
        canvas.hline(MARGIN, static_cast<int16_t>(row1.y + row1.h), rule_w, Color::Black);
        canvas.hline(MARGIN, static_cast<int16_t>(row1.y + row1.h + 1), rule_w, Color::Black);
    }

    if (row1_has) {
        Rect rates_col, combined_col;
        compute_columns_fixed_first(row1, TOP_GAP, rates_ok, combined_ok, RATES_WIDTH, &rates_col,
                                     &combined_col);
        if (rates_ok) draw_rates(canvas, store, device, rates_col);
        if (combined_ok) draw_limits_and_air(canvas, store, device, combined_col);
        // Разделителя между Рынками и правой колонкой в эталоне нет (только
        // зазор) — в отличие от CO₂|TVOC и Почта|Сегодня, где вертикальная
        // линия есть. compare_frame.py это подтверждает: единственные найденные
        // вертикальные линии — x=557 (внутри Воздуха) и x=567 (Почта|Сегодня).
    }
    if (row2_has) {
        Rect mail_col, today_col;
        compute_columns_fixed_second(row2, BOTTOM_GAP, mail_ok, today_ok, TODAY_WIDTH, &mail_col,
                                      &today_col);
        if (mail_ok) draw_mail(canvas, store, device, mail_col);
        if (today_ok) draw_today(canvas, store, device, today_col);
        if (mail_ok && today_ok) {
            canvas.vline(static_cast<int16_t>(today_col.x - BOTTOM_GAP + 2), row2.y, row2.h,
                         Color::Black);
            canvas.vline(static_cast<int16_t>(today_col.x - BOTTOM_GAP + 1), row2.y, row2.h,
                         Color::Black);
        }
    }
}

namespace {

// ── экран учётных данных точки доступа (docs/decisions.md, п.8) ──
//
// Раскладка: слева текстом сеть и пароль (камера может не сработать — тогда
// вводят руками), справа — QR той же информации в формате WIFI:. Разнесены
// зазором AP_QR_GAP, а не поставлены впритык, — иначе QR читается как
// приклеенный сбоку довесок, а не равноправная часть кадра.
constexpr int16_t AP_TITLE_Y = 34;          // базовая линия заголовка, как у даты в шапке (draw_header)
constexpr int16_t AP_BODY_TOP = 82;         // под линией под заголовком
constexpr uint8_t AP_QR_MODULE_PX = 6;      // ≥4-5px на модуль — читается с руки на 800x480
constexpr uint8_t AP_QR_QUIET_MODULES = 4;  // обязательная светлая рамка по спецификации QR
constexpr int16_t AP_QR_GAP = 30;

}  // namespace

void draw_ap_credentials(Canvas& canvas, const String& ssid, const String& password) {
    canvas.fill(Color::White);

    draw_text(canvas, fonts::Terminus24, MARGIN, AP_TITLE_Y, "НАСТРОЙКА INKROAM", Color::Black, 1,
              /*bold=*/true);
    canvas.fill_rect(MARGIN, HEADER_RULE_Y, static_cast<int16_t>(canvas.width() - 2 * MARGIN), 2,
                     Color::Black);

    int16_t body_bottom = static_cast<int16_t>(canvas.height() - MARGIN);
    int16_t body_h = static_cast<int16_t>(body_bottom - AP_BODY_TOP);

    int16_t qr_side = wifi_qr::side_for(AP_QR_MODULE_PX, AP_QR_QUIET_MODULES);
    int16_t qr_x = static_cast<int16_t>(canvas.width() - MARGIN - qr_side);
    int16_t qr_y = static_cast<int16_t>(AP_BODY_TOP + (body_h - qr_side) / 2);

    String payload = wifi_qr::payload(ssid, password);
    // draw() возвращает 0, если payload не поместился в фиксированную версию
    // QR (wifi_qr.h) — молча не рисуем код, а не роняем весь кадр: имя сети и
    // пароль текстом ниже остаются рабочим способом подключиться руками.
    wifi_qr::draw(canvas, qr_x, qr_y, payload, AP_QR_MODULE_PX, AP_QR_QUIET_MODULES);

    // Текстовая колонка слева от QR — та же ширина, что осталась после него
    // и зазора, а не фиксированное число: так верстка не разъедется, если
    // геометрию QR однажды придётся поменять.
    int16_t text_w = static_cast<int16_t>(qr_x - AP_QR_GAP - MARGIN);

    int16_t y = static_cast<int16_t>(AP_BODY_TOP + (body_h - 260) / 2);  // 260 — высота блока текста ниже

    // Terminus, не PlexMono: у PlexMono в наборе только цифры и пунктуация
    // (используется для курсов и процентов) — букв SSID/пароля в нём просто
    // нет, глиф молча не рисуется (font.h: find_glyph возвращает nullptr, а
    // draw_text пропускает символ). Terminus покрывает весь ASCII.
    draw_eyebrow(canvas, Rect{MARGIN, y, text_w, 0}, "СЕТЬ");
    y = static_cast<int16_t>(y + EYEBROW_TEXT_OFFSET + 34);
    draw_text(canvas, fonts::Terminus24, MARGIN, y,
              truncate_to_width(fonts::Terminus24, ssid.c_str(), text_w).c_str(), Color::Black, 1,
              /*bold=*/true);

    y = static_cast<int16_t>(y + 50);
    draw_eyebrow(canvas, Rect{MARGIN, y, text_w, 0}, "ПАРОЛЬ");
    y = static_cast<int16_t>(y + EYEBROW_TEXT_OFFSET + 56);
    // Terminus24 при scale=2 — тот же приём, что курс BTC в блоке Рынков
    // (см. font.h про kern 48 без пятого файла шрифта): пароль должен быть
    // зрительно доминирующим, чтобы его можно было перепечатать со стола без
    // наклона к панели. Алфавит пароля (config.cpp) фиксирован по длине (10
    // символов) — можно рисовать без truncate_to_width, переполнения не
    // бывает даже с запасом по ширине колонки.
    draw_text(canvas, fonts::Terminus24, MARGIN, y, password.c_str(), Color::Black, 2,
              /*bold=*/true);

    y = static_cast<int16_t>(y + 60);
    draw_text(canvas, fonts::Terminus14, MARGIN, y,
              "наведите камеру телефона на QR — сеть добавится сама,", Color::Black, 1, false);
    y = static_cast<int16_t>(y + 22);
    draw_text(canvas, fonts::Terminus14, MARGIN, y,
              "не считалось — введите сеть и пароль вручную.", Color::Black, 1, false);
}

}  // namespace layout

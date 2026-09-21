// Перенос анонимного namespace layout.cpp — пиксели не меняются, это
// перемещение функций, не переписывание (см. .claude/plans/constructor.md).
#include "prims.h"

#include <cstdio>
#include <string>

#include "../assets/terminus_14.h"
#include "../assets/terminus_16.h"
#include "../assets/plexmono_16.h"
#include "../assets/plexmono_20.h"
#include "../assets/plexmono_28.h"

namespace widgets::prims {

using canvas::Canvas;
using canvas::Color;
using fonts::draw_text;
using fonts::text_width;
using layout::Rect;
using slots::Slot;
using slots::Store;

void draw_eyebrow(Canvas& c, Rect r, const char* label) {
    draw_text(c, fonts::Terminus16, r.x, static_cast<int16_t>(r.y + kEyebrowTextOffset), label,
              Color::Black, 1, /*bold=*/true);
    int16_t lw = text_width(fonts::Terminus16, label);
    int16_t line_x = static_cast<int16_t>(r.x + lw + 8);
    if (line_x < r.x + r.w) {
        c.hline(line_x, static_cast<int16_t>(r.y + kEyebrowLineOffset),
                static_cast<int16_t>(r.x + r.w - line_x), Color::Black);
    }
}

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

String format_delta_per_hour(float delta_per_hour) {
    char buf[24];
    const char* arrow = delta_per_hour >= 0 ? "▲" : "▼";
    float magnitude = delta_per_hour >= 0 ? delta_per_hour : -delta_per_hour;
    std::snprintf(buf, sizeof(buf), "%s%.0f/ч", arrow, static_cast<double>(magnitude));
    return String(buf);
}

// Режет по кодовым точкам, а не по байтам, — иначе можно обрубить середину
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

namespace {

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

}  // namespace

// Спарклайну нужен ряд точек, заканчивающийся текущим значением: history —
// то, что накопил Store::put() до этого замера, number — самый свежий.
uint8_t build_spark(const Slot& s, float* out, uint8_t max_len) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < s.history_len && n < max_len; ++i) out[n++] = s.history[i];
    if (n < max_len) out[n++] = s.number;
    return n;
}

void draw_sparkline(Canvas& c, Rect r, const float* raw_values, uint8_t count, bool fill_below) {
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
    // Минимальный диапазон — 1% от величины. Иначе две первые точки после
    // включения рисуются клином на всю высоту блока: их разница и есть весь
    // диапазон, и колебание курса в десятые процента читается как обвал.
    const float mid = (hi + lo) * 0.5f;
    const float floor_range = (mid < 0 ? -mid : mid) * 0.01f;
    if (range < floor_range) {
        lo = mid - floor_range * 0.5f;
        hi = mid + floor_range * 0.5f;
        range = hi - lo;
    }
    if (range < 1e-6f) range = 1.0f;

    int16_t prev_x = 0, prev_y = 0;
    for (uint8_t i = 0; i < count; ++i) {
        int16_t x = static_cast<int16_t>(r.x + (static_cast<float>(i) / (count - 1)) * (r.w - 1));
        int16_t y = static_cast<int16_t>(r.y + r.h - 1 -
                                          ((values[i] - lo) / range) * (r.h - 1));
        if (i > 0) {
            c.line(prev_x, prev_y, x, y, Color::Black);
            c.line(prev_x, static_cast<int16_t>(prev_y + 1), x, static_cast<int16_t>(y + 1),
                   Color::Black);
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

// Индикатор нормы — тремя разными начертаниями, как .tag/.tag.quiet/.tag.alarm
// в cockpit.html: тревога — инверсная плашка (бросается в глаза сильнее
// всего), норма — рамка вокруг подписи, свежо/тихо — голый текст без рамки
// (спокойное состояние не нуждается в акценте).
void draw_state_tag(Canvas& c, int16_t right_x, int16_t baseline_y, const char* label, bool alarm,
                    bool quiet) {
    int16_t tw = text_width(fonts::Terminus14, label);
    if (alarm) {
        constexpr int16_t kPadX = 6;
        int16_t plate_w = static_cast<int16_t>(tw + 2 * kPadX);
        int16_t x = static_cast<int16_t>(right_x - plate_w);
        c.fill_rect(x, static_cast<int16_t>(baseline_y - 12), plate_w, 16, Color::Black);
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(x + kPadX), baseline_y, label,
                  Color::White, 1, /*bold=*/true);
    } else {
        // Единый стиль для «СВЕЖО» и «НОРМА» — рамка. В эталоне спокойное
        // состояние шло без рамки и полупрозрачным, но на однобитной панели
        // прозрачности нет, и бейдж без обводки читался как выпавший из ряда
        // (замечание с живого экрана). Различие несёт только тревога: она
        // инверсная. `quiet` остаётся в сигнатуре — им решают, что писать.
        (void) quiet;
        constexpr int16_t kPadX = 6;
        int16_t plate_w = static_cast<int16_t>(tw + 2 * kPadX);
        int16_t x = static_cast<int16_t>(right_x - plate_w);
        c.rect(x, static_cast<int16_t>(baseline_y - 12), plate_w, 16, Color::Black);
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(x + kPadX), baseline_y, label,
                  Color::Black, 1, /*bold=*/true);
    }
}

void draw_limit_row(Canvas& c, Rect area, const char* window_label, const Slot* s, uint32_t now,
                    uint8_t segments) {
    draw_text(c, fonts::Terminus14, area.x, static_cast<int16_t>(area.y + 10), window_label,
              Color::Black, 1, /*bold=*/true);
    String pct = layout::format_percent(s, now);
    // 16px — .num парного окна (Claude 5ч/неделя) в cockpit.html.
    int16_t pw = text_width(fonts::PlexMono16, pct.c_str());
    int16_t label_w = text_width(fonts::Terminus14, window_label);
    int16_t bar_x = static_cast<int16_t>(area.x + label_w + 8);
    int16_t bar_w = static_cast<int16_t>(area.w - label_w - 8 - pw - 8);
    if (bar_w > 0) {
        // Полоса показывает использование, число рядом — остаток: так делает
        // интерфейс Anthropic, и так поставил владелец. Слот несёт остаток,
        // поэтому заполнение — дополнение до ста.
        draw_segbar(c, Rect{bar_x, area.y, bar_w, 14},
                   layout::has_data(s) ? 100.0f - s->number : 0, segments);
    }
    draw_text(c, fonts::PlexMono16, static_cast<int16_t>(area.x + area.w - pw),
              static_cast<int16_t>(area.y + 12), pct.c_str(), Color::Black);
}

bool co2_alarm(float v) { return v >= 1200.0f; }
bool co2_quiet(float v) { return v < 700.0f; }
bool tvoc_alarm(float v) { return v >= 660.0f; }
bool tvoc_quiet(float v) { return v < 220.0f; }

void draw_air_metric(Canvas& c, Rect area, const char* label, const char* unit, const Slot* s,
                     uint32_t now, bool (*is_alarm)(float), bool (*is_quiet)(float)) {
    int16_t baseline = static_cast<int16_t>(area.y + 14);
    draw_text(c, fonts::Terminus14, area.x, baseline, label, Color::Black, 1, /*bold=*/true);
    String val = layout::format_value(s, now, 0, "");
    int16_t label_w = text_width(fonts::Terminus14, label);
    int16_t value_x = static_cast<int16_t>(area.x + label_w + 8);
    draw_text(c, fonts::PlexMono28, value_x, static_cast<int16_t>(baseline + 4), val.c_str(),
              Color::Black);
    int16_t vw = text_width(fonts::PlexMono28, val.c_str());
    draw_text(c, fonts::Terminus14, static_cast<int16_t>(value_x + vw + 6),
              static_cast<int16_t>(baseline + 4), unit, Color::Black, 1, /*bold=*/true);

    if (layout::has_data(s) && s->delta != 0.0f) {
        String delta = format_delta_per_hour(s->delta);
        int16_t dw = text_width(fonts::Terminus14, delta.c_str());
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(area.x + area.w - dw), baseline,
                  delta.c_str(), Color::Black, 1, /*bold=*/true);
    }

    if (!layout::has_data(s)) return;

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
    bool alarm = is_alarm(s->number);
    bool quiet = !alarm && is_quiet(s->number);
    const char* state = alarm ? "ПРОВЕТРИТЬ" : (quiet ? "СВЕЖО" : "НОРМА");
    // График не доходит до правого края: там стоит бейдж состояния, и линия
    // проходила прямо по нему — на живой панели «НОРМА» читалась сквозь
    // штрих. Ширина бейджа известна заранее: текст плюс 6px полей с каждой
    // стороны (см. draw_state_tag), плюс зазор.
    const int16_t tag_w = static_cast<int16_t>(text_width(fonts::Terminus14, state) + 12);
    const int16_t spark_w = static_cast<int16_t>(area.w - tag_w - 8);
    if (spark_w > 20) {
        draw_sparkline(c, Rect{area.x, spark_top, spark_w, kSparkH}, spark, n);
    }

    draw_state_tag(c, static_cast<int16_t>(area.x + area.w),
                   static_cast<int16_t>(spark_top + kSparkH + 6), state, alarm, quiet);
}

// Текст причины отказа для строки лимита: слот `<id>.status` с ok=false и
// непустым error, и только пока настоящих данных нет — как только лимит
// пришёл, причина уступает место шкале. nullptr — показывать нечего.
const char* limits_error_text(const Store& store, const char* status_slot, bool has_data) {
    if (has_data) return nullptr;
    const Slot* st = store.find(status_slot);
    if (st == nullptr || st->ok || st->error.length() == 0) return nullptr;
    return st->error.c_str();
}

// Строка «Claude   <причина>» той же высоты, что строка со шкалой (заголовок
// + 13 px + 14 px полосы), чтобы соседние строки и КАБИНЕТ·ВОЗДУХ ниже не
// ёрзали при переходе из ошибки в данные и обратно. Причина — Terminus14 под
// заголовком, обрезается по ширине с многоточием.
int16_t draw_limit_error_row(Canvas& c, Rect r, int16_t cursor, const char* title,
                             const char* error) {
    const int16_t title_baseline = static_cast<int16_t>(cursor + 26);
    draw_text(c, fonts::Terminus16, r.x, title_baseline, title, Color::Black, 1, /*bold=*/true);
    const int16_t text_baseline = static_cast<int16_t>(title_baseline + 13 + 12);
    String fitted = truncate_to_width(fonts::Terminus14, error, r.w);
    draw_text(c, fonts::Terminus14, r.x, text_baseline, fitted.c_str(), Color::Black);
    return static_cast<int16_t>(title_baseline + 13 + 14);
}

bool has_failure_reason(const Store& store, const char* status_slot) {
    return limits_error_text(store, status_slot, /*has_data=*/false) != nullptr;
}

void draw_limits_and_air(Canvas& c, const Store& store, const layout::DeviceInfo& d, Rect r,
                         bool show_limits, bool show_air) {
    const Slot* claude_5h = store.find("limit.claude.5h");
    const Slot* claude_week = store.find("limit.claude.week");
    const Slot* codex = store.find("limit.codex");
    bool has_claude = layout::has_data(claude_5h) || layout::has_data(claude_week);
    bool has_codex = layout::has_data(codex);
    // Причина отказа источника вместо исчезновения строки: пока данных нет,
    // но коннектор объяснил почему (`<id>.status`, slots::Store::mark_failed),
    // строка остаётся — заголовок и текст причины на месте шкалы. Владелец
    // просил именно это: пустое место читается как «сломалось непонятно что»,
    // «сервер просит подождать (429)» — как состояние (2026-09-21).
    const char* claude_error = limits_error_text(store, "claude.status", has_claude);
    const char* codex_error = limits_error_text(store, "codex.status", has_codex);
    bool has_limits = show_limits && (has_claude || has_codex || claude_error || codex_error);

    const Slot* co2 = store.find("co2");
    const Slot* tvoc = store.find("tvoc");
    bool has_co2 = layout::has_data(co2);
    bool has_tvoc = layout::has_data(tvoc);
    bool has_air = show_air && (has_co2 || has_tvoc);

    // Эйброу текущей рубрики садится туда, куда довёл курсор y — если ЛИМИТЫ
    // отсутствуют вовсе, КАБИНЕТ·ВОЗДУХ окажется в самом верху колонки, ровно
    // как в потоке документа cockpit.html.
    int16_t y = r.y;

    if (has_limits) {
        // «· ОСТАТОК» — явное указание, что число в шкале ниже показывает
        // ОСТАТОК, а не использование (задача: слот теперь несёт
        // remaining_percent(), не utilization — connectors.cpp).
        draw_eyebrow(c, Rect{r.x, y, r.w, 0}, "ЛИМИТЫ · ОСТАТОК");
        int16_t cursor = static_cast<int16_t>(y + kEyebrowTextOffset);

        if (claude_error) {
            cursor = draw_limit_error_row(c, r, cursor, "Claude", claude_error);
        } else if (has_claude) {
            int16_t title_baseline = static_cast<int16_t>(cursor + 26);
            draw_text(c, fonts::Terminus16, r.x, title_baseline, "Claude", Color::Black, 1, /*bold=*/true);
            const Slot* reset = store.find("limit.claude.reset");
            if (layout::has_data(reset)) {
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

        if (codex_error) {
            cursor = draw_limit_error_row(c, r, static_cast<int16_t>(cursor + (has_claude || claude_error ? 1 : 0)),
                                          "GPT", codex_error);
        } else if (has_codex) {
            int16_t title_baseline =
                static_cast<int16_t>(cursor + (has_claude || claude_error ? 27 : 26));
            draw_text(c, fonts::Terminus16, r.x, title_baseline, "GPT", Color::Black, 1, /*bold=*/true);
            const Slot* reset = store.find("limit.codex.reset");
            if (layout::has_data(reset)) {
                int16_t rw = text_width(fonts::Terminus14, reset->text.c_str());
                draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - rw),
                          title_baseline, reset->text.c_str(), Color::Black, 1, /*bold=*/true);
            }
            int16_t bar_y = static_cast<int16_t>(title_baseline + 16);
            String pct = layout::format_percent(codex, d.now);
            // 20px — .num одиночного окна (GPT) в cockpit.html, крупнее
            // парного (16px у Claude выше): в эталоне у одной строки больше
            // свободного места по высоте, чем у половины разделённого блока.
            int16_t pw = text_width(fonts::PlexMono20, pct.c_str());
            int16_t bar_w = static_cast<int16_t>(r.w - pw - 8);
            if (bar_w > 0) {
                draw_segbar(c, Rect{r.x, bar_y, bar_w, 14}, 100.0f - codex->number,  // полоса — использование, число — остаток
                            layout::segments_for_width(bar_w));
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

}  // namespace widgets::prims

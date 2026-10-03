// Раскладка — шапка кадра плюс сборка дашборда по данным. Числа каркаса ниже
// — не оценка на глаз, а обмер эталона тем же способом, каким его потом
// проверяет tools/compare_frame.py (самый длинный сплошной пробег тёмных
// пикселей в строке/столбце — линия, текст такого пробега не даёт). Подробный
// разбор — Status Log в .claude/plans/tablo.md.
//
// Отрисовка конкретных блоков (Рынки, Лимиты, Воздух, Почта, Сегодня, …)
// переехала в widgets/w_*.cpp — единый протокол виджета (widgets/widget.h,
// docs/widgets.md). Здесь остаётся то, что знает только раскладка: шапка,
// арифметика календаря, правило ряда (S/M/flex) и сборка активного
// дашборда — draw_frame() ниже просто раскладывает виджеты по их Spec и не
// знает имён конкретных слотов.
#include "layout.h"

#include <cstdio>
#include <string>

#include "../assets/terminus_14.h"
#include "../assets/terminus_24.h"
#include "config.h"
#include "font.h"
#include "i18n.h"
#include "widgets/prims.h"
#include "widgets/widget.h"
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
// Зазор между виджетами В РЯДУ — раздельно по рядам, TOP_GAP != BOTTOM_GAP в
// эталоне (19 против 17), см. docs/widgets.md, «Правило ряда».
constexpr int16_t TOP_GAP = 19;     // между Рынками и правой колонкой
constexpr int16_t BOTTOM_GAP = 17;  // между Почтой и Сегодня

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
    std::snprintf(date_buf, sizeof(date_buf), "%s · %d %s", i18n::weekday(now_civil.weekday),
                  now_civil.day, i18n::month(now_civil.month - 1));
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
        draw_battery_icon(c, x, 26, d.battery_pct);  // низ на базовой линии текста (38)
    } else {
        const char* usb = "USB";
        int16_t w = text_width(fonts::Terminus14, usb);
        x = static_cast<int16_t>(x - w);
        draw_text(c, fonts::Terminus14, x, 38, usb, Color::Black, 1, /*bold=*/true);
    }
    x = static_cast<int16_t>(x - 16 - 22);
    draw_wifi_bars(c, x, 22, wifi_bars(d.wifi_rssi));  // низ на базовой линии текста (38)

    c.fill_rect(MARGIN, HEADER_RULE_Y, static_cast<int16_t>(c.width() - 2 * MARGIN), 2,
                Color::Black);
}

}  // namespace

String format_decimal(float value, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(value));
    // Десятичный разделитель — по языку экрана: «96,40» для ru, «96.40» для en.
    const char point = i18n::lang() == i18n::Lang::kRu ? ',' : '.';
    for (char* p = buf; *p; ++p) {
        if (*p == '.') *p = point;
    }

    // Разряды целой части разделяем пробелом: «80 689» вместо «80689». На
    // пятизначном курсе без разделителя читатель считает нули глазами, а
    // эталон эту группировку делает. Работаем с буфером, а не со String:
    // хостовый шим не умеет индексацию, а вести две ветки ради форматирования
    // числа — лишнее.
    int int_end = 0;
    while (buf[int_end] != '\0' && buf[int_end] != point) ++int_end;
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

void layout_row(Rect row, int16_t gap, const std::vector<widgets::Instance>& items,
                const std::vector<bool>& visible, Rect* out) {
    const size_t n = items.size();
    for (size_t i = 0; i < n; ++i) out[i] = Rect{};

    size_t visible_count = 0;
    size_t flex_count = 0;
    int16_t fixed_sum = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!visible[i]) continue;
        ++visible_count;
        if (items[i].size == widgets::Size::kFlex) {
            ++flex_count;
        } else {
            fixed_sum = static_cast<int16_t>(fixed_sum + widgets::size_px(items[i].size));
        }
    }
    if (visible_count == 0) return;

    const int16_t gap_total = static_cast<int16_t>(gap * static_cast<int16_t>(visible_count - 1));

    // Нет ни одного flex среди видимых — видимые делят весь ряд поровну
    // (docs/widgets.md, «Правило ряда»). При visible_count==1 это отдаёт всю
    // ширину единственному видимому — так Рынки в эталоне занимают весь
    // верхний ряд, когда правая колонка пропала.
    // Переполнение: фиксированные ширины (плюс минимум на каждый flex) не
    // влезают в ряд — например три M по 296 в 770 px. Без этой ветки flex
    // получал бы отрицательную ширину, а хвост ряда уезжал за край панели
    // молча. Делим поровну между видимыми — кадр остаётся в границах, а
    // владелец видит на предпросмотре и на панели одно и то же.
    constexpr int16_t kMinFlexWidth = 120;
    const bool overflow =
        fixed_sum + gap_total + static_cast<int16_t>(flex_count) * kMinFlexWidth > row.w;
    const bool equal_split = flex_count == 0 || overflow;

    int16_t equal_w = 0;
    int16_t flex_w = 0;
    if (equal_split) {
        equal_w = static_cast<int16_t>((row.w - gap_total) / static_cast<int16_t>(visible_count));
    } else {
        const int16_t remaining = static_cast<int16_t>(row.w - fixed_sum - gap_total);
        flex_w = static_cast<int16_t>(remaining / static_cast<int16_t>(flex_count));
    }

    int16_t cursor = row.x;
    for (size_t i = 0; i < n; ++i) {
        if (!visible[i]) continue;
        const int16_t w = equal_split ? equal_w
                          : items[i].size == widgets::Size::kFlex
                              ? flex_w
                              : widgets::size_px(items[i].size);
        out[i] = Rect{cursor, row.y, w, row.h};
        cursor = static_cast<int16_t>(cursor + w + gap);
    }
}

void draw_frame(Canvas& canvas, const Store& store, const DeviceInfo& device,
                const config::Dashboard& dashboard) {
    canvas.fill(Color::White);
    draw_header(canvas, device);

    Rect body{MARGIN, BODY_TOP, static_cast<int16_t>(canvas.width() - 2 * MARGIN),
              static_cast<int16_t>(canvas.height() - BODY_TOP - MARGIN)};

    // Видимость каждого инстанса — по его widgets::Spec::visible: раскладка
    // не знает имён слотов, только у кого спросить.
    std::vector<bool> visible[2];
    bool row_has[2] = {false, false};
    for (int r = 0; r < 2; ++r) {
        const std::vector<widgets::Instance>& items = dashboard.rows[r];
        visible[r].assign(items.size(), false);
        for (size_t i = 0; i < items.size(); ++i) {
            const widgets::Spec* spec = widgets::find(items[i].type.c_str());
            const bool v = spec != nullptr && spec->visible(store, items[i]);
            visible[r][i] = v;
            if (v) row_has[r] = true;
        }
    }

    Rect row1, row2;
    compute_two_rows(body, ROW_GAP, row_has[0], row_has[1], &row1, &row2);

    if (row_has[0] && row_has[1]) {
        // Разделитель садится на границу рядов, а не в середину зазора — в
        // эталоне зазор целиком идёт ПОСЛЕ линии (298→310 у div, а не по 6px
        // с обеих сторон).
        const int16_t rule_w = static_cast<int16_t>(canvas.width() - 2 * MARGIN);
        canvas.hline(MARGIN, static_cast<int16_t>(row1.y + row1.h), rule_w, Color::Black);
        canvas.hline(MARGIN, static_cast<int16_t>(row1.y + row1.h + 1), rule_w, Color::Black);
    }

    const int16_t row_gap[2] = {TOP_GAP, BOTTOM_GAP};
    const Rect rows[2] = {row1, row2};
    for (int r = 0; r < 2; ++r) {
        if (!row_has[r]) continue;
        const std::vector<widgets::Instance>& items = dashboard.rows[r];
        std::vector<Rect> rects(items.size());
        layout_row(rows[r], row_gap[r], items, visible[r], rects.data());

        bool drawn_any = false;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!visible[r][i]) continue;
            const widgets::Instance& item = items[i];
            if (item.divider && drawn_any) {
                // Вертикальная линия 2px в зазоре слева от виджета — тот же
                // приём, что раньше был жёстко зашит для Сегодня (два
                // соседних vline вместо толщины линии в canvas).
                const int16_t x2 = static_cast<int16_t>(rects[i].x - row_gap[r] + 2);
                const int16_t x1 = static_cast<int16_t>(rects[i].x - row_gap[r] + 1);
                canvas.vline(x2, rows[r].y, rows[r].h, Color::Black);
                canvas.vline(x1, rows[r].y, rows[r].h, Color::Black);
            }
            const widgets::Spec* spec = widgets::find(item.type.c_str());
            if (spec != nullptr) spec->draw(canvas, store, device, rects[i], item);
            drawn_any = true;
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

void draw_boot(Canvas& canvas, const char* status) {
    canvas.fill(Color::White);
    const int16_t cx = static_cast<int16_t>(canvas.width() / 2);

    // Угловые метки — тот же 2px штрих, что у линий кадра: экран включения
    // должен выглядеть частью того же прибора, а не заставкой другой программы.
    constexpr int16_t kTick = 24;
    const int16_t right = static_cast<int16_t>(canvas.width() - MARGIN);
    const int16_t bottom = static_cast<int16_t>(canvas.height() - MARGIN);
    canvas.fill_rect(MARGIN, MARGIN, kTick, 2, Color::Black);
    canvas.fill_rect(MARGIN, MARGIN, 2, kTick, Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - kTick), MARGIN, kTick, 2, Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - 2), MARGIN, 2, kTick, Color::Black);
    canvas.fill_rect(MARGIN, static_cast<int16_t>(bottom - 2), kTick, 2, Color::Black);
    canvas.fill_rect(MARGIN, static_cast<int16_t>(bottom - kTick), 2, kTick, Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - kTick), static_cast<int16_t>(bottom - 2), kTick, 2,
                     Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - 2), static_cast<int16_t>(bottom - kTick), 2, kTick,
                     Color::Black);

    // Имя — Terminus24 при scale=2, как пароль точки доступа: у PlexMono букв
    // нет (только цифры и знаки), а второго крупного шрифта с латиницей мы
    // намеренно не заводили (font.h).
    const char* name = "tablo";
    const int16_t name_w = static_cast<int16_t>(text_width(fonts::Terminus24, name) * 2);
    draw_text(canvas, fonts::Terminus24, static_cast<int16_t>(cx - name_w / 2), 228, name,
              Color::Black, 2, /*bold=*/true);

    // Короткая линия под именем — как правило под шапкой кадра, но по ширине
    // слова, не экрана: это подпись к имени, а не разделитель рядов.
    canvas.fill_rect(static_cast<int16_t>(cx - name_w / 2), 246, name_w, 2, Color::Black);

    const char* tagline = i18n::tr(i18n::Str::kTagline);
    const int16_t tag_w = text_width(fonts::Terminus14, tagline);
    draw_text(canvas, fonts::Terminus14, static_cast<int16_t>(cx - tag_w / 2), 276, tagline,
              Color::Black, 1, /*bold=*/true);

    // Состояние — внизу слева, где в рабочем кадре стоит почта: глаз уже
    // приучен искать «что сейчас происходит» там.
    draw_text(canvas, fonts::Terminus14, static_cast<int16_t>(MARGIN + 12),
              static_cast<int16_t>(bottom - 12), status, Color::Black, 1, false);
    const char* hw = "TRMNL 7.5\" · XIAO ESP32-S3";
    const int16_t hw_w = text_width(fonts::Terminus14, hw);
    draw_text(canvas, fonts::Terminus14, static_cast<int16_t>(right - 12 - hw_w),
              static_cast<int16_t>(bottom - 12), hw, Color::Black, 1, false);
}

void draw_ap_credentials(Canvas& canvas, const String& ssid, const String& password) {
    canvas.fill(Color::White);

    draw_text(canvas, fonts::Terminus24, MARGIN, AP_TITLE_Y, i18n::tr(i18n::Str::kApSetup), Color::Black, 1,
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
    widgets::prims::draw_eyebrow(canvas, Rect{MARGIN, y, text_w, 0}, i18n::tr(i18n::Str::kNetwork));
    y = static_cast<int16_t>(y + widgets::prims::kEyebrowTextOffset + 34);
    draw_text(canvas, fonts::Terminus24, MARGIN, y,
              widgets::prims::truncate_to_width(fonts::Terminus24, ssid.c_str(), text_w).c_str(),
              Color::Black, 1, /*bold=*/true);

    y = static_cast<int16_t>(y + 50);
    widgets::prims::draw_eyebrow(canvas, Rect{MARGIN, y, text_w, 0}, i18n::tr(i18n::Str::kPassword));
    y = static_cast<int16_t>(y + widgets::prims::kEyebrowTextOffset + 56);
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
              i18n::tr(i18n::Str::kApHint1), Color::Black, 1, false);
    y = static_cast<int16_t>(y + 22);
    draw_text(canvas, fonts::Terminus14, MARGIN, y,
              i18n::tr(i18n::Str::kApHint2), Color::Black, 1, false);
}

}  // namespace layout

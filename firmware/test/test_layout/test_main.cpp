// Тесты чистой логики раскладки — видимость виджетов, пересборка ряда при
// отсутствии части из них, форматирование значений. Рисование (draw_frame и
// widgets::w_*.cpp::*_draw) не тестируется юнит-тестами: у него нет
// числового результата, который стоило бы сверять построчно — сверка глазами
// через tools/render_frame/build_and_run.sh (см. .claude/plans/inkroam.md).
//
// layout.cpp подключается исходником по тому же приёму, что и slots.cpp/
// connectors.cpp в test_slots — test_build_src не включён, см. их комментарий.
// Реестр виджетов (widgets/*.cpp) — тем же приёмом: draw_frame и
// widgets::find/all зовут друг друга, а раздельная компиляция здесь не
// нужна ровно как остальным исходникам этого теста.
#include <unity.h>

#include "../../src/canvas.cpp"
#include "../../src/canvas_mem.cpp"
#include "../../src/font.cpp"
#include "../../src/wifi_qr.cpp"  // layout.cpp зовёт wifi_qr:: в draw_ap_credentials
#include "../../src/widgets/types.cpp"
#include "../../src/widgets/prims.cpp"
#include "../../src/widgets/w_markets.cpp"
#include "../../src/widgets/w_limits.cpp"
#include "../../src/widgets/w_air.cpp"
#include "../../src/widgets/w_limits_air.cpp"
#include "../../src/widgets/w_mail.cpp"
#include "../../src/widgets/w_today.cpp"
#include "../../src/widgets/w_metric.cpp"
#include "../../src/widgets/w_text.cpp"
#include "../../src/widgets/registry.cpp"
#include "../../src/layout.cpp"
#include "../../src/slots.cpp"

using namespace layout;
using slots::Slot;
using slots::Store;

void setUp() {}
void tearDown() {}

// ── wifi_bars ──

static void test_wifi_bars_zero_is_no_signal_sentinel() {
    TEST_ASSERT_EQUAL(0, wifi_bars(0));
}

static void test_wifi_bars_boundaries() {
    TEST_ASSERT_EQUAL(4, wifi_bars(-55));
    TEST_ASSERT_EQUAL(3, wifi_bars(-56));
    TEST_ASSERT_EQUAL(3, wifi_bars(-65));
    TEST_ASSERT_EQUAL(2, wifi_bars(-66));
    TEST_ASSERT_EQUAL(2, wifi_bars(-75));
    TEST_ASSERT_EQUAL(1, wifi_bars(-76));
    TEST_ASSERT_EQUAL(1, wifi_bars(-95));
}

// ── segments_for_width ──

static void test_segments_for_width_clamps_low_end() {
    // Узкая колонка (лимит 5ч/неделя в дизайне занимает мало места) — не
    // меньше 4 сегментов, даже если по формуле вышло бы меньше.
    TEST_ASSERT_EQUAL(4, segments_for_width(5));
    TEST_ASSERT_EQUAL(4, segments_for_width(39));  // 39/10 = 3 -> поднято до 4
}

static void test_segments_for_width_clamps_high_end() {
    TEST_ASSERT_EQUAL(20, segments_for_width(500));
}

static void test_segments_for_width_scales_with_available_width() {
    TEST_ASSERT_EQUAL(10, segments_for_width(100));
    TEST_ASSERT_EQUAL(15, segments_for_width(150));
}

// ── to_civil ──

static void test_to_civil_unix_epoch_is_thursday() {
    Civil c = to_civil(0, 0);
    TEST_ASSERT_EQUAL(3, c.weekday);  // Чт — индекс 3 в WEEKDAYS (Пн=0)
    TEST_ASSERT_EQUAL(1, c.day);
    TEST_ASSERT_EQUAL(1, c.month);
    TEST_ASSERT_EQUAL(0, c.hour);
    TEST_ASSERT_EQUAL(0, c.minute);
}

// 2025-09-16 08:20:00 UTC+3, известное значение, сверено с python
// datetime.utcfromtimestamp(1758000000 + 180*60) == 2025-09-16 08:20:00, Tuesday.
static void test_to_civil_known_date_with_timezone() {
    Civil c = to_civil(1758000000u, 180);
    TEST_ASSERT_EQUAL(1, c.weekday);  // Вт
    TEST_ASSERT_EQUAL(16, c.day);
    TEST_ASSERT_EQUAL(9, c.month);
    TEST_ASSERT_EQUAL(8, c.hour);
    TEST_ASSERT_EQUAL(20, c.minute);
}

static void test_to_civil_negative_timezone_offset_rolls_back_a_day() {
    // Полночь UTC минус смещение уводит локальное время в предыдущие сутки —
    // проверка floor_div/floor_mod, а не usual truncating division.
    Civil utc_midnight = to_civil(86400u * 10, 0);  // 1970-01-11 00:00 UTC
    Civil shifted = to_civil(86400u * 10, -60);     // тот же момент, UTC-1
    TEST_ASSERT_EQUAL(11, utc_midnight.day);
    TEST_ASSERT_EQUAL(10, shifted.day);
    TEST_ASSERT_EQUAL(23, shifted.hour);
}

// ── has_data / format_percent / format_value ──

static void test_has_data_null_is_false() { TEST_ASSERT_FALSE(has_data(nullptr)); }

static void test_has_data_empty_slot_is_false() {
    Slot s;
    s.ok = false;
    TEST_ASSERT_FALSE(has_data(&s));
}

static void test_has_data_present_slot_is_true() {
    Slot s;
    s.ok = true;
    s.at = 100;
    TEST_ASSERT_TRUE(has_data(&s));
}

static void test_format_percent_dash_when_no_data() {
    TEST_ASSERT_EQUAL_STRING("—", format_percent(nullptr, 1000).c_str());
}

static void test_format_percent_fresh_has_no_mark() {
    Slot s;
    s.ok = true;
    s.at = 990;
    s.ttl = 60;
    s.number = 41.6f;
    TEST_ASSERT_EQUAL_STRING("42%", format_percent(&s, 1000).c_str());
}

static void test_format_percent_stale_gets_approx_mark() {
    Slot s;
    s.ok = true;
    s.at = 100;
    s.ttl = 60;
    s.number = 50;
    TEST_ASSERT_EQUAL_STRING("≈50%", format_percent(&s, 1000).c_str());
}

static void test_format_percent_clamps_to_0_100() {
    Slot over;
    over.ok = true;
    over.at = 990;
    over.ttl = 60;
    over.number = 142;
    TEST_ASSERT_EQUAL_STRING("100%", format_percent(&over, 1000).c_str());

    Slot under;
    under.ok = true;
    under.at = 990;
    under.ttl = 60;
    under.number = -5;
    TEST_ASSERT_EQUAL_STRING("0%", format_percent(&under, 1000).c_str());
}

static void test_format_value_dash_when_no_data() {
    TEST_ASSERT_EQUAL_STRING("—", format_value(nullptr, 1000, 2, "").c_str());
}

static void test_format_value_fresh_decimals_and_suffix() {
    Slot s;
    s.ok = true;
    s.at = 990;
    s.ttl = 60;
    s.number = 96.4f;
    TEST_ASSERT_EQUAL_STRING("96,40", format_value(&s, 1000, 2, "").c_str());
}

static void test_format_value_stale_gets_approx_mark() {
    Slot s;
    s.ok = true;
    s.at = 100;  // at > 0 обязательно — иначе Slot::empty() отсечёт слот до
                 // format_value и тест проверял бы уже другую ветку (дефис).
    s.ttl = 60;
    s.number = 620;
    TEST_ASSERT_EQUAL_STRING("≈620", format_value(&s, 1000, 0, "").c_str());
}

// ── видимость виджетов (widgets::Spec::visible — раньше жила здесь пятью
// функциями rates_visible/limits_visible/..., переехала в widgets/w_*.cpp,
// см. .claude/plans/constructor.md) ──

static void put_ok(Store& store, const char* id, float number = 1) {
    Slot s;
    s.ok = true;
    s.at = 100;
    s.ttl = 60;
    s.number = number;
    store.put(id, s);
}

static bool widget_visible(const char* type, const Store& store) {
    const widgets::Spec* spec = widgets::find(type);
    TEST_ASSERT_NOT_NULL(spec);
    return spec->visible(store, widgets::Instance{});
}

static void test_markets_visible_by_any_of_three_slots() {
    Store store;
    TEST_ASSERT_FALSE(widget_visible("markets", store));
    put_ok(store, "eur_rub");
    TEST_ASSERT_TRUE(widget_visible("markets", store));
}

static void test_limits_visible_by_any_window() {
    Store store;
    TEST_ASSERT_FALSE(widget_visible("limits", store));
    put_ok(store, "limit.codex");
    TEST_ASSERT_TRUE(widget_visible("limits", store));
}

static void test_limits_visible_by_failure_reason_alone() {
    Store store;
    store.mark_failed(String("claude"), String("сервер просит подождать (429)"));
    TEST_ASSERT_TRUE(widget_visible("limits", store));
    TEST_ASSERT_TRUE(widget_visible("limits_air", store));
}

static void test_limits_visible_by_reason_after_data_went_stale() {
    // Тёплый путь: лимит был, потом 429 — блок остаётся и объясняет причину.
    Store store;
    Slot v; v.ok = true; v.at = 100; v.ttl = 60; v.number = 42;
    store.put("limit.claude.5h", v, "claude");
    store.mark_failed(String("claude"), String("сервер просит подождать (429)"));
    TEST_ASSERT_TRUE(widget_visible("limits", store));
}

static void test_limits_hidden_when_failure_has_no_reason() {
    Store store;
    store.mark_failed(String("claude"));  // без причины — заглушки нет, сказать нечего
    TEST_ASSERT_FALSE(widget_visible("limits", store));
}

static void test_limits_error_row_renders_reason_text() {
    // Кадр с одной лишь причиной отказа рисуется без падения и не пуст в
    // области блока — прямой контракт «показывать ошибку, а не прятать блок».
    Store store;
    store.mark_failed(String("claude"), String("сервер просит подождать (429)"));
    canvas::CanvasMemory cv(800, 480);
    cv.fill(canvas::Color::White);
    DeviceInfo d;
    widgets::Instance inst;
    inst.type = "limits";
    widgets::find("limits")->draw(cv, store, d, Rect{330, 60, 455, 200}, inst);
    int dark = 0;
    for (int y = 60; y < 140; ++y)
        for (int x = 330; x < 785; ++x)
            if (cv.at(x, y)) ++dark;
    TEST_ASSERT_TRUE(dark > 200);
}

static void test_air_visible_by_co2_or_tvoc() {
    Store store;
    TEST_ASSERT_FALSE(widget_visible("air", store));
    put_ok(store, "tvoc");
    TEST_ASSERT_TRUE(widget_visible("air", store));
}

static void test_limits_air_visible_by_either_half() {
    Store store;
    TEST_ASSERT_FALSE(widget_visible("limits_air", store));
    put_ok(store, "co2");
    TEST_ASSERT_TRUE(widget_visible("limits_air", store));
}

static void test_mail_visible_requires_unread_counter() {
    Store store;
    TEST_ASSERT_FALSE(widget_visible("mail", store));
    put_ok(store, "mail.unread", 0);  // счётчик есть, даже если равен нулю —
    // Slot::empty() решает по ok/at, не по числовому значению.
    TEST_ASSERT_TRUE(widget_visible("mail", store));
}

static void test_today_visible_by_weather_or_first_event() {
    Store store;
    TEST_ASSERT_FALSE(widget_visible("today", store));
    put_ok(store, "event.1.title");
    TEST_ASSERT_TRUE(widget_visible("today", store));
}

static void test_metric_visible_by_its_own_slot() {
    Store store;
    widgets::Instance instance;
    instance.type = "metric";
    instance.slot = "co2";
    const widgets::Spec* spec = widgets::find("metric");
    TEST_ASSERT_FALSE(spec->visible(store, instance));
    put_ok(store, "co2");
    TEST_ASSERT_TRUE(spec->visible(store, instance));
}

static void test_text_is_always_visible() {
    Store store;
    TEST_ASSERT_TRUE(widget_visible("text", store));
}

static void test_find_unknown_type_is_null() { TEST_ASSERT_NULL(widgets::find("bogus")); }

// ── реестр (widgets::all) согласован с лёгкой таблицей типов
// (widgets::find_type, widgets/types.cpp): default_size/min_width
// дублируются намеренно (docs/widgets.md, types.h) — этот тест держит их в
// одном значении, а не полагается на память при следующей правке.
static void test_widgets_registry_matches_types_table() {
    size_t count = 0;
    const widgets::Spec* const* all = widgets::all(&count);
    TEST_ASSERT_TRUE(count > 0);
    for (size_t i = 0; i < count; ++i) {
        const widgets::TypeInfo* info = widgets::find_type(all[i]->type);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_TRUE(info->default_size == all[i]->default_size);
        TEST_ASSERT_EQUAL(info->min_width, all[i]->min_width);
    }
}

// ── required_slots ──

static void test_required_slots_for_static_widget_lists_spec_slots() {
    widgets::Instance instance;
    instance.type = "air";
    std::vector<String> out;
    widgets::required_slots(instance, out);
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL_STRING("co2", out[0].c_str());
    TEST_ASSERT_EQUAL_STRING("tvoc", out[1].c_str());
}

static void test_required_slots_for_metric_uses_instance_slot() {
    widgets::Instance instance;
    instance.type = "metric";
    instance.slot = "weather.temp";
    std::vector<String> out;
    widgets::required_slots(instance, out);
    TEST_ASSERT_EQUAL(1, out.size());
    TEST_ASSERT_EQUAL_STRING("weather.temp", out[0].c_str());
}

static void test_required_slots_for_metric_without_slot_is_empty() {
    widgets::Instance instance;
    instance.type = "metric";
    std::vector<String> out;
    widgets::required_slots(instance, out);
    TEST_ASSERT_EQUAL(0, out.size());
}

// ── compute_columns ──

static void test_compute_columns_all_visible_equal_width() {
    Rect row{0, 10, 300, 50};
    bool visible[3] = {true, true, true};
    Rect out[3];
    compute_columns(row, 12, visible, 3, out);

    // (300 - 2*12) / 3 = 92
    TEST_ASSERT_EQUAL(92, out[0].w);
    TEST_ASSERT_EQUAL(92, out[1].w);
    TEST_ASSERT_EQUAL(92, out[2].w);
    TEST_ASSERT_EQUAL(0, out[0].x);
    TEST_ASSERT_EQUAL(104, out[1].x);   // 0 + 92 + 12
    TEST_ASSERT_EQUAL(208, out[2].x);   // 104 + 92 + 12
    TEST_ASSERT_EQUAL(10, out[0].y);
    TEST_ASSERT_EQUAL(50, out[0].h);
}

static void test_compute_columns_missing_one_widens_the_rest_without_gap() {
    Rect row{0, 0, 300, 50};
    bool visible[3] = {true, false, true};
    Rect out[3];
    compute_columns(row, 12, visible, 3, out);

    // Средний невидим — оставшиеся два делят всю ширину пополам, без дыры.
    TEST_ASSERT_EQUAL(144, out[0].w);  // (300-12)/2
    TEST_ASSERT_EQUAL(144, out[2].w);
    TEST_ASSERT_EQUAL(0, out[1].w);
    TEST_ASSERT_EQUAL(0, out[1].x);
}

static void test_compute_columns_none_visible_all_zero() {
    Rect row{0, 0, 300, 50};
    bool visible[3] = {false, false, false};
    Rect out[3];
    compute_columns(row, 12, visible, 3, out);
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL(0, out[i].w);
        TEST_ASSERT_EQUAL(0, out[i].h);
    }
}

static void test_compute_columns_single_visible_takes_full_width() {
    Rect row{5, 0, 300, 50};
    bool visible[3] = {false, true, false};
    Rect out[3];
    compute_columns(row, 12, visible, 3, out);
    TEST_ASSERT_EQUAL(300, out[1].w);
    TEST_ASSERT_EQUAL(5, out[1].x);
}

// ── compute_columns_fixed_first / _fixed_second (верхний/нижний ряд эталона:
// Рынки flex:none слева, Сегодня flex:none справа — остальное гибкое) ──

static void test_fixed_first_both_visible_first_keeps_fixed_width() {
    Rect row{15, 0, 770, 200};
    Rect first, second;
    compute_columns_fixed_first(row, 19, true, true, 296, &first, &second);
    TEST_ASSERT_EQUAL(15, first.x);
    TEST_ASSERT_EQUAL(296, first.w);
    TEST_ASSERT_EQUAL(330, second.x);  // 15 + 296 + 19
    TEST_ASSERT_EQUAL(455, second.w);  // 770 - 296 - 19
}

static void test_fixed_first_only_first_gets_full_row() {
    Rect row{15, 0, 770, 200};
    Rect first, second;
    compute_columns_fixed_first(row, 19, true, false, 296, &first, &second);
    TEST_ASSERT_EQUAL(770, first.w);
    TEST_ASSERT_EQUAL(0, second.w);
}

static void test_fixed_first_only_second_gets_full_row() {
    Rect row{15, 0, 770, 200};
    Rect first, second;
    compute_columns_fixed_first(row, 19, false, true, 296, &first, &second);
    TEST_ASSERT_EQUAL(770, second.w);
    TEST_ASSERT_EQUAL(15, second.x);
    TEST_ASSERT_EQUAL(0, first.w);
}

static void test_fixed_first_neither_visible_both_zero() {
    Rect row{15, 0, 770, 200};
    Rect first, second;
    compute_columns_fixed_first(row, 19, false, false, 296, &first, &second);
    TEST_ASSERT_EQUAL(0, first.w);
    TEST_ASSERT_EQUAL(0, second.w);
}

static void test_fixed_second_both_visible_second_keeps_fixed_width() {
    Rect row{15, 0, 770, 150};
    Rect first, second;
    compute_columns_fixed_second(row, 17, true, true, 202, &first, &second);
    TEST_ASSERT_EQUAL(202, second.w);
    TEST_ASSERT_EQUAL(583, second.x);  // 15 + 770 - 202
    TEST_ASSERT_EQUAL(15, first.x);
    TEST_ASSERT_EQUAL(551, first.w);  // 583 - 17 - 15
}

static void test_fixed_second_only_first_gets_full_row() {
    Rect row{15, 0, 770, 150};
    Rect first, second;
    compute_columns_fixed_second(row, 17, true, false, 202, &first, &second);
    TEST_ASSERT_EQUAL(770, first.w);
    TEST_ASSERT_EQUAL(0, second.w);
}

static void test_fixed_second_only_second_gets_full_row() {
    Rect row{15, 0, 770, 150};
    Rect first, second;
    compute_columns_fixed_second(row, 17, false, true, 202, &first, &second);
    TEST_ASSERT_EQUAL(770, second.w);
    TEST_ASSERT_EQUAL(0, first.w);
}

// ── compute_two_rows ──

static void test_compute_two_rows_both_present_split_body() {
    Rect body{0, 100, 400, 200};
    Rect r1, r2;
    compute_two_rows(body, 16, true, true, &r1, &r2);
    TEST_ASSERT_TRUE(r1.h > 0);
    TEST_ASSERT_TRUE(r2.h > 0);
    TEST_ASSERT_EQUAL(body.y, r1.y);
    TEST_ASSERT_EQUAL(r1.y + r1.h + 16, r2.y);
    TEST_ASSERT_EQUAL(body.h, r1.h + 16 + r2.h);
    // Пропорция — не круглое число «пополам», а обмер эталона (235px верхний
    // ряд / 154px нижний на теле кадра 390px без зазора — Status Log в
    // .claude/plans/inkroam.md). Число фиксирует именно эту пропорцию
    // (~60/40), а не только «оба > 0», иначе её смена осталась бы незамеченной.
    TEST_ASSERT_EQUAL(110, r1.h);  // (200-16)*0.603, целочисленно
    TEST_ASSERT_EQUAL(74, r2.h);
}

static void test_compute_two_rows_only_first_gets_full_body() {
    Rect body{0, 100, 400, 200};
    Rect r1, r2;
    compute_two_rows(body, 16, true, false, &r1, &r2);
    TEST_ASSERT_EQUAL(200, r1.h);
    TEST_ASSERT_EQUAL(0, r2.h);
}

static void test_compute_two_rows_only_second_gets_full_body() {
    Rect body{0, 100, 400, 200};
    Rect r1, r2;
    compute_two_rows(body, 16, false, true, &r1, &r2);
    TEST_ASSERT_EQUAL(0, r1.h);
    TEST_ASSERT_EQUAL(200, r2.h);
    TEST_ASSERT_EQUAL(100, r2.y);
}

static void test_compute_two_rows_neither_present_both_zero() {
    Rect body{0, 100, 400, 200};
    Rect r1, r2;
    compute_two_rows(body, 16, false, false, &r1, &r2);
    TEST_ASSERT_EQUAL(0, r1.h);
    TEST_ASSERT_EQUAL(0, r2.h);
}

// ── layout_row — общее правило ряда дашборда (S/M/flex), docs/widgets.md.
// Заводской «Стол» — компоновка через эту функцию должна дать те же числа,
// что раньше давали compute_columns_fixed_first/_second выше (см. их тесты):
// это и есть проверка, что переход на дашборды не сдвинул эталон.

static widgets::Instance sized(widgets::Size size, bool divider = false) {
    widgets::Instance i;
    i.size = size;
    i.divider = divider;
    return i;
}

static void test_layout_row_fixed_m_plus_flex_matches_fixed_first() {
    // Верхний ряд «Стола»: Рынки (M, 296) + Лимиты-и-Воздух (flex) — тот же
    // row/gap, что test_fixed_first_both_visible_first_keeps_fixed_width.
    Rect row{15, 0, 770, 200};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kM), sized(widgets::Size::kFlex)};
    std::vector<bool> visible = {true, true};
    Rect out[2];
    layout_row(row, 19, items, visible, out);
    TEST_ASSERT_EQUAL(15, out[0].x);
    TEST_ASSERT_EQUAL(296, out[0].w);
    TEST_ASSERT_EQUAL(330, out[1].x);  // 15 + 296 + 19
    TEST_ASSERT_EQUAL(455, out[1].w);  // 770 - 296 - 19
}

static void test_layout_row_flex_plus_fixed_s_matches_fixed_second() {
    // Нижний ряд «Стола»: Почта (flex) + Сегодня (S, 202) — тот же row/gap,
    // что test_fixed_second_both_visible_second_keeps_fixed_width.
    Rect row{15, 0, 770, 150};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kFlex),
                                             sized(widgets::Size::kS, /*divider=*/true)};
    std::vector<bool> visible = {true, true};
    Rect out[2];
    layout_row(row, 17, items, visible, out);
    TEST_ASSERT_EQUAL(202, out[1].w);
    TEST_ASSERT_EQUAL(583, out[1].x);  // 15 + 770 - 202
    TEST_ASSERT_EQUAL(15, out[0].x);
    TEST_ASSERT_EQUAL(551, out[0].w);  // 583 - 17 - 15
}

static void test_layout_row_single_visible_takes_full_width_regardless_of_token() {
    // Правая колонка пропала — единственный видимый (M) получает всю
    // ширину ряда, не свои номинальные 296px: n=1, flex среди видимых нет —
    // «делят поровну» на одного и есть вся ширина (docs/widgets.md).
    Rect row{15, 0, 770, 200};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kM), sized(widgets::Size::kFlex)};
    std::vector<bool> visible = {true, false};
    Rect out[2];
    layout_row(row, 19, items, visible, out);
    TEST_ASSERT_EQUAL(770, out[0].w);
    TEST_ASSERT_EQUAL(0, out[1].w);
}

static void test_layout_row_no_flex_among_visible_splits_evenly() {
    Rect row{0, 0, 300, 50};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kM), sized(widgets::Size::kS)};
    std::vector<bool> visible = {true, true};
    Rect out[2];
    layout_row(row, 12, items, visible, out);
    // (300-12)/2 = 144, не 296/202 — оба видимых без единого flex делят ряд
    // поровну, номинальные S/M-токены здесь не участвуют.
    TEST_ASSERT_EQUAL(144, out[0].w);
    TEST_ASSERT_EQUAL(144, out[1].w);
}

static void test_layout_row_multiple_flex_share_remainder_equally() {
    Rect row{0, 0, 300, 50};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kFlex), sized(widgets::Size::kFlex)};
    std::vector<bool> visible = {true, true};
    Rect out[2];
    layout_row(row, 12, items, visible, out);
    TEST_ASSERT_EQUAL(144, out[0].w);  // (300-12)/2, как и без токенов
    TEST_ASSERT_EQUAL(144, out[1].w);
}

static void test_layout_row_none_visible_all_zero() {
    Rect row{0, 0, 300, 50};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kFlex), sized(widgets::Size::kFlex)};
    std::vector<bool> visible = {false, false};
    Rect out[2];
    layout_row(row, 12, items, visible, out);
    TEST_ASSERT_EQUAL(0, out[0].w);
    TEST_ASSERT_EQUAL(0, out[1].w);
}

// ── font: decode_utf8 / find_glyph / text_width (byстрая проверка на границе
// ASCII/кириллицы, чтобы не полагаться только на глазастую сверку PNG) ──

static void test_decode_utf8_ascii() {
    const char* text = "A";
    const char* p = text;
    uint32_t cp = fonts::decode_utf8(p);
    TEST_ASSERT_EQUAL_UINT32('A', cp);
    TEST_ASSERT_EQUAL_PTR(text + 1, p);
}

static void test_decode_utf8_two_byte_cyrillic() {
    const char* text = "Б";  // U+0411, UTF-8: D0 91
    const char* p = text;
    uint32_t cp = fonts::decode_utf8(p);
    TEST_ASSERT_EQUAL_UINT32(0x411, cp);
    TEST_ASSERT_EQUAL_PTR(text + 2, p);
}

static void test_find_glyph_known_and_unknown_codepoint() {
    TEST_ASSERT_NOT_NULL(fonts::find_glyph(fonts::Terminus16, 'A'));
    TEST_ASSERT_NOT_NULL(fonts::find_glyph(fonts::Terminus16, 0x411));  // Б
    TEST_ASSERT_NULL(fonts::find_glyph(fonts::Terminus16, 0x10FFFF));   // заведомо нет
}

static void test_text_width_is_sum_of_advances_for_monospace() {
    int16_t one = fonts::text_width(fonts::Terminus16, "A");
    int16_t three = fonts::text_width(fonts::Terminus16, "AAA");
    TEST_ASSERT_EQUAL(one * 3, three);
}


// ── группировка разрядов ───────────────────────────────────────────────

void test_thousands_get_a_space(void) {
    // «80 689», как в эталоне: на пятизначном курсе без разделителя нули
    // приходится считать глазами.
    TEST_ASSERT_EQUAL_STRING("80 689", layout::format_decimal(80689.0f, 0).c_str());
}

void test_four_digits_stay_together(void) {
    // TVOC 5496 в эталоне идёт без пробела — группировка на четырёх цифрах
    // только дробит число.
    TEST_ASSERT_EQUAL_STRING("5496", layout::format_decimal(5496.0f, 0).c_str());
}

void test_decimals_survive_grouping(void) {
    TEST_ASSERT_EQUAL_STRING("104,80", layout::format_decimal(104.8f, 2).c_str());
}

void test_millions_get_two_spaces(void) {
    TEST_ASSERT_EQUAL_STRING("1 234 567", layout::format_decimal(1234567.0f, 0).c_str());
}

void test_negative_keeps_sign_next_to_digits(void) {
    TEST_ASSERT_EQUAL_STRING("-80 689", layout::format_decimal(-80689.0f, 0).c_str());
}

static void test_layout_row_overflow_falls_back_to_equal_split() {
    // Три M (888 px) в ряду 770 px + flex: фиксированные не влезают — ряд
    // делится поровну между четырьмя видимыми, никто не уезжает за край.
    Rect row{15, 0, 770, 200};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kM), sized(widgets::Size::kM),
                                            sized(widgets::Size::kM), sized(widgets::Size::kFlex)};
    std::vector<bool> visible = {true, true, true, true};
    Rect out[4];
    layout_row(row, 19, items, visible, out);
    const int16_t expected = (770 - 3 * 19) / 4;  // 178
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL(expected, out[i].w);
        TEST_ASSERT_TRUE(out[i].x + out[i].w <= 15 + 770);
    }
}

static void test_layout_row_fixed_that_fits_keeps_fixed_widths() {
    // Граница: M + S + flex = 296 + 202 + 2 зазора + минимум flex (120) = 656 ≤ 770 —
    // не переполнение, фиксированные сохраняют свои пиксели.
    Rect row{15, 0, 770, 200};
    std::vector<widgets::Instance> items = {sized(widgets::Size::kM), sized(widgets::Size::kS),
                                            sized(widgets::Size::kFlex)};
    std::vector<bool> visible = {true, true, true};
    Rect out[3];
    layout_row(row, 19, items, visible, out);
    TEST_ASSERT_EQUAL(296, out[0].w);
    TEST_ASSERT_EQUAL(202, out[1].w);
    TEST_ASSERT_EQUAL(770 - 296 - 202 - 38, out[2].w);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_wifi_bars_zero_is_no_signal_sentinel);
    RUN_TEST(test_wifi_bars_boundaries);

    RUN_TEST(test_segments_for_width_clamps_low_end);
    RUN_TEST(test_segments_for_width_clamps_high_end);
    RUN_TEST(test_segments_for_width_scales_with_available_width);

    RUN_TEST(test_to_civil_unix_epoch_is_thursday);
    RUN_TEST(test_to_civil_known_date_with_timezone);
    RUN_TEST(test_to_civil_negative_timezone_offset_rolls_back_a_day);

    RUN_TEST(test_has_data_null_is_false);
    RUN_TEST(test_has_data_empty_slot_is_false);
    RUN_TEST(test_has_data_present_slot_is_true);

    RUN_TEST(test_format_percent_dash_when_no_data);
    RUN_TEST(test_format_percent_fresh_has_no_mark);
    RUN_TEST(test_format_percent_stale_gets_approx_mark);
    RUN_TEST(test_format_percent_clamps_to_0_100);

    RUN_TEST(test_format_value_dash_when_no_data);
    RUN_TEST(test_format_value_fresh_decimals_and_suffix);
    RUN_TEST(test_format_value_stale_gets_approx_mark);

    RUN_TEST(test_markets_visible_by_any_of_three_slots);
    RUN_TEST(test_limits_visible_by_any_window);
    RUN_TEST(test_limits_visible_by_failure_reason_alone);
    RUN_TEST(test_limits_visible_by_reason_after_data_went_stale);
    RUN_TEST(test_limits_hidden_when_failure_has_no_reason);
    RUN_TEST(test_limits_error_row_renders_reason_text);
    RUN_TEST(test_air_visible_by_co2_or_tvoc);
    RUN_TEST(test_limits_air_visible_by_either_half);
    RUN_TEST(test_mail_visible_requires_unread_counter);
    RUN_TEST(test_today_visible_by_weather_or_first_event);
    RUN_TEST(test_metric_visible_by_its_own_slot);
    RUN_TEST(test_text_is_always_visible);
    RUN_TEST(test_find_unknown_type_is_null);
    RUN_TEST(test_widgets_registry_matches_types_table);

    RUN_TEST(test_required_slots_for_static_widget_lists_spec_slots);
    RUN_TEST(test_required_slots_for_metric_uses_instance_slot);
    RUN_TEST(test_required_slots_for_metric_without_slot_is_empty);

    RUN_TEST(test_compute_columns_all_visible_equal_width);
    RUN_TEST(test_compute_columns_missing_one_widens_the_rest_without_gap);
    RUN_TEST(test_compute_columns_none_visible_all_zero);
    RUN_TEST(test_compute_columns_single_visible_takes_full_width);

    RUN_TEST(test_fixed_first_both_visible_first_keeps_fixed_width);
    RUN_TEST(test_fixed_first_only_first_gets_full_row);
    RUN_TEST(test_fixed_first_only_second_gets_full_row);
    RUN_TEST(test_fixed_first_neither_visible_both_zero);
    RUN_TEST(test_fixed_second_both_visible_second_keeps_fixed_width);
    RUN_TEST(test_fixed_second_only_first_gets_full_row);
    RUN_TEST(test_fixed_second_only_second_gets_full_row);

    RUN_TEST(test_compute_two_rows_both_present_split_body);
    RUN_TEST(test_compute_two_rows_only_first_gets_full_body);
    RUN_TEST(test_compute_two_rows_only_second_gets_full_body);
    RUN_TEST(test_compute_two_rows_neither_present_both_zero);

    RUN_TEST(test_layout_row_fixed_m_plus_flex_matches_fixed_first);
    RUN_TEST(test_layout_row_flex_plus_fixed_s_matches_fixed_second);
    RUN_TEST(test_layout_row_single_visible_takes_full_width_regardless_of_token);
    RUN_TEST(test_layout_row_no_flex_among_visible_splits_evenly);
    RUN_TEST(test_layout_row_multiple_flex_share_remainder_equally);
    RUN_TEST(test_layout_row_none_visible_all_zero);
    RUN_TEST(test_layout_row_overflow_falls_back_to_equal_split);
    RUN_TEST(test_layout_row_fixed_that_fits_keeps_fixed_widths);

    RUN_TEST(test_decode_utf8_ascii);
    RUN_TEST(test_decode_utf8_two_byte_cyrillic);
    RUN_TEST(test_find_glyph_known_and_unknown_codepoint);
    RUN_TEST(test_text_width_is_sum_of_advances_for_monospace);

    RUN_TEST(test_thousands_get_a_space);
    RUN_TEST(test_four_digits_stay_together);
    RUN_TEST(test_decimals_survive_grouping);
    RUN_TEST(test_millions_get_two_spaces);
    RUN_TEST(test_negative_keeps_sign_next_to_digits);

    return UNITY_END();
}

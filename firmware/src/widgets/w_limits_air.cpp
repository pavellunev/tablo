// Лимиты + Воздух одной колонкой — draw_limits_and_air предшественника без
// подавления половин, ровно как рисовала верхняя правая колонка «Стола» до
// разделения на отдельные виджеты limits/air.
#include "widget.h"

#include "prims.h"

namespace widgets {

namespace {

using canvas::Canvas;
using layout::DeviceInfo;
using layout::Rect;
using slots::Store;

// Префикс limits_air_ — см. комментарий в widgets/w_markets.cpp.
const char* const kLimitsAirSlots[] = {
    "limit.claude.5h", "limit.claude.week", "limit.claude.reset", "limit.codex",
    "limit.codex.reset", "co2",              "tvoc",               nullptr,
};

bool limits_air_visible(const Store& store, const Instance&) {
    return layout::has_data(store.find("limit.claude.5h")) ||
           layout::has_data(store.find("limit.claude.week")) ||
           layout::has_data(store.find("limit.codex")) ||
           // причина отказа источника тоже показывается (prims.cpp,
           // limits_error_text) — блок не исчезает, пока есть что сказать
           prims::has_failure_reason(store, "claude.status") ||
           prims::has_failure_reason(store, "codex.status") ||
           prims::has_failure_reason(store, "home.status") || layout::has_data(store.find("co2")) ||
           layout::has_data(store.find("tvoc"));
}

void limits_air_draw(Canvas& c, const Store& store, const DeviceInfo& d, Rect r, const Instance&) {
    prims::draw_limits_and_air(c, store, d, r, /*show_limits=*/true, /*show_air=*/true);
}

}  // namespace

extern const Spec kLimitsAirSpec = {
    "limits_air", "Limits + Air", Size::kFlex, 202, kLimitsAirSlots,
    300,           &limits_air_visible, &limits_air_draw,
};

}  // namespace widgets

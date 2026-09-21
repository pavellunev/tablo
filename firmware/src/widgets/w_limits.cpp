// Лимиты AI (Claude/Codex) — половина draw_limits_and_air предшественника,
// воздух подавлен флагом show_air=false (widgets/prims.cpp), даже если
// данные воздуха есть: widgets::air — отдельный виджет.
#include "widget.h"

#include "prims.h"

namespace widgets {

namespace {

using canvas::Canvas;
using layout::DeviceInfo;
using layout::Rect;
using slots::Store;

// Префикс limits_ — см. комментарий в widgets/w_markets.cpp про то, почему
// имена внутри анонимного namespace не могут повторяться между файлами.
const char* const kLimitsSlots[] = {
    "limit.claude.5h", "limit.claude.week", "limit.claude.reset",
    "limit.codex",      "limit.codex.reset", "claude.status", "codex.status", nullptr,
};

bool limits_visible(const Store& store, const Instance&) {
    return layout::has_data(store.find("limit.claude.5h")) ||
           layout::has_data(store.find("limit.claude.week")) ||
           layout::has_data(store.find("limit.codex")) ||
           // причина отказа источника тоже показывается (prims.cpp,
           // limits_error_text) — блок не исчезает, пока есть что сказать
           prims::has_failure_reason(store, "claude.status") ||
           prims::has_failure_reason(store, "codex.status");
}

void limits_draw(Canvas& c, const Store& store, const DeviceInfo& d, Rect r, const Instance&) {
    prims::draw_limits_and_air(c, store, d, r, /*show_limits=*/true, /*show_air=*/false);
}

}  // namespace

extern const Spec kLimitsSpec = {
    "limits", "Лимиты AI", Size::kFlex, 202, kLimitsSlots, 300, &limits_visible, &limits_draw,
};

}  // namespace widgets

// Сегодня — перенос draw_today из layout.cpp без изменения пикселей.
#include "widget.h"

#include <cstdio>

#include "../assets/plexmono_14.h"
#include "../assets/plexmono_28.h"
#include "../assets/terminus_14.h"
#include "prims.h"
#include "../i18n.h"

namespace widgets {

namespace {

using canvas::Canvas;
using canvas::Color;
using fonts::draw_text;
using fonts::text_width;
using layout::DeviceInfo;
using layout::Rect;
using slots::Slot;
using slots::Store;

// Префикс today_ — см. комментарий в widgets/w_markets.cpp.
const char* const kTodaySlots[] = {
    "weather.temp", "weather.summary", "event.1.at", "event.1.title",
    "event.2.at",   "event.2.title",   "event.3.at", "event.3.title",
    nullptr,
};

const char* const kTodayStatus[] = {"weather.status", "geocode.status", nullptr};

bool today_has_data(const Store& store) {
    return layout::has_data(store.find("weather.temp")) ||
           layout::has_data(store.find("event.1.title"));
}

bool today_visible(const Store& store, const Instance&) {
    return today_has_data(store) || prims::failure_reason(store, kTodayStatus) != nullptr;
}

void today_draw(Canvas& c, const Store& store, const DeviceInfo&, Rect r, const Instance&) {
    if (!today_has_data(store)) {
        prims::draw_reason_block(c, r, i18n::tr(i18n::Str::kToday), prims::failure_reason(store, kTodayStatus));
        return;
    }
    prims::draw_eyebrow(c, r, i18n::tr(i18n::Str::kToday));
    int16_t y = static_cast<int16_t>(r.y + prims::kEyebrowTextOffset + 34);

    const Slot* temp = store.find("weather.temp");
    if (layout::has_data(temp)) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%+d°", static_cast<int>(temp->number));
        draw_text(c, fonts::PlexMono28, r.x, y, buf, Color::Black);
        int16_t tw = text_width(fonts::PlexMono28, buf);
        const Slot* summary = store.find("weather.summary");
        if (layout::has_data(summary)) {
            int16_t summary_w = static_cast<int16_t>(r.w - tw - 10);
            String label = prims::truncate_to_width(fonts::Terminus14, summary->text.c_str(), summary_w);
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
        if (!layout::has_data(title)) continue;
        if (y > r.y + r.h) break;

        if (layout::has_data(at)) {
            // 14px — .num времени события в cockpit.html (`ev.at_label`).
            draw_text(c, fonts::PlexMono14, r.x, y, at->text.c_str(), Color::Black);
        }
        String label = prims::truncate_to_width(fonts::Terminus14, title->text.c_str(),
                                                 static_cast<int16_t>(r.w - 56));
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + 56), y, label.c_str(),
                  Color::Black, 1, /*bold=*/true);
        y = static_cast<int16_t>(y + 20);
    }
}

}  // namespace

extern const Spec kTodaySpec = {
    "today", "Today", Size::kS, 202, kTodaySlots, 1800, &today_visible, &today_draw,
};

}  // namespace widgets

// Показатель — универсальный виджет на один слот (docs/widgets.md): «CO₂
// крупно» или «курс евро» без своего типа блока. Слот и подпись выбирает
// владелец на странице настройки (widgets::Instance::slot/label), поэтому
// Spec::slots здесь пуст — реестр не знает заранее, что именно покажет
// конкретный инстанс (widgets::required_slots special-кейсит "metric").
#include "widget.h"

#include "../assets/plexmono_28.h"
#include "../assets/terminus_14.h"
#include "prims.h"

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

// Префикс metric_ — см. комментарий в widgets/w_markets.cpp.
bool metric_visible(const Store& store, const Instance& instance) {
    return layout::has_data(store.find(instance.slot));
}

void metric_draw(Canvas& c, const Store& store, const DeviceInfo& d, Rect r,
                  const Instance& instance) {
    const char* label =
        instance.label.length() > 0 ? instance.label.c_str() : instance.slot.c_str();
    prims::draw_eyebrow(c, r, label);

    const Slot* s = store.find(instance.slot);
    int16_t baseline = static_cast<int16_t>(r.y + prims::kEyebrowTextOffset + 34);

    if (layout::has_data(s) && s->delta != 0.0f) {
        String delta = prims::format_delta(s->delta);
        int16_t dw = text_width(fonts::Terminus14, delta.c_str());
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - dw),
                  static_cast<int16_t>(r.y + prims::kEyebrowTextOffset), delta.c_str(),
                  Color::Black, 1, /*bold=*/true);
    }

    String value = layout::format_value(s, d.now, 0, "");
    draw_text(c, fonts::PlexMono28, r.x, baseline, value.c_str(), Color::Black);

    if (layout::has_data(s) && s->history_len >= 2) {
        int16_t spark_top = static_cast<int16_t>(baseline + 12);
        int16_t spark_h = static_cast<int16_t>(r.y + r.h - spark_top);
        if (spark_h > 10) {
            float spark[Slot::kHistoryCapacity + 1];
            uint8_t n = prims::build_spark(*s, spark, static_cast<uint8_t>(Slot::kHistoryCapacity + 1));
            prims::draw_sparkline(c, Rect{r.x, spark_top, r.w, spark_h}, spark, n);
        }
    }
}

}  // namespace

extern const Spec kMetricSpec = {
    "metric", "Metric", Size::kS, 120, nullptr, 300, &metric_visible, &metric_draw,
};

}  // namespace widgets

// Подпись — заголовок или ярлык без данных (docs/widgets.md): чем разбавить
// дашборд, если готового виджета под содержимое нет, а просто подписать
// секцию — надо.
#include "widget.h"

#include "../assets/terminus_20.h"

namespace widgets {

namespace {

using canvas::Canvas;
using canvas::Color;
using fonts::draw_text;
using layout::DeviceInfo;
using layout::Rect;
using slots::Store;

// Префикс text_ — см. комментарий в widgets/w_markets.cpp.
bool text_visible(const Store&, const Instance&) { return true; }

void text_draw(Canvas& c, const Store&, const DeviceInfo&, Rect r, const Instance& instance) {
    int16_t baseline = static_cast<int16_t>(r.y + r.h / 2 + 7);  // визуально по центру блока
    draw_text(c, fonts::Terminus20, r.x, baseline, instance.label.c_str(), Color::Black, 1,
              /*bold=*/true);
}

}  // namespace

extern const Spec kTextSpec = {
    "text", "Подпись", Size::kS, 120, nullptr, 0, &text_visible, &text_draw,
};

}  // namespace widgets

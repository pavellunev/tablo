// Рынки — курс BTC (визуальная доминанта кадра, как .num 41px в эталоне) и
// пары USD/RUB, EUR/RUB. Перенос draw_rates из layout.cpp без изменения
// пикселей (см. .claude/plans/constructor.md).
#include "widget.h"

#include "../assets/plexmono_25.h"
#include "../assets/plexmono_41.h"
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

// Имена внутри анонимного namespace — с префиксом типа виджета: тесты
// (test_layout) собирают все widgets/w_*.cpp в одну единицу трансляции через
// #include, и безымянный namespace в C++ один на всю единицу трансляции —
// одинаковые `visible`/`draw`/`kSlots` в разных файлах столкнулись бы
// переопределением при таком объединении, хотя при обычной раздельной
// компиляции (env:xiao-esp32s3, каждый .cpp — своя единица трансляции)
// коллизии бы не было.
const char* const kMarketsSlots[] = {"btc", "usd_rub", "eur_rub", nullptr};

bool markets_visible(const Store& store, const Instance&) {
    return layout::has_data(store.find("btc")) || layout::has_data(store.find("usd_rub")) ||
           layout::has_data(store.find("eur_rub"));
}

void markets_draw(Canvas& c, const Store& store, const DeviceInfo& d, Rect r, const Instance&) {
    prims::draw_eyebrow(c, r, "РЫНКИ");
    int16_t y = static_cast<int16_t>(r.y + prims::kEyebrowTextOffset);  // базовая линия эйброу

    const Slot* btc = store.find("btc");
    const Slot* usd_rub = store.find("usd_rub");
    const Slot* eur_rub = store.find("eur_rub");

    if (layout::has_data(btc)) {
        int16_t label_baseline = static_cast<int16_t>(y + 22);
        draw_text(c, fonts::Terminus14, r.x, label_baseline, "BTC / USD", Color::Black, 1, /*bold=*/true);

        String delta = prims::format_delta(btc->delta);
        int16_t dw = text_width(fonts::Terminus14, delta.c_str());
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - dw), label_baseline,
                  delta.c_str(), Color::Black, 1, /*bold=*/true);
        const char* period = "ЗА 24 Ч";
        int16_t pw = text_width(fonts::Terminus14, period);
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - pw),
                  static_cast<int16_t>(label_baseline + 18), period, Color::Black, 1, /*bold=*/true);

        int16_t number_baseline = static_cast<int16_t>(label_baseline + 41);
        String value = layout::format_value(btc, d.now, 0, "");
        draw_text(c, fonts::PlexMono41, r.x, number_baseline, value.c_str(), Color::Black);

        int16_t spark_top = static_cast<int16_t>(number_baseline + 10);
        constexpr int16_t kSparkH = 47;
        float spark[Slot::kHistoryCapacity + 1];
        uint8_t n = prims::build_spark(*btc, spark, static_cast<uint8_t>(Slot::kHistoryCapacity + 1));
        prims::draw_sparkline(c, Rect{r.x, spark_top, r.w, kSparkH}, spark, n, /*fill_below=*/true);

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
        if (!layout::has_data(p.slot)) continue;

        draw_text(c, fonts::Terminus14, r.x, y, p.label, Color::Black, 1, /*bold=*/true);

        String delta = prims::format_delta(p.slot->delta);
        int16_t dw = text_width(fonts::Terminus14, delta.c_str());
        draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - dw), y, delta.c_str(),
                  Color::Black, 1, /*bold=*/true);

        String val = layout::format_value(p.slot, d.now, 2, "");
        int16_t vw = text_width(fonts::PlexMono25, val.c_str());
        draw_text(c, fonts::PlexMono25, static_cast<int16_t>(r.x + r.w - dw - vw - 10), y,
                  val.c_str(), Color::Black);
        y = static_cast<int16_t>(y + 36);
    }
}

}  // namespace

extern const Spec kMarketsSpec = {
    "markets", "Рынки", Size::kM, 202, kMarketsSlots, 300, &markets_visible, &markets_draw,
};

}  // namespace widgets

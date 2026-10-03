// Почта — перенос draw_mail из layout.cpp без изменения пикселей.
#include "widget.h"

#include <cstdio>

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

// Префикс mail_ — см. комментарий в widgets/w_markets.cpp.
//
// mail.unread — счётчик; демонстрации ради явно не разворачиваем все
// mail.N.{from,subject,time} (N=1..4) в этот список — imap-коннектор отдаёт
// их все вместе, unread уже достаточно, чтобы widgets::compute_demand понял,
// что почта нужна (connectors::provides матчит по префиксу "mail.").
const char* const kMailSlots[] = {"mail.unread", nullptr};

const char* const kMailStatus[] = {"mail.status", nullptr};

bool mail_visible(const Store& store, const Instance&) {
    return layout::has_data(store.find("mail.unread")) ||
           prims::failure_reason(store, kMailStatus) != nullptr;
}

// Правый край колонки времени — с отступом от разделителя Почта|Сегодня:
// в эталоне между текстом времени и вертикальной линией 20 px, у нас без
// отступа выходило 3 — дата на живой панели упиралась в линию (фото
// владельца, 2026-09-22). Замер: tools/compare_frame и PIL по
// reference/cockpit-reference.png (x=547 против линии x=567).
constexpr int16_t kTimeRightInset = 18;

void mail_draw(Canvas& c, const Store& store, const DeviceInfo&, Rect r, const Instance&) {
    int16_t header_baseline = static_cast<int16_t>(r.y + 12);
    int16_t plate_w = prims::draw_inverse_label(c, r.x, header_baseline, i18n::tr(i18n::Str::kMail));

    const char* reason = layout::has_data(store.find("mail.unread"))
                             ? nullptr
                             : prims::failure_reason(store, kMailStatus);
    if (reason != nullptr) {
        // Данных нет, причина есть — строка причины на месте первого письма.
        String fitted = prims::truncate_to_width(fonts::Terminus14, reason, r.w);
        draw_text(c, fonts::Terminus14, r.x, static_cast<int16_t>(r.y + 45), fitted.c_str(),
                  Color::Black);
        return;
    }

    const Slot* unread = store.find("mail.unread");
    char summary[32];
    int count = layout::has_data(unread) ? static_cast<int>(unread->number) : 0;
    std::snprintf(summary, sizeof(summary), i18n::tr(i18n::Str::kUnreadFmt), count);
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
        if (!layout::has_data(from) && !layout::has_data(subject)) continue;
        if (y > r.y + r.h) break;

        // Точка-маркер перед отправителем — как .dot в cockpit.html; здесь
        // без чтения/непрочитанного состояния на слот (в Store такого пока
        // нет), просто отметка «это письмо из стопки».
        constexpr int16_t kDotSize = 5;
        c.fill_rect(r.x, static_cast<int16_t>(y - kDotSize), kDotSize, kDotSize, Color::Black);
        int16_t text_x = static_cast<int16_t>(r.x + kDotSize + 8);

        if (layout::has_data(from)) {
            String label = prims::truncate_to_width(fonts::Terminus14, from->text.c_str(),
                                                     static_cast<int16_t>(kSubjectX - kDotSize - 16));
            draw_text(c, fonts::Terminus14, text_x, y, label.c_str(), Color::Black, 1, /*bold=*/true);
        }
        // Колонка времени: «14:32» укладывается в kTimeReserve, «yesterday» —
        // нет, поэтому резерв берём от реальной ширины текста, иначе тема
        // письма налезает на него.
        const Slot* at = store.find(String(time_id));
        int16_t tw = layout::has_data(at) ? text_width(fonts::Terminus14, at->text.c_str()) : 0;
        int16_t time_reserve = static_cast<int16_t>(tw + 12 > kTimeReserve ? tw + 12 : kTimeReserve);
        if (layout::has_data(subject)) {
            int16_t subject_w = static_cast<int16_t>(r.w - kSubjectX - time_reserve - kTimeRightInset);
            String label =
                prims::truncate_to_width(fonts::Terminus14, subject->text.c_str(), subject_w);
            draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + kSubjectX), y,
                      label.c_str(), Color::Black, 1, /*bold=*/true);
        }
        if (layout::has_data(at)) {
            draw_text(c, fonts::Terminus14, static_cast<int16_t>(r.x + r.w - kTimeRightInset - tw),
                      y, at->text.c_str(), Color::Black, 1, /*bold=*/true);
        }
        c.hline(r.x, static_cast<int16_t>(y + kSeparatorGap), r.w, Color::Black);
        y = static_cast<int16_t>(y + kRowHeight);
    }
}

}  // namespace

extern const Spec kMailSpec = {
    "mail", "Mail", Size::kFlex, 202, kMailSlots, 900, &mail_visible, &mail_draw,
};

}  // namespace widgets

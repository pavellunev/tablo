#include "types.h"

#include <cstring>

namespace widgets {

int16_t size_px(Size size) {
    switch (size) {
        case Size::kS:
            return 202;  // «Сегодня» в эталоне
        case Size::kM:
            return 296;  // «Рынки» в эталоне
        case Size::kFlex:
        default:
            return 0;
    }
}

const char* size_to_string(Size size) {
    switch (size) {
        case Size::kS:
            return "S";
        case Size::kM:
            return "M";
        case Size::kFlex:
        default:
            return "flex";
    }
}

bool size_from_string(const char* text, Size& size) {
    if (text == nullptr) return false;
    if (std::strcmp(text, "S") == 0) { size = Size::kS; return true; }
    if (std::strcmp(text, "M") == 0) { size = Size::kM; return true; }
    if (std::strcmp(text, "flex") == 0) { size = Size::kFlex; return true; }
    return false;
}

namespace {

// Базовый набор — перенос существующего кода отрисовки (docs/widgets.md).
// min_width — размеры уже этого страница не предлагает (недостаточно места
// даже на минимальное содержимое виджета).
constexpr TypeInfo kTypes[] = {
    {"markets", Size::kM, 202},
    {"limits", Size::kFlex, 202},
    {"air", Size::kFlex, 202},
    {"limits_air", Size::kFlex, 202},
    {"mail", Size::kFlex, 202},
    {"today", Size::kS, 202},
    {"metric", Size::kS, 120},
    {"text", Size::kS, 120},
};
constexpr size_t kTypesCount = sizeof(kTypes) / sizeof(kTypes[0]);

// Таблица указателей — то же, что widgets::all() в widget.h (registry.cpp),
// параллельная лёгкая версия без Spec.
const TypeInfo* kTypePtrs[kTypesCount] = {
    &kTypes[0], &kTypes[1], &kTypes[2], &kTypes[3],
    &kTypes[4], &kTypes[5], &kTypes[6], &kTypes[7],
};

}  // namespace

const TypeInfo* find_type(const char* type) {
    if (type == nullptr) return nullptr;
    for (const TypeInfo& t : kTypes) {
        if (std::strcmp(t.type, type) == 0) return &t;
    }
    return nullptr;
}

const TypeInfo* const* all_types(size_t* count) {
    if (count) *count = kTypesCount;
    return kTypePtrs;
}

}  // namespace widgets

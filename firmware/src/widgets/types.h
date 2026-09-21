// Лёгкое описание типов виджетов — токены размера и то, что нужно
// config.cpp для проверки `type`/`size` при разборе дашборда. Никакой
// зависимости от Canvas/раскладки (widgets/widget.h) — иначе test_config
// (включает config.cpp исходником) тянул бы за собой весь код отрисовки
// восьми виджетов ради проверки одной строки.
#pragma once

#include <Arduino.h>

#include <cstdint>

namespace widgets {

// Три стандартных токена ширины (docs/widgets.md): S — «Сегодня» в эталоне
// (202px), M — «Рынки» (296px), flex — делит остаток ряда поровну между
// такими же flex-виджетами, сам по себе пикселей не имеет.
enum class Size : uint8_t { kS, kM, kFlex };

// kFlex -> 0: раскладка ряда (layout::layout_row) вычисляет фактическую
// ширину сама, здесь отдавать нечего.
int16_t size_px(Size size);

const char* size_to_string(Size size);
// false — незнакомая строка; size не трогается, вызывающий код (config.cpp)
// сам решает, что подставить (default_size спеки).
bool size_from_string(const char* text, Size& size);

// То, что лежит в дашборде (config::Dashboard::rows) и уходит на страницу
// настройки как есть — config.cpp, to_json/from_json.
struct Instance {
    String type;
    Size size = Size::kFlex;
    bool divider = false;
    String slot;   // metric: какой слот показывать
    String label;  // metric/text: подпись
};

// Лёгкая запись реестра типов — без указателей на функции отрисовки/
// видимости (те живут в widgets::Spec, widgets/widget.h, вместе с Canvas).
// type/default_size/min_width дублируют одноимённые поля Spec того же типа —
// согласованность держит тест (test_layout,
// test_widgets_registry_matches_types_table).
struct TypeInfo {
    const char* type;
    Size default_size;
    int16_t min_width;
};

const TypeInfo* find_type(const char* type);  // nullptr — неизвестный тип
const TypeInfo* const* all_types(size_t* count);

}  // namespace widgets

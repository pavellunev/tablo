// Протокол виджета (docs/widgets.md): спецификация → видимость → отрисовка
// → регистрация. Добавить виджет = один файл widgets/w_<type>.cpp со Spec +
// одна строка в таблице реестра (widgets/registry.cpp) — страница настройки
// подхватывает его сама через GET /api/widgets, ничего в portal.cpp/
// index.html руками не трогая.
#pragma once

#include <cstdint>
#include <vector>

#include <Arduino.h>

#include "../layout.h"
#include "../slots.h"
#include "types.h"

namespace widgets {

struct Spec {
    const char* type;   // ключ в реестре, "markets"
    const char* title;  // «Рынки» — для палитры на странице настройки
    Size default_size;
    int16_t min_width;             // размеры уже этого страница не предлагает
    const char* const* slots;      // nullptr-terminated; metric — слоты из Instance::slot
    uint32_t refresh_seconds;      // как часто виджету нужны свежие данные (widgets::compute_demand)
    bool (*visible)(const slots::Store&, const Instance&);
    void (*draw)(canvas::Canvas&, const slots::Store&, const layout::DeviceInfo&, layout::Rect,
                 const Instance&);
};

const Spec* find(const char* type);            // nullptr — неизвестный тип
const Spec* const* all(size_t* count);          // реестр для /api/widgets и палитры

// Слоты, которые нужны инстансу: spec->slots для большинства виджетов, а для
// metric — единственный слот из Instance::slot (спека metric его не несёт,
// он выбирается на странице настройки). Используется расчётом потребностей
// (widgets::compute_demand) и страницей настройки.
void required_slots(const Instance& instance, std::vector<String>& out);

}  // namespace widgets

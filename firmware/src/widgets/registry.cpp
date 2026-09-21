// Реестр виджетов — таблица указателей на Spec, каждый определён в своём
// widgets/w_<type>.cpp. Добавить виджет = один такой файл + одна строка
// здесь (docs/widgets.md).
#include "widget.h"

#include <cstring>

namespace widgets {

// Определены в widgets/w_<type>.cpp — по одному Spec на файл, единственная
// точка связи между реестром и конкретным виджетом.
extern const Spec kMarketsSpec;
extern const Spec kLimitsSpec;
extern const Spec kAirSpec;
extern const Spec kLimitsAirSpec;
extern const Spec kMailSpec;
extern const Spec kTodaySpec;
extern const Spec kMetricSpec;
extern const Spec kTextSpec;

namespace {

// Не constexpr: инициализаторы — адреса extern-объектов, определённых в
// других файлах (w_<type>.cpp); порядок статической инициализации между
// единицами трансляции для этого не важен — берём только адрес, не значение.
const Spec* const kRegistry[] = {
    &kMarketsSpec, &kLimitsSpec, &kAirSpec,   &kLimitsAirSpec,
    &kMailSpec,    &kTodaySpec,  &kMetricSpec, &kTextSpec,
};
const size_t kRegistryCount = sizeof(kRegistry) / sizeof(kRegistry[0]);

}  // namespace

const Spec* find(const char* type) {
    if (type == nullptr) return nullptr;
    for (const Spec* s : kRegistry) {
        if (std::strcmp(s->type, type) == 0) return s;
    }
    return nullptr;
}

const Spec* const* all(size_t* count) {
    if (count) *count = kRegistryCount;
    return kRegistry;
}

void required_slots(const Instance& instance, std::vector<String>& out) {
    const Spec* spec = find(instance.type.c_str());
    if (spec == nullptr) return;

    // metric не несёт статического списка слотов в Spec — какой слот
    // показывать, владелец выбирает при добавлении виджета на дашборд
    // (Instance::slot), а не при регистрации типа.
    if (std::strcmp(spec->type, "metric") == 0) {
        if (instance.slot.length() > 0) out.push_back(instance.slot);
        return;
    }

    for (const char* const* p = spec->slots; p != nullptr && *p != nullptr; ++p) {
        out.push_back(String(*p));
    }
}

}  // namespace widgets

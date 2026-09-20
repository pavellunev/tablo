// Реализация Store: значения слотов живут в ОЗУ и переживают только текущий
// цикл опроса — смысла хранить их дольше нет, источники опрашиваются заново.
#include "slots.h"

namespace slots {

void Store::put(const String& id, const Slot& slot, const String& connector_id) {
    items_[id] = slot;
    // Пустой connector_id — вызов без владельца: слот просто не участвует в
    // mark_failed, но это не ошибка, а осознанный вызов put() «как раньше».
    if (connector_id.length() > 0) {
        owner_[id] = connector_id;
    }
}

const Slot* Store::find(const String& id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

bool Store::has_fresh(const String& id, uint32_t now) const {
    const Slot* slot = find(id);
    return slot != nullptr && slot->fresh(now);
}

void Store::mark_failed(const String& connector_id) {
    // Проходим по owner_, а не по items_: слот без зарегистрированного
    // владельца не должен погаснуть от чужого mark_failed.
    for (const auto& entry : owner_) {
        if (entry.second != connector_id) continue;
        auto it = items_.find(entry.first);
        if (it != items_.end()) {
            it->second.ok = false;
        }
    }
}

}  // namespace slots

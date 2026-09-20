// Реализация Store: значения слотов живут в ОЗУ и переживают только текущий
// цикл опроса — смысла хранить их дольше нет, источники опрашиваются заново.
#include "slots.h"

namespace slots {

void Store::put(const String& id, const Slot& slot_in, const String& connector_id) {
    Slot slot = slot_in;

    auto it = items_.find(id);
    if (it != items_.end() && it->second.ok) {
        // Копим историю прошлых значений автоматически: тот, кто уже лежал в
        // сторе перед перезаписью, был живым замером, и его число уходит в
        // history нового слота. Если предыдущая запись не ok (например, после
        // mark_failed) — историю не двигаем: неизвестное состояние источника
        // не должно попасть в спарклайн как точка данных.
        const Slot& prev = it->second;
        uint8_t len = prev.history_len;
        if (len < Slot::kHistoryCapacity) {
            for (uint8_t i = 0; i < len; ++i) slot.history[i] = prev.history[i];
            slot.history[len] = prev.number;
            slot.history_len = static_cast<uint8_t>(len + 1);
        } else {
            // Буфер полон — старейшая точка выпадает, остальные сдвигаются.
            for (uint8_t i = 1; i < Slot::kHistoryCapacity; ++i) {
                slot.history[i - 1] = prev.history[i];
            }
            slot.history[Slot::kHistoryCapacity - 1] = prev.number;
            slot.history_len = Slot::kHistoryCapacity;
        }
    }

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

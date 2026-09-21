// Реализация Store: значения слотов живут в ОЗУ и переживают только текущий
// цикл опроса — смысла хранить их дольше нет, источники опрашиваются заново.
#include "slots.h"

#include <string>

namespace slots {

void Store::put(const String& id, const Slot& slot_in, const String& connector_id) {
    Slot slot = slot_in;

    // История больше не копится сама (см. комментарий у Slot::history в
    // slots.h) — обычный put() кормят одной свежей точкой текущего значения,
    // у неё своей истории нет (history_len == 0). Если история уже лежала в
    // слоте (put_history() записала её раньше или позже — порядок вызовов
    // внутри одного опроса не важен), put() её не стирает.
    if (slot.history_len == 0) {
        auto it = items_.find(id);
        if (it != items_.end()) {
            slot.history_len = it->second.history_len;
            for (uint8_t i = 0; i < slot.history_len; ++i) slot.history[i] = it->second.history[i];
        }
    }

    items_[id] = slot;
    // Пустой connector_id — вызов без владельца: слот просто не участвует в
    // mark_failed, но это не ошибка, а осознанный вызов put() «как раньше».
    if (connector_id.length() > 0) {
        owner_[id] = connector_id;
        // Коннектор ожил — заглушка причины отказа не должна висеть в статусе
        // рядом с живым значением.
        std::string status_id(connector_id.c_str());
        status_id += ".status";
        items_.erase(String(status_id.c_str()));
        owner_.erase(String(status_id.c_str()));
    }
}

void Store::put_history(const String& id, const float* values, uint8_t count,
                        const String& connector_id) {
    // operator[] создаёт пустой слот, если его ещё нет — значение (text/
    // number) довершит put(), который может прийти как до, так и после этого
    // вызова в рамках одного опроса коннектора.
    Slot& slot = items_[id];
    uint8_t n = count > Slot::kHistoryCapacity ? Slot::kHistoryCapacity : count;
    for (uint8_t i = 0; i < n; ++i) slot.history[i] = values[i];
    slot.history_len = n;
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

void Store::mark_failed(const String& connector_id, const String& reason) {
    // Проходим по owner_, а не по items_: слот без зарегистрированного
    // владельца не должен погаснуть от чужого mark_failed.
    bool touched = false;
    for (const auto& entry : owner_) {
        if (entry.second != connector_id) continue;
        auto it = items_.find(entry.first);
        if (it != items_.end()) {
            it->second.ok = false;
            it->second.error = reason;
            touched = true;
        }
    }
    // Коннектор, который ни разу ничего не положил (холодный старт с 401 или
    // 403), иначе исчезал бы из /api/status вместе с причиной — а ради неё
    // причина и заведена («недоступен из этой страны — нужен VPN»). Слот
    // `<коннектор>.status` раскладка не рисует, зато статус его показывает.
    if (!touched && reason.length() > 0) {
        Slot placeholder;
        placeholder.ok = false;
        placeholder.error = reason;
        // Без String::operator+ — хостовый шим для тестов его не имеет.
        std::string sid(connector_id.c_str());
        sid += ".status";
        const String id(sid.c_str());
        items_[id] = placeholder;
        owner_[id] = connector_id;
    }
}

}  // namespace slots

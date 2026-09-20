// Слоты — именованные ячейки данных между коннекторами и экраном.
//
// Возраст хранится у каждого слота свой, а не одной общей «датой обновления»:
// курс может прийти минуту назад, а почта — час, и экран обязан показывать
// это раздельно. Иначе один отвалившийся источник делает лжецом весь кадр.
#pragma once

#include <Arduino.h>

#include <map>

namespace slots {

struct Slot {
    String text;       // готовое к печати значение
    float number = 0;  // оно же числом — для шкал и спарклайнов
    float delta = 0;   // изменение, если источник его даёт
    uint32_t at = 0;   // когда получено, unix-время
    uint32_t ttl = 0;  // сколько считается свежим, секунды
    bool ok = false;   // источник ответил без ошибки

    // Свежесть: пустой слот и нулевой — разные вещи. `ok == false` рисуется
    // прочерком, потому что ноль вместо «нет данных» читается как «свободно»
    // и уже однажды вводил в заблуждение на лимитах.
    bool fresh(uint32_t now) const { return ok && at > 0 && now - at < ttl; }
    bool stale(uint32_t now) const { return ok && at > 0 && now - at >= ttl; }
    bool empty() const { return !ok || at == 0; }
};

class Store {
   public:
    // connector_id — необязательный: без него слот не привязывается ни к
    // какому владельцу, и mark_failed его не тронет. Параметр — единственный
    // способ наполнить owner_ ниже: Slot владельца не несёт, а put() был
    // единственной публичной точкой записи.
    void put(const String& id, const Slot&, const String& connector_id = String());
    const Slot* find(const String& id) const;
    bool has_fresh(const String& id, uint32_t now) const;

    // Пометить все слоты коннектора недоступными — источник отвалился.
    // Значения не удаляем: экран покажет их со знаком устаревания, это
    // честнее пустого места.
    void mark_failed(const String& connector_id);

    size_t size() const { return items_.size(); }

    // Перебор для раскладки: ей нужно пройти по всему, что есть, а не
    // спрашивать заранее известные имена.
    const std::map<String, Slot>& all() const { return items_; }

   private:
    std::map<String, Slot> items_;
    std::map<String, String> owner_;  // слот → коннектор, для mark_failed
};

}  // namespace slots

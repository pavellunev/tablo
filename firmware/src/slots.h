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
    String error;      // причина последнего отказа коннектора, для /api/status

    // История прошлых значений — для спарклайна на экране (курс BTC, CO2,
    // TVOC). Раньше Store::put() копил её сам из редких опросов (раз в
    // несколько минут) — за 40 минут реальных данных подпись под графиком
    // честно обещала «за 24 ч», и это было ложью. Теперь put() эту историю
    // не трогает вовсе: она приходит только явно, от коннектора, у которого
    // есть настоящие исторические данные источника (часовые свечи Binance,
    // история Home Assistant) — через Store::put_history(). Нет источника —
    // нет истории, history_len остаётся 0, и график просто не рисуется
    // (build_spark/draw_sparkline в layout.cpp).
    static constexpr uint8_t kHistoryCapacity = 24;
    float history[kHistoryCapacity] = {};
    uint8_t history_len = 0;

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

    // Кладёт готовый массив точек истории отдельно от текущего значения —
    // коннектор, у которого есть настоящие исторические данные (klines,
    // /api/history/period), не обязан знать, существует ли уже слот с
    // текущим значением: put_history() создаёт слот сама, если его ещё нет,
    // put() значения ниже (в любом порядке относительно этого вызова)
    // сохранит уже записанную историю, если сама её не несёт (см. put()).
    void put_history(const String& id, const float* values, uint8_t count,
                      const String& connector_id = String());

    const Slot* find(const String& id) const;
    bool has_fresh(const String& id, uint32_t now) const;

    // Пометить все слоты коннектора недоступными — источник отвалился.
    // Значения не удаляем: экран покажет их со знаком устаревания, это
    // честнее пустого места. reason — по возможности честная причина отказа
    // (например, «недоступен из этой страны — нужен VPN» для 403 у
    // Anthropic/Codex, docs/decisions.md п.8а) — оседает в Slot::error для
    // /api/status; пусто — причина не классифицирована точнее «не ответил».
    void mark_failed(const String& connector_id, const String& reason = String());

    size_t size() const { return items_.size(); }

    // Перебор для раскладки: ей нужно пройти по всему, что есть, а не
    // спрашивать заранее известные имена.
    const std::map<String, Slot>& all() const { return items_; }

   private:
    std::map<String, Slot> items_;
    std::map<String, String> owner_;  // слот → коннектор, для mark_failed
};

}  // namespace slots

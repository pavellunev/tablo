// Потребности виджетов — какие коннекторы вообще нужны опрашивать и как
// часто, посчитанные по ВСЕМ трём дашбордам (не только активному:
// переключение кнопкой должно показать данные сразу, не ждать первого опроса
// после смены экрана). connectors::poll_due читает Demand вместо того, чтобы
// опрашивать всё подряд по одному лишь Connector::interval (docs/widgets.md).
#pragma once

#include <Arduino.h>

#include <map>
#include <string>

#include "../config.h"

namespace widgets {

class Demand {
   public:
    bool needs(const String& connector_id) const;
    // 0 — ни один виджет не ограничивал refresh явно (poll_due берёт
    // Connector::interval как есть); иначе — минимум среди всех виджетов,
    // которым нужен этот коннектор (виджет просит чаще, источник в poll_due
    // всё равно не даст опрашивать реже своего interval).
    uint32_t refresh_seconds(const String& connector_id) const;

    // Отмечает коннектор нужным с данным refresh — берёт минимум с уже
    // отмеченным значением, если коннектора попросил другой виджет раньше.
    void mark_needed(const String& connector_id, uint32_t refresh_seconds);

   private:
    std::map<std::string, uint32_t> refresh_by_connector_;
};

// По всем виджетам всех трёх дашбордов: widgets::required_slots() каждого
// инстанса -> connectors::provides() каждого коннектора -> Demand.
Demand compute_demand(const config::Settings& settings);

}  // namespace widgets

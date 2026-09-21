#include "demand.h"

#include <vector>

#include "../connectors.h"
#include "widget.h"

namespace widgets {

bool Demand::needs(const String& connector_id) const {
    return refresh_by_connector_.find(std::string(connector_id.c_str())) !=
           refresh_by_connector_.end();
}

uint32_t Demand::refresh_seconds(const String& connector_id) const {
    auto it = refresh_by_connector_.find(std::string(connector_id.c_str()));
    return it == refresh_by_connector_.end() ? 0 : it->second;
}

void Demand::mark_needed(const String& connector_id, uint32_t refresh_seconds) {
    const std::string key(connector_id.c_str());
    auto it = refresh_by_connector_.find(key);
    if (it == refresh_by_connector_.end() || refresh_seconds < it->second) {
        refresh_by_connector_[key] = refresh_seconds;
    }
}

namespace {

void collect_instance(const Instance& instance, const config::Settings& settings,
                       Demand& demand) {
    const Spec* spec = find(instance.type.c_str());
    if (spec == nullptr) return;  // неизвестный/отброшенный тип — спрашивать нечего

    std::vector<String> slots;
    required_slots(instance, slots);
    for (const String& slot : slots) {
        for (const config::Connector& c : settings.connectors) {
            if (connectors::provides(c, slot)) {
                demand.mark_needed(c.id, spec->refresh_seconds);
            }
        }
    }
}

}  // namespace

Demand compute_demand(const config::Settings& settings) {
    Demand demand;
    for (const config::Dashboard& db : settings.dashboards) {
        for (const std::vector<Instance>& row : db.rows) {
            for (const Instance& instance : row) {
                collect_instance(instance, settings, demand);
            }
        }
    }
    return demand;
}

}  // namespace widgets

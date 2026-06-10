#include "chat/WaypointNames.h"
#include "chat/WaypointTable.h"
#include <algorithm>

namespace PieUI {
namespace WaypointNames {

static const char* TypeLabel(uint8_t t) {
    switch (t) {
        case 0:  return "Waypoint";
        case 2:  return "Vista";
        default: return "Point of Interest";  // landmark / unlock / other
    }
}

bool Get(uint32_t poiId, std::string& name, std::string& mapName,
         const char** typeLabel) {
    if (poiId == 0) return false;
    using namespace WaypointData;
    const PoiEntry* end = kPois + kPoiCount;
    const PoiEntry* it = std::lower_bound(
        kPois, end, poiId,
        [](const PoiEntry& e, uint32_t id) { return e.id < id; });
    if (it == end || it->id != poiId) return false;
    name = it->name;
    mapName = kMaps[it->mapIdx];
    if (typeLabel) *typeLabel = TypeLabel(it->type);
    return true;
}

} // namespace WaypointNames
} // namespace PieUI

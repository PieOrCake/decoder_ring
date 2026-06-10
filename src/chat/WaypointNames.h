#pragma once
#include <cstdint>
#include <string>

namespace PieUI {

// Resolves a chat-link waypoint/PoI id to its name + the map it sits on, so chat
// waypoint links show the real name instead of a generic "[Waypoint]" label.
//
// Backed by a compiled-in table (src/chat/WaypointTable.h, generated offline by
// tools/gen_waypoints.py from the continents API) — available instantly at load,
// no fetch, no async, no disk cache. Regenerate the header after content patches.
namespace WaypointNames {
    // True + fills name/mapName when the id is known. If typeLabel is non-null it
    // receives a static label for the PoI kind ("Waypoint" / "Point of Interest" /
    // "Vista") so the tooltip doesn't mislabel a landmark as a waypoint.
    bool Get(uint32_t poiId, std::string& name, std::string& mapName,
             const char** typeLabel = nullptr);
}
} // namespace PieUI

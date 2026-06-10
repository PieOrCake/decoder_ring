#pragma once
#include "DecoderRingApi.h"
#include <string>

namespace Decoder {
// Resolve the OFFLINE link types (no network): build/AE2 spec label and
// waypoint/POI name. Fills `out` and returns true on success (out.status set to
// DR_Resolved). Returns false for non-offline types or undecodable input.
//   id:       the POI id for LINK_MAP (sufficient on its own); ignored for builds.
//   chatCode: the full "[&...]" chat link. REQUIRED for LINK_BUILD; for LINK_MAP it
//             is only consulted when id == 0 (the poi id is extracted from it then).
bool ResolveOffline(uint8_t linkType, uint32_t id, const std::string& chatCode, DecoderRecord& out);
}

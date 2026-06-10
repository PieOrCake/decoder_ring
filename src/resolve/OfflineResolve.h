#pragma once
#include "DecoderRingApi.h"
#include <string>

namespace Decoder {
// Resolve the OFFLINE link types (no network): build/AE2 spec label and
// waypoint/POI name. Fills `out` and returns true on success (out.status set to
// DR_Resolved). Returns false for non-offline types or undecodable input.
//   chatCode: the full "[&...]" chat link (for LINK_BUILD / LINK_MAP).
bool ResolveOffline(uint8_t linkType, const std::string& chatCode, DecoderRecord& out);
}

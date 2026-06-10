#include "resolve/OfflineResolve.h"
#include "resolve/RecordFill.h"
#include "chat/ChatLinks.h"
#include "chat/SpecData.h"
#include "chat/WaypointNames.h"
#include <string>

namespace Decoder {
using namespace PieUI;
using namespace PieUI::ChatLinks;

namespace {
// "Mirage Build" / "Mesmer Build" from a decoded build link's profession + 3rd-slot spec.
std::string BuildLabel(const DecodedBuildLink& b) {
    const char* spec = SpecData::GetEliteSpecName(b.specs[2].spec_id);
    if (b.specs[2].spec_id != 0 && spec) return std::string(spec) + " Build";
    const char* prof = SpecData::GetProfessionName(b.profession);
    if (prof && prof[0]) return std::string(prof) + " Build";
    return "Build";
}
}

bool ResolveOffline(uint8_t linkType, const std::string& chatCode, DecoderRecord& out) {
    if (linkType == LINK_BUILD) {
        DecodedBuildLink b{};
        if (!DecodeBuild(chatCode, b)) return false;
        InitRecord(out, linkType, 0, DR_Resolved);
        CopyField(out.name, BuildLabel(b));
        return true;
    }
    if (linkType == LINK_MAP) {
        auto segs = SegmentLine(chatCode);
        uint32_t poiId = 0;
        for (auto& s : segs) if (s.kind == SegmentKind::Link && s.linkType == LINK_MAP) { poiId = s.primaryId; break; }
        if (poiId == 0) return false;
        std::string name, mapName; const char* typeLabel = nullptr;
        if (!WaypointNames::Get(poiId, name, mapName, &typeLabel)) return false;
        InitRecord(out, linkType, poiId, DR_Resolved);
        CopyField(out.name, name);
        CopyField(out.mapName, mapName);
        out.poiType = (typeLabel && std::string(typeLabel) == "Point of Interest") ? DP_PointOfInterest
                    : (typeLabel && std::string(typeLabel) == "Vista") ? DP_Vista : DP_Waypoint;
        return true;
    }
    return false;
}
}

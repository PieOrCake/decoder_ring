// Isolation proof: this TU + ChatLinks.cpp are the ONLY sources linked.
// If it builds and links, the vendored codec has no hidden dependency.
#include "chat/ChatLinks.h"
#include <cstdio>

int main() {
    using namespace PieUI::ChatLinks;
    std::string skin = EncodeSkin(8585);
    bool ok = DetectType(skin) == LINK_SKIN;
    auto segs = SegmentLine("hello " + skin);
    ok = ok && !segs.empty();
    std::printf("chatlinks isolation: %s (%zu segments)\n", ok ? "OK" : "FAIL", segs.size());
    return ok ? 0 : 1;
}

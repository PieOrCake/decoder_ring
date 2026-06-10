#include "resolve/RecordFill.h"
#include <cstring>

namespace Decoder {
void InitRecord(DecoderRecord& r, uint8_t linkType, uint32_t id, DecoderStatus status) {
    std::memset(&r, 0, sizeof(r));
    r.schemaVersion = (uint16_t)DECODER_RING_API_VERSION;
    r.linkType = linkType;
    r.id = id;
    r.status = (uint8_t)status;
}
}

#pragma once
#include "DecoderRingApi.h"
#include <string>

namespace Decoder {
// Copy src into a fixed char buffer, always NUL-terminated, truncating safely.
template <size_t N>
inline void CopyField(char (&dst)[N], const std::string& src) {
    size_t n = src.size() < (N - 1) ? src.size() : (N - 1);
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = '\0';
}
// Zero a record and stamp the common header (version, key, status).
void InitRecord(DecoderRecord& r, uint8_t linkType, uint32_t id, DecoderStatus status);
}

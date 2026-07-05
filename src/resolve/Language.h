#pragma once
#include <string>

namespace Decoder {
// Map a Nexus language identifier (2-letter, e.g. "en"/"de"/"fr"/"es"/"cz"/"cn")
// to the /v2 API language code we support. Unsupported / null / empty -> "en".
std::string MapNexusToApi(const char* nexusCode);
}

#pragma once
#include "nlohmann/json.hpp"
#include <string>

namespace Decoder {
// Pre-format one GW2 /v2 API fact into a display line. Skills and traits share the
// fact schema, so both resolvers use this single formatter. "" = nothing renderable.
std::string FormatApiFact(const nlohmann::json& f);
}

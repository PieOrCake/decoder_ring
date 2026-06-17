#pragma once
#include "DecoderRingApi.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct SkinMeta { std::string name, icon; };
struct SkinTraits {
    using Meta = SkinMeta;
    static std::string Url(uint32_t id) { return "https://api.guildwars2.com/v2/skins/" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& out);
    static std::string FallbackUrl(uint32_t) { return ""; }                  // no fallback source
    static bool ParseFallback(const std::vector<char>&, Meta&) { return false; }
    static const char* FileName() { return "skininfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m) { return nlohmann::json{ {"n",m.name},{"ic",m.icon} }; }
    static void FromJson(const nlohmann::json& j, Meta& m) {
        if (j.is_object()) { if (j.contains("n")) m.name=j["n"].get<std::string>();
                             if (j.contains("ic")) m.icon=j["ic"].get<std::string>(); }
    }
};
}

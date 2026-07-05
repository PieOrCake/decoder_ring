#pragma once
#include "DecoderRingApi.h"
#include "resolve/Http.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct SkinMeta { std::string name, icon; };
struct SkinTraits {
    using Meta = SkinMeta;
    static std::string Url(uint32_t id, const std::string& lang = "en") { return "https://api.guildwars2.com/v2/skins/" + std::to_string(id) + "?lang=" + lang; }
    static bool Parse(const std::vector<char>& body, Meta& out, const std::string& lang = "en");
    static std::string FallbackUrl(uint32_t, const std::string& = "en") { return ""; }                  // no fallback source
    static bool ParseFallback(const std::vector<char>&, Meta&, const std::string& = "en") { return false; }
    static bool ResolveDeps(Meta&, const HttpFetch&, const std::string& = "en") { return true; }        // no dependent fetches
    static std::string EnrichUrl(uint32_t, const Meta&, const std::string& = "en") { return ""; }       // no enrichment
    static bool ParseEnrich(const std::vector<char>&, Meta&, const std::string& = "en") { return false; }
    static const char* FileName() { return "skininfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m) { return nlohmann::json{ {"n",m.name},{"ic",m.icon} }; }
    static void FromJson(const nlohmann::json& j, Meta& m) {
        if (j.is_object()) { if (j.contains("n")) m.name=j["n"].get<std::string>();
                             if (j.contains("ic")) m.icon=j["ic"].get<std::string>(); }
    }
};
}

#pragma once
#include "DecoderRingApi.h"
#include "resolve/Http.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct TraitFactM { std::string icon, text; };
struct TraitMeta  { std::string name, icon, description; std::vector<TraitFactM> facts; };
struct TraitTraits {
    using Meta = TraitMeta;
    static std::string Url(uint32_t id, const std::string& lang = "en") {
        return "https://api.guildwars2.com/v2/traits/" + std::to_string(id) + "?lang=" + lang; }
    static bool Parse(const std::vector<char>& body, Meta& out, const std::string& lang = "en");
    // Every trait lives in /v2/traits — no wiki fallback, no multi-hop. All seams are no-ops.
    static std::string FallbackUrl(uint32_t, const std::string& = "en") { return ""; }
    static std::string FallbackUrl2(uint32_t, const std::string& = "en") { return ""; }
    static bool ParseFallback(const std::vector<char>&, Meta&, const std::string& = "en") { return false; }
    static bool ResolveDeps(Meta&, const HttpFetch&, const std::string& = "en") { return true; }
    static std::string EnrichUrl(uint32_t, const Meta&, const std::string& = "en") { return ""; }
    static bool ParseEnrich(const std::vector<char>&, Meta&, const std::string& = "en") { return false; }
    static const char* FileName() { return "traitinfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m);
    static void FromJson(const nlohmann::json& j, Meta& m);
};
}

#pragma once
#include "DecoderRingApi.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct SkillFactM { std::string icon, text; };
struct SkillMeta { std::string name, icon, description; std::vector<SkillFactM> facts; };
struct SkillTraits {
    using Meta = SkillMeta;
    static std::string Url(uint32_t id) { return "https://api.guildwars2.com/v2/skills/" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& out);
    // Wiki fallback: the /v2/skills API 404s on mount/turtle/transform/etc. skills,
    // so on a primary miss AsyncResolver retries against the wiki's SMW ask API.
    static std::string FallbackUrl(uint32_t id);
    static bool ParseFallback(const std::vector<char>& body, Meta& out);
    static const char* FileName() { return "skillinfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m);
    static void FromJson(const nlohmann::json& j, Meta& m);
};
}

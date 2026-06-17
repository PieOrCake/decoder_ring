#pragma once
#include "DecoderRingApi.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct ItemMeta {
    std::string name, icon;
    uint8_t bound = DB_None;
    bool noSell = false, tradeable = false;
    int32_t vendorValue = 0;
    uint8_t rarity = DR_RarityUnknown;   // from /v2 "rarity"; Unknown when absent (old cache)
    std::string description;             // flavour text (markup stripped), "" if none
    std::vector<std::string> lines;      // pre-formatted tooltip lines (defense, attrs, etc.)
};
struct ItemTraits {
    using Meta = ItemMeta;
    static std::string Url(uint32_t id) { return "https://api.guildwars2.com/v2/items/" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& out);
    static std::string FallbackUrl(uint32_t) { return ""; }                  // no fallback source
    static bool ParseFallback(const std::vector<char>&, Meta&) { return false; }
    static std::string EnrichUrl(uint32_t, const Meta&) { return ""; }       // no enrichment
    static bool ParseEnrich(const std::vector<char>&, Meta&) { return false; }
    static const char* FileName() { return "iteminfo_v2.json"; }   // v2: + description/lines; old shape refetched
    static nlohmann::json ToJson(const Meta& m);
    static void FromJson(const nlohmann::json& j, Meta& m);
};
}

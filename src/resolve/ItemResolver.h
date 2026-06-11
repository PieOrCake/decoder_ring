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
};
struct ItemTraits {
    using Meta = ItemMeta;
    static std::string Url(uint32_t id) { return "https://api.guildwars2.com/v2/items/" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& out);
    static const char* FileName() { return "iteminfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m);
    static void FromJson(const nlohmann::json& j, Meta& m);
};
}

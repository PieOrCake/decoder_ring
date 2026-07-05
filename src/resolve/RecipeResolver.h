#pragma once
#include "DecoderRingApi.h"
#include "resolve/Http.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace Decoder {
struct RecipeIng { uint32_t id; int count; };
struct RecipeMeta {
    std::string name, icon;                 // final: "Recipe: X (Rarity)", output icon ("" until ResolveDeps)
    std::vector<std::string> lines;         // ingredient lines + "Required Rating: N"
    uint32_t outputItemId = 0;              // -> overloaded into record.vendorValue
    int minRating = 0;                      // transient (Parse -> ResolveDeps)
    std::vector<RecipeIng> ingredients;     // item_id + count   (transient)
    std::vector<RecipeIng> guild;           // upgrade_id + count (transient)
};
struct RecipeTraits {
    using Meta = RecipeMeta;
    static std::string Url(uint32_t id, const std::string& lang = "en");                 // /v2/recipes/:id
    static bool Parse(const std::vector<char>&, Meta&, const std::string& lang = "en");  // recipe body -> transient fields
    static std::string FallbackUrl(uint32_t, const std::string& = "en") { return ""; }
    static bool ParseFallback(const std::vector<char>&, Meta&, const std::string& = "en") { return false; }
    static bool ResolveDeps(Meta&, const HttpFetch&, const std::string& lang = "en");    // Task 3
    static std::string EnrichUrl(uint32_t, const Meta&, const std::string& = "en") { return ""; }
    static bool ParseEnrich(const std::vector<char>&, Meta&, const std::string& = "en") { return false; }
    static const char* FileName() { return "recipeinfo_v1.json"; }
    static nlohmann::json ToJson(const Meta&);           // Task 4
    static void FromJson(const nlohmann::json&, Meta&);  // Task 4
};
}

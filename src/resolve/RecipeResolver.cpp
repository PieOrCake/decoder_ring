#include "resolve/RecipeResolver.h"

namespace Decoder {
namespace {
int I(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_number_integer()) ? j[k].get<int>() : 0;
}
void ReadIngs(const nlohmann::json& arr, const char* idKey, std::vector<RecipeIng>& out) {
    if (!arr.is_array()) return;
    for (auto& e : arr) {
        if (!e.is_object() || !e.contains(idKey)) continue;
        out.push_back({ (uint32_t)e[idKey].get<int>(), I(e, "count") });
    }
}
}

std::string RecipeTraits::Url(uint32_t id) {
    return "https://api.guildwars2.com/v2/recipes/" + std::to_string(id);
}

bool RecipeTraits::Parse(const std::vector<char>& body, Meta& out) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_object() || !j.contains("output_item_id")) return false;
        out.outputItemId = (uint32_t)j["output_item_id"].get<int>();
        out.minRating = I(j, "min_rating");
        if (j.contains("ingredients")) ReadIngs(j["ingredients"], "item_id", out.ingredients);
        if (j.contains("guild_ingredients")) ReadIngs(j["guild_ingredients"], "upgrade_id", out.guild);
        return true;
    } catch (...) { return false; }
}

// ResolveDeps -> Task 3 ; ToJson/FromJson -> Task 4 (stub here so it links):
bool RecipeTraits::ResolveDeps(Meta&, const HttpFetch&) { return true; }
nlohmann::json RecipeTraits::ToJson(const Meta& m) { return nlohmann::json::object(); }
void RecipeTraits::FromJson(const nlohmann::json&, Meta&) {}
}

#include "resolve/RecipeResolver.h"
#include <unordered_map>

namespace Decoder {
namespace {
int I(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_number_integer()) ? j[k].get<int>() : 0;
}
std::string S(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string();
}
void ReadIngs(const nlohmann::json& arr, const char* idKey, std::vector<RecipeIng>& out) {
    if (!arr.is_array()) return;
    for (auto& e : arr) {
        if (!e.is_object() || !e.contains(idKey)) continue;
        out.push_back({ (uint32_t)e[idKey].get<int>(), I(e, "count") });
    }
}
bool IsEquipmentType(const std::string& t) {
    return t=="Armor" || t=="Weapon" || t=="Back" || t=="Trinket" || t=="UpgradeComponent";
}
std::string Line(int count, const std::string& name) {
    return count > 1 ? (std::to_string(count) + " " + name) : name;
}
std::string JoinIds(uint32_t first, const std::vector<RecipeIng>& more) {
    std::string s = std::to_string(first);
    for (auto& i : more) s += "," + std::to_string(i.id);
    return s;
}
bool FetchArray(const HttpFetch& fetch, const std::string& url,
                std::unordered_map<uint32_t, nlohmann::json>& out) {
    std::vector<char> body;
    if (!fetch(url, body)) return false;
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_array()) return false;
        for (auto& e : j) if (e.is_object() && e.contains("id")) out[(uint32_t)e["id"].get<int>()] = e;
        return true;
    } catch (...) { return false; }
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

bool RecipeTraits::ResolveDeps(Meta& m, const HttpFetch& fetch) {
    std::unordered_map<uint32_t, nlohmann::json> items, guild;
    if (!FetchArray(fetch, "https://api.guildwars2.com/v2/items?ids=" + JoinIds(m.outputItemId, m.ingredients), items))
        return false;
    auto oit = items.find(m.outputItemId);
    if (oit == items.end()) return false;              // output must resolve
    const auto& o = oit->second;
    std::string oname = S(o, "name");
    if (oname.empty()) return false;
    m.icon = S(o, "icon");

    std::string title = "Recipe: " + oname;
    std::string otype = S(o, "type"), orar = S(o, "rarity");
    if (IsEquipmentType(otype) && !orar.empty()) title += " (" + orar + ")";
    m.name = title;

    m.lines.clear();
    for (auto& ing : m.ingredients) {
        auto it = items.find(ing.id);
        if (it != items.end()) m.lines.push_back(Line(ing.count, S(it->second, "name")));
    }
    if (!m.guild.empty()) {
        std::string gids; for (auto& g : m.guild) gids += (gids.empty()?"":",") + std::to_string(g.id);
        FetchArray(fetch, "https://api.guildwars2.com/v2/guild/upgrades?ids=" + gids, guild);  // best-effort
        for (auto& g : m.guild) {
            auto it = guild.find(g.id);
            if (it != guild.end()) m.lines.push_back(Line(g.count, S(it->second, "name")));
        }
    }
    if (m.minRating > 0) m.lines.push_back("Required Rating: " + std::to_string(m.minRating));
    return true;
}

// ToJson/FromJson -> Task 4 (stubs here so it links):
nlohmann::json RecipeTraits::ToJson(const Meta& m) { return nlohmann::json::object(); }
void RecipeTraits::FromJson(const nlohmann::json&, Meta&) {}
}

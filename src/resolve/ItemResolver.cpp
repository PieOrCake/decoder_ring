#include "resolve/ItemResolver.h"
#include <regex>

namespace Decoder {
namespace {
std::string S(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string();
}
int I(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_number_integer()) ? j[k].get<int>() : 0;
}
// GW2 /v2 attribute keys -> the names shown in-game (most pass through unchanged).
std::string AttrName(const std::string& a) {
    if (a=="CritDamage")        return "Ferocity";
    if (a=="ConditionDamage")   return "Condition Damage";
    if (a=="ConditionDuration") return "Expertise";
    if (a=="BoonDuration")      return "Concentration";
    if (a=="Healing")           return "Healing Power";
    if (a=="AgonyResistance")   return "Agony Resistance";
    return a;   // Power, Precision, Toughness, Vitality, or any future key verbatim
}
// Tidy the raw /v2 details.type for display: drop the generic "Default" (unidentified
// gear / no real slot), and turn "*Aquatic" armour enums into "Aquatic *" (e.g.
// "HelmAquatic" -> "Aquatic Helm"). Everything else passes through verbatim — real
// slots ("Coat", "Rifle") and aquatic *weapons* ("Trident", "Speargun") are unchanged.
std::string PrettySubtype(const std::string& t) {
    if (t.empty() || t == "Default") return "";
    static const std::string aq = "Aquatic";
    if (t.size() > aq.size() && t.compare(t.size() - aq.size(), aq.size(), aq) == 0)
        return "Aquatic " + t.substr(0, t.size() - aq.size());
    return t;
}
// Item flavour text -> plain text: <br> to newline, drop <c=…>/</c> and other tags.
std::string CleanItemText(std::string s) {
    using std::regex; using std::regex_replace;
    s = regex_replace(s, regex("<br ?/?>"), "\n");
    s = regex_replace(s, regex("<[^>]*>"), "");
    s = regex_replace(s, regex("^\\s+|\\s+$"), "");
    return s;
}
uint8_t BoundOf(const nlohmann::json& flags) {
    bool acc=false, soulAcq=false, accUse=false, soulUse=false;
    for (auto& f : flags) if (f.is_string()) { auto s=f.get<std::string>();
        if (s=="AccountBound") acc=true; else if (s=="SoulbindOnAcquire") soulAcq=true;
        else if (s=="AccountBindOnUse") accUse=true; else if (s=="SoulBindOnUse") soulUse=true; }
    if (soulAcq) return DB_SoulOnAcquire;
    if (acc)     return DB_AccountOnAcquire;
    if (soulUse) return DB_SoulOnUse;
    if (accUse)  return DB_AccountOnUse;
    return DB_None;
}
uint8_t RarityOf(const std::string& s) {
    if (s=="Junk")       return DR_Junk;
    if (s=="Basic")      return DR_Basic;
    if (s=="Fine")       return DR_Fine;
    if (s=="Masterwork") return DR_Masterwork;
    if (s=="Rare")       return DR_Rare;
    if (s=="Exotic")     return DR_Exotic;
    if (s=="Ascended")   return DR_Ascended;
    if (s=="Legendary")  return DR_Legendary;
    return DR_RarityUnknown;   // missing or unrecognised tier
}
}

bool ItemTraits::Parse(const std::vector<char>& body, Meta& out) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_object() || !j.contains("name")) return false;
        out.name = S(j, "name");
        out.icon = S(j, "icon");
        out.rarity = RarityOf(S(j, "rarity"));   // same /v2 body already parsed for name/icon
        out.vendorValue = (j.contains("vendor_value") && j["vendor_value"].is_number_integer())
                          ? j["vendor_value"].get<int>() : 0;
        if (j.contains("flags") && j["flags"].is_array()) {
            out.bound = BoundOf(j["flags"]);
            for (auto& f : j["flags"]) if (f.is_string() && f.get<std::string>()=="NoSell") out.noSell = true;
        }
        // Tradeable iff not bound-on-acquire (bind-on-use stays tradeable; NoSell does NOT gate TP).
        out.tradeable = !(out.bound == DB_AccountOnAcquire || out.bound == DB_SoulOnAcquire);

        // Full-tooltip surfacing (schema v3): flavour text + pre-formatted lines, all
        // distilled from this same document. Ordered for a top-down tooltip read.
        out.description = CleanItemText(S(j, "description"));
        if (j.contains("details") && j["details"].is_object()) {
            const auto& d = j["details"];
            int defense = I(d, "defense");
            if (defense > 0) out.lines.push_back("Defense: " + std::to_string(defense));
            int maxp = I(d, "max_power");
            if (maxp > 0) out.lines.push_back("Weapon Strength: " + std::to_string(I(d, "min_power")) + " - " + std::to_string(maxp));
            if (d.contains("infix_upgrade") && d["infix_upgrade"].is_object()
                && d["infix_upgrade"].contains("attributes") && d["infix_upgrade"]["attributes"].is_array())
                for (auto& a : d["infix_upgrade"]["attributes"]) {
                    if (!a.is_object()) continue;
                    std::string an = S(a, "attribute");
                    if (!an.empty()) out.lines.push_back("+" + std::to_string(I(a, "modifier")) + " " + AttrName(an));
                }
            if (d.contains("infusion_slots") && d["infusion_slots"].is_array() && !d["infusion_slots"].empty()) {
                size_t n = d["infusion_slots"].size();
                out.lines.push_back(n > 1 ? ("Unused Infusion Slot (x" + std::to_string(n) + ")")
                                          : std::string("Unused Infusion Slot"));
            }
            if (d.contains("bonuses") && d["bonuses"].is_array())          // rune set bonuses, verbatim
                for (auto& b : d["bonuses"]) if (b.is_string()) out.lines.push_back(b.get<std::string>());
            std::string sub = PrettySubtype(S(d, "type"));               // e.g. "Coat", "Aquatic Helm"; "" drops it
            if (!sub.empty()) out.lines.push_back(sub);
            std::string wc = S(d, "weight_class");                        // armour only
            if (!wc.empty()) out.lines.push_back(wc + " Armor");
        }
        int level = I(j, "level");
        if (level > 0) out.lines.push_back("Required Level: " + std::to_string(level));
        return true;
    } catch (...) { return false; }
}

nlohmann::json ItemTraits::ToJson(const Meta& m) {
    nlohmann::json ln = nlohmann::json::array();
    for (auto& l : m.lines) ln.push_back(l);
    return nlohmann::json{ {"n",m.name},{"ic",m.icon},{"b",m.bound},
                           {"ns",m.noSell},{"tr",m.tradeable},{"vv",m.vendorValue},{"rr",m.rarity},
                           {"d",m.description},{"ln",std::move(ln)} };
}
void ItemTraits::FromJson(const nlohmann::json& j, Meta& m) {
    if (!j.is_object()) return;
    if (j.contains("n"))  m.name = j["n"].get<std::string>();
    if (j.contains("ic")) m.icon = j["ic"].get<std::string>();
    if (j.contains("b"))  m.bound = j["b"].get<uint8_t>();
    if (j.contains("ns")) m.noSell = j["ns"].get<bool>();
    if (j.contains("tr")) m.tradeable = j["tr"].get<bool>();
    if (j.contains("vv")) m.vendorValue = j["vv"].get<int>();
    // Pre-v2 cache files lack "rr"; the key guard leaves the DR_RarityUnknown default.
    if (j.contains("rr")) m.rarity = j["rr"].get<uint8_t>();
    if (j.contains("d"))  m.description = j["d"].get<std::string>();
    if (j.contains("ln") && j["ln"].is_array())
        for (auto& l : j["ln"]) if (l.is_string()) m.lines.push_back(l.get<std::string>());
}
}

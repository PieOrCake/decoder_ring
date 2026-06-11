#include "resolve/ItemResolver.h"

namespace Decoder {
namespace {
std::string S(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string();
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
        return true;
    } catch (...) { return false; }
}

nlohmann::json ItemTraits::ToJson(const Meta& m) {
    return nlohmann::json{ {"n",m.name},{"ic",m.icon},{"b",m.bound},
                           {"ns",m.noSell},{"tr",m.tradeable},{"vv",m.vendorValue},{"rr",m.rarity} };
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
}
}

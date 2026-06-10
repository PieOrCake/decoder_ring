#include "resolve/SkillResolver.h"
#include <cstdio>

namespace Decoder {
namespace {
std::string NumStr(const nlohmann::json& j, const char* k) {
    if (!j.contains(k) || !j[k].is_number()) return "";
    double v = j[k].get<double>(); long long iv = (long long)v;
    if ((double)iv == v) return std::to_string(iv);
    char b[32]; std::snprintf(b, sizeof(b), "%g", v); return b;
}
// Port of Pie UI SkillInfoCache::FormatFact — pre-formats one API fact.
std::string FormatFact(const nlohmann::json& f) {
    std::string type = f.value("type", std::string());
    std::string text = f.value("text", std::string());
    if (type=="Range"||type=="Number"||type=="Radius"||type=="Distance"||type=="AttributeAdjust"||type=="HealingAdjust") {
        std::string v = NumStr(f,"value"); if (v.empty()) v = NumStr(f,"distance");
        return v.empty()?text:(text+": "+v); }
    if (type=="Percent") { std::string v=NumStr(f,"percent"); return v.empty()?text:(text+": "+v+"%"); }
    if (type=="Time"||type=="Duration"||type=="Recharge") { std::string v=NumStr(f,"duration"); return v.empty()?text:(text+": "+v+"s"); }
    if (type=="Damage") { std::string hc=NumStr(f,"hit_count"); return hc.empty()?text:(text+" (x"+hc+")"); }
    if (type=="ComboField") { std::string v=f.value("field_type",std::string()); return v.empty()?text:(text+": "+v); }
    if (type=="ComboFinisher") { std::string v=f.value("finisher_type",std::string()); return v.empty()?text:(text+": "+v); }
    if (type=="Buff"||type=="PrefixedBuff") {
        std::string status=f.value("status",std::string()); std::string s=status.empty()?text:status;
        std::string ac=NumStr(f,"apply_count"); if (!ac.empty()&&ac!="0"&&ac!="1") s=ac+"x "+s;
        std::string dur=NumStr(f,"duration"); if (!dur.empty()&&dur!="0") s+=" ("+dur+"s)";
        return s; }
    return text;
}
}

bool SkillTraits::Parse(const std::vector<char>& body, Meta& out) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_object() || !j.contains("name")) return false;
        if (j["name"].is_string()) out.name = j["name"].get<std::string>();
        if (j.contains("icon") && j["icon"].is_string()) out.icon = j["icon"].get<std::string>();
        if (j.contains("description") && j["description"].is_string()) out.description = j["description"].get<std::string>();
        if (j.contains("facts") && j["facts"].is_array())
            for (auto& f : j["facts"]) {
                if (!f.is_object()) continue;
                SkillFactM sf; sf.text = FormatFact(f);
                if (sf.text.empty()) continue;
                if (f.contains("icon") && f["icon"].is_string()) sf.icon = f["icon"].get<std::string>();
                out.facts.push_back(std::move(sf));
            }
        return true;
    } catch (...) { return false; }
}

nlohmann::json SkillTraits::ToJson(const Meta& m) {
    nlohmann::json fa = nlohmann::json::array();
    for (auto& f : m.facts) fa.push_back(nlohmann::json{ {"ic",f.icon},{"t",f.text} });
    return nlohmann::json{ {"n",m.name},{"ic",m.icon},{"d",m.description},{"f",std::move(fa)} };
}
void SkillTraits::FromJson(const nlohmann::json& j, Meta& m) {
    if (!j.is_object()) return;
    if (j.contains("n")) m.name = j["n"].get<std::string>();
    if (j.contains("ic")) m.icon = j["ic"].get<std::string>();
    if (j.contains("d")) m.description = j["d"].get<std::string>();
    if (j.contains("f") && j["f"].is_array())
        for (auto& fj : j["f"]) { SkillFactM sf;
            if (fj.contains("ic")) sf.icon = fj["ic"].get<std::string>();
            if (fj.contains("t"))  sf.text = fj["t"].get<std::string>();
            if (!sf.text.empty()) m.facts.push_back(std::move(sf)); }
}
}

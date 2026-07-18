#include "resolve/SkillFact.h"
#include <cstdio>

namespace Decoder {
namespace {
std::string NumStr(const nlohmann::json& j, const char* k) {
    if (!j.contains(k) || !j[k].is_number()) return "";
    double v = j[k].get<double>(); long long iv = (long long)v;
    if ((double)iv == v) return std::to_string(iv);
    char b[32]; std::snprintf(b, sizeof(b), "%g", v); return b;
}
}
// Port of Pie UI SkillInfoCache::FormatFact — pre-formats one API fact.
std::string FormatApiFact(const nlohmann::json& f) {
    std::string type = f.value("type", std::string());
    std::string text = f.value("text", std::string());
    if (type=="Range"||type=="Number"||type=="Radius"||type=="Distance"||type=="AttributeAdjust"||type=="HealingAdjust") {
        std::string v = NumStr(f,"value"); if (v.empty()) v = NumStr(f,"distance");
        return v.empty()?text:(text+": "+v); }
    if (type=="Percent") { std::string v=NumStr(f,"percent"); return v.empty()?text:(text+": "+v+"%"); }
    if (type=="Time"||type=="Duration") { std::string v=NumStr(f,"duration"); return v.empty()?text:(text+": "+v+"s"); }
    if (type=="Recharge") { std::string v=NumStr(f,"value"); return v.empty()?text:(text+": "+v+"s"); }  // API stores cooldown in "value", not "duration"
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

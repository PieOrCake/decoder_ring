#include "resolve/SkillResolver.h"
#include "resolve/Labels.h"
#include <cstdio>
#include <regex>

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

// --- Wiki fallback helpers ----------------------------------------------------
std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF"; std::string o;
    for (unsigned char c : s) {
        bool u = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~';
        if (u) o += (char)c; else { o += '%'; o += hex[c>>4]; o += hex[c&0xF]; }
    }
    return o;
}

// Reduce wiki markup (links, bold/italic, file/category tags, entities) to plain text.
std::string CleanWiki(std::string s) {
    using std::regex; using std::regex_replace;
    s = regex_replace(s, regex("\\[\\[:?(?:File|Category):[^\\]]*\\]\\]"), "");
    s = regex_replace(s, regex("\\[\\[[^\\]|]*\\|([^\\]]*)\\]\\]"), "$1");   // [[target|label]] -> label
    s = regex_replace(s, regex("\\[\\[([^\\]]*)\\]\\]"), "$1");              // [[label]] -> label
    s = regex_replace(s, regex("'''|''"), "");
    s = regex_replace(s, regex("&nbsp;|&#160;|&#32;"), " ");
    s = regex_replace(s, regex("&amp;"), "&");
    return s;
}

// One wiki skill-fact HTML line -> plain text. "" for the competitive (wvw/pvp) variant.
std::string StripWikiFact(std::string s) {
    if (s.find("wvw pvp") != std::string::npos) return "";   // keep the PvE variant only
    using std::regex; using std::regex_replace;
    s = regex_replace(s, regex("<sup[^>]*>.*?</sup>"), "");   // drop the "?" coefficient note
    s = regex_replace(s, regex("<[^>]*>"), "");               // drop HTML tags
    s = CleanWiki(s);
    s = regex_replace(s, regex("^[\\s:]+"), "");              // strip the ":" bullet
    s = regex_replace(s, regex("\\s+"), " ");
    s = regex_replace(s, regex("\\s+$"), "");
    return s;
}

// True if the meta already carries a defiance fact (wiki-fallback skills do).
bool HasDefianceFact(const SkillMeta& m) {
    for (const auto& f : m.facts) if (f.text.rfind("Defiance Break", 0) == 0) return true;
    return false;
}
// Pull the integer right after "Defiance Break]]:" out of a raw wiki facts blob
// (0 if absent). Reads only the digits immediately following the key, so a later
// stray number in the wiki's markup (e.g. a "sic" note) can't fool it.
int ParseDefiance(const std::string& blob) {
    const std::string key = "Defiance Break]]:";
    size_t p = blob.find(key);
    if (p == std::string::npos) return 0;
    p += key.size();
    while (p < blob.size() && (blob[p]==' '||blob[p]=='\t')) ++p;
    int v=0; bool any=false;
    while (p < blob.size() && blob[p]>='0' && blob[p]<='9') { v=v*10+(blob[p]-'0'); ++p; any=true; }
    return any ? v : 0;
}
}

bool SkillTraits::Parse(const std::vector<char>& body, Meta& out, const std::string& lang) {
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

std::string SkillTraits::FallbackUrl(uint32_t id, const std::string& lang) {
    // [[Has context::Skill]] is ESSENTIAL — the wiki game-id space is shared across
    // object types, so a skill id can collide with an item/skin of the same id
    // (skill 63440 "Open Access" collides with item "Defender's Staff"). Without the
    // constraint you resolve the skill link to the item.
    std::string q = "[[Has game id::" + std::to_string(id) +
        "]][[Has context::Skill]]|?Has canonical name|?Has game description|?Has skill facts";
    return "https://wiki.guildwars2.com/api.php?action=ask&query=" + UrlEncode(q) + "&format=json";
}

bool SkillTraits::ParseFallback(const std::vector<char>& body, Meta& out, const std::string& lang) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.contains("query") || !j["query"].contains("results")) return false;
        const auto& results = j["query"]["results"];
        if (!results.is_object()) return false;   // an empty result set comes back as []
        const nlohmann::json* best = nullptr; std::string bestName; bool bestHasFacts = false;
        for (auto it = results.begin(); it != results.end(); ++it) {
            const auto& po = it.value();
            if (!po.contains("printouts")) continue;
            const auto& pr = po["printouts"];
            std::string cname;
            if (pr.contains("Has canonical name") && pr["Has canonical name"].is_array()
                && !pr["Has canonical name"].empty() && pr["Has canonical name"][0].is_string())
                cname = pr["Has canonical name"][0].get<std::string>();
            if (cname.empty()) continue;
            bool hasFacts = pr.contains("Has skill facts") && pr["Has skill facts"].is_array()
                            && !pr["Has skill facts"].empty();
            // Prefer a facts-bearing page among variants (handles #WvW,PvP subpages).
            if (!best || (hasFacts && !bestHasFacts)) { best = &po; bestName = cname; bestHasFacts = hasFacts; }
        }
        if (!best) return false;
        out.name = bestName;   // canonical name -> matches the game ("Slam", not "Slam (turtle)")
        const auto& pr = (*best)["printouts"];
        if (pr.contains("Has game description") && pr["Has game description"].is_array()
            && !pr["Has game description"].empty() && pr["Has game description"][0].is_string()) {
            std::string d = pr["Has game description"][0].get<std::string>();
            d = std::regex_replace(d, std::regex("<[^>]*>"), "");
            d = CleanWiki(d);
            d = std::regex_replace(d, std::regex("\\s+"), " ");
            d = std::regex_replace(d, std::regex("^\\s+|\\s+$"), "");
            out.description = d;
        }
        if (pr.contains("Has skill facts") && pr["Has skill facts"].is_array())
            for (const auto& fb : pr["Has skill facts"]) {
                if (!fb.is_string()) continue;
                std::string blob = fb.get<std::string>();
                for (size_t start = 0; start <= blob.size(); ) {
                    size_t nl = blob.find('\n', start);
                    std::string line = blob.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                    std::string txt = StripWikiFact(line);
                    if (!txt.empty()) { SkillFactM sf; sf.text = txt; out.facts.push_back(std::move(sf)); }
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
            }
        return !out.name.empty();
    } catch (...) { return false; }
}

std::string SkillTraits::EnrichUrl(uint32_t id, const Meta& m, const std::string& lang) {
    // /v2/skills has no breakbar field; the wiki does. Skip when there's no name to
    // match against, or when a defiance fact is already present (a wiki-fallback skill
    // brought it in) — that avoids a redundant fetch and any chance of doubling.
    if (id == 0 || m.name.empty() || HasDefianceFact(m)) return "";
    std::string q = "[[Has game id::" + std::to_string(id) + "]][[Has context::Skill]]|?Has skill facts";
    return "https://wiki.guildwars2.com/api.php?action=ask&query=" + UrlEncode(q) + "&format=json";
}

bool SkillTraits::ParseEnrich(const std::vector<char>& body, Meta& out, const std::string& lang) {
    if (HasDefianceFact(out)) return false;   // never double an existing defiance line
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.contains("query") || !j["query"].contains("results")) return false;
        const auto& results = j["query"]["results"];
        if (!results.is_object()) return false;
        for (auto it = results.begin(); it != results.end(); ++it) {
            // Match the API skill by page title, stripping any #PvP/#WvW mode variant.
            std::string page = it.key();
            std::string base = page.substr(0, page.find('#'));
            if (base != out.name) continue;
            const auto& po = it.value();
            if (!po.contains("printouts")) continue;
            const auto& pr = po["printouts"];
            if (!pr.contains("Has skill facts") || !pr["Has skill facts"].is_array()) continue;
            std::string blob;
            for (const auto& s : pr["Has skill facts"])
                if (s.is_string()) { if (!blob.empty()) blob += '\n'; blob += s.get<std::string>(); }
            int v = ParseDefiance(blob);
            if (v > 0) { SkillFactM sf; sf.text = Label("DefianceBreak", lang) + ": " + std::to_string(v); out.facts.push_back(std::move(sf)); return true; }
        }
        return false;
    } catch (...) { return false; }
}
}

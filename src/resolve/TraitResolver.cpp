#include "resolve/TraitResolver.h"
#include "resolve/SkillFact.h"

namespace Decoder {
bool TraitTraits::Parse(const std::vector<char>& body, Meta& out, const std::string& /*lang*/) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_object() || !j.contains("name")) return false;   // "no such id" body has no "name"
        if (j["name"].is_string()) out.name = j["name"].get<std::string>();
        if (j.contains("icon") && j["icon"].is_string()) out.icon = j["icon"].get<std::string>();
        if (j.contains("description") && j["description"].is_string()) out.description = j["description"].get<std::string>();
        if (j.contains("facts") && j["facts"].is_array())
            for (auto& f : j["facts"]) {
                if (!f.is_object()) continue;
                TraitFactM sf; sf.text = FormatApiFact(f);
                if (sf.text.empty()) continue;
                if (f.contains("icon") && f["icon"].is_string()) sf.icon = f["icon"].get<std::string>();
                out.facts.push_back(std::move(sf));
            }
        return true;
    } catch (...) { return false; }
}

nlohmann::json TraitTraits::ToJson(const Meta& m) {
    nlohmann::json fa = nlohmann::json::array();
    for (auto& f : m.facts) fa.push_back(nlohmann::json{ {"ic",f.icon},{"t",f.text} });
    return nlohmann::json{ {"n",m.name},{"ic",m.icon},{"d",m.description},{"f",std::move(fa)} };
}
void TraitTraits::FromJson(const nlohmann::json& j, Meta& m) {
    if (!j.is_object()) return;
    if (j.contains("n")) m.name = j["n"].get<std::string>();
    if (j.contains("ic")) m.icon = j["ic"].get<std::string>();
    if (j.contains("d")) m.description = j["d"].get<std::string>();
    if (j.contains("f") && j["f"].is_array())
        for (auto& fj : j["f"]) { TraitFactM sf;
            if (fj.contains("ic")) sf.icon = fj["ic"].get<std::string>();
            if (fj.contains("t"))  sf.text = fj["t"].get<std::string>();
            if (!sf.text.empty()) m.facts.push_back(std::move(sf)); }
}
}

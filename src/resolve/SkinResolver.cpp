#include "resolve/SkinResolver.h"
namespace Decoder {
bool SkinTraits::Parse(const std::vector<char>& body, Meta& out, const std::string& lang) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_object() || !j.contains("name")) return false;
        if (j["name"].is_string()) out.name = j["name"].get<std::string>();
        if (j.contains("icon") && j["icon"].is_string()) out.icon = j["icon"].get<std::string>();
        return true;
    } catch (...) { return false; }
}
}

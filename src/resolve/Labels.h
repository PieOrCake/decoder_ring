#pragma once
#include <string>

namespace Decoder {
// Localized DR-authored tooltip label for `key` in `lang` (en/de/fr/es). Falls back
// to the English value, then to `key` itself. The English column reproduces the
// exact strings DR emits today, so English output is unchanged. Attribute keys use
// the /v2 attribute spelling (e.g. "CritDamage" -> "Ferocity"); weight-class keys
// use the /v2 weight_class spelling (e.g. "Heavy" -> "Heavy Armor"); rarity keys use
// the /v2 rarity spelling. Unknown keys (free-form subtypes) return verbatim.
std::string Label(const std::string& key, const std::string& lang);
}

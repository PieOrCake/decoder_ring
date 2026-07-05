#include "resolve/Labels.h"
#include <unordered_map>

namespace Decoder {
namespace {
// key -> per-language value. Absent (lang,key) falls through to "en", then to key.
using Row = std::unordered_map<std::string, std::string>;   // lang -> value
const std::unordered_map<std::string, Row>& Table() {
    static const std::unordered_map<std::string, Row> t = {
        // --- fixed line labels ---
        {"Defense",            {{"en","Defense"},            {"de","Verteidigung"},        {"fr","Défense"},              {"es","Defensa"}}},
        {"WeaponStrength",     {{"en","Weapon Strength"},    {"de","Waffenstärke"},        {"fr","Puissance de l'arme"},  {"es","Daño del arma"}}},
        {"RequiredLevel",      {{"en","Required Level"},      {"de","Benötigte Stufe"},     {"fr","Niveau requis"},        {"es","Nivel requerido"}}},
        {"RequiredRating",     {{"en","Required Rating"},     {"de","Benötigter Wert"},     {"fr","Niveau requis"},        {"es","Nivel requerido"}}},
        {"Recipe",             {{"en","Recipe"},              {"de","Rezept"},              {"fr","Recette"},              {"es","Receta"}}},
        {"DefianceBreak",      {{"en","Defiance Break"},      {"de","Entschlossenheitsbruch"},{"fr","Rupture de bravoure"},{"es","Ruptura de desafío"}}},
        {"UnusedInfusionSlot", {{"en","Unused Infusion Slot"},{"de","Unbenutzter Infusionsplatz"},{"fr","Emplacement d'infusion inutilisé"},{"es","Ranura de infusión sin usar"}}},
        // --- attributes (/v2 attribute key -> display) ---
        {"Power",           {{"en","Power"},            {"de","Kraft"},          {"fr","Puissance"},          {"es","Poder"}}},
        {"Precision",       {{"en","Precision"},        {"de","Präzision"},      {"fr","Précision"},          {"es","Precisión"}}},
        {"Toughness",       {{"en","Toughness"},        {"de","Zähigkeit"},      {"fr","Robustesse"},         {"es","Dureza"}}},
        {"Vitality",        {{"en","Vitality"},         {"de","Vitalität"},      {"fr","Vitalité"},           {"es","Vitalidad"}}},
        {"CritDamage",      {{"en","Ferocity"},         {"de","Wildheit"},       {"fr","Férocité"},           {"es","Ferocidad"}}},
        {"ConditionDamage", {{"en","Condition Damage"}, {"de","Zustandsschaden"},{"fr","Dégâts d'altération"},{"es","Daño de condición"}}},
        {"ConditionDuration",{{"en","Expertise"},       {"de","Kompetenz"},      {"fr","Expertise"},          {"es","Competencia"}}},
        {"BoonDuration",    {{"en","Concentration"},    {"de","Konzentration"},  {"fr","Concentration"},      {"es","Concentración"}}},
        {"Healing",         {{"en","Healing Power"},    {"de","Heilkraft"},      {"fr","Guérison"},           {"es","Poder de sanación"}}},
        {"AgonyResistance", {{"en","Agony Resistance"}, {"de","Qual-Widerstand"},{"fr","Résistance à l'agonie"},{"es","Resistencia a la agonía"}}},
        // --- weight-class phrase (/v2 weight_class -> "<X> Armor" wording) ---
        {"Heavy",    {{"en","Heavy Armor"},    {"de","Schwere Rüstung"},   {"fr","Armure lourde"},   {"es","Armadura pesada"}}},
        {"Medium",   {{"en","Medium Armor"},   {"de","Mittlere Rüstung"},  {"fr","Armure intermédiaire"},{"es","Armadura media"}}},
        {"Light",    {{"en","Light Armor"},    {"de","Leichte Rüstung"},   {"fr","Armure légère"},   {"es","Armadura ligera"}}},
        {"Clothing", {{"en","Clothing Armor"}, {"de","Kleidung Armor"},    {"fr","Vêtement Armor"},  {"es","Ropa Armor"}}},
        // --- rarity words (/v2 rarity -> display), for the recipe "(Rarity)" suffix ---
        {"Junk",       {{"en","Junk"},       {"de","Ramsch"},      {"fr","Camelote"},   {"es","Basura"}}},
        {"Basic",      {{"en","Basic"},      {"de","Einfach"},     {"fr","Basique"},    {"es","Básico"}}},
        {"Fine",       {{"en","Fine"},       {"de","Fein"},        {"fr","Raffiné"},    {"es","Fino"}}},
        {"Masterwork", {{"en","Masterwork"}, {"de","Meisterwerk"}, {"fr","Chef-d'œuvre"},{"es","Obra maestra"}}},
        {"Rare",       {{"en","Rare"},       {"de","Selten"},      {"fr","Rare"},       {"es","Raro"}}},
        {"Exotic",     {{"en","Exotic"},     {"de","Exotisch"},    {"fr","Exotique"},   {"es","Exótico"}}},
        {"Ascended",   {{"en","Ascended"},   {"de","Aufgestiegen"},{"fr","Élevé"},      {"es","Ascendido"}}},
        {"Legendary",  {{"en","Legendary"},  {"de","Legendär"},    {"fr","Légendaire"}, {"es","Legendario"}}},
    };
    return t;
}
}

std::string Label(const std::string& key, const std::string& lang) {
    auto row = Table().find(key);
    if (row == Table().end()) return key;                 // free-form (e.g. subtype) -> verbatim
    auto v = row->second.find(lang);
    if (v != row->second.end()) return v->second;         // requested language
    auto en = row->second.find("en");
    return en != row->second.end() ? en->second : key;    // English fallback, then key
}
}

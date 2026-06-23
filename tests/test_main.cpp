// Decoder Ring host test suite. Pure logic only — no Nexus, no Win32, no real HTTP.
#include "DecoderRingApi.h"
#include "resolve/OfflineResolve.h"
#include "chat/ChatLinks.h"
#include "resolve/AsyncResolver.h"
#include "resolve/ItemResolver.h"
#include "resolve/SkinResolver.h"
#include "resolve/SkillResolver.h"
#include "resolve/PriceCache.h"
#include "resolve/DecoderService.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>

static int g_fail = 0;
#define CHECK(cond) do { if(!(cond)){ std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while(0)

static void test_abi_is_pod() {
    // The record crosses a DLL boundary in shared memory + event payloads, so it
    // MUST be trivially copyable with the version field first.
    CHECK(std::is_trivially_copyable<DecoderRecord>::value);
    CHECK(offsetof(DecoderRecord, schemaVersion) == 0);
    CHECK(DECODER_RING_API_VERSION >= 1);
    CHECK(std::strcmp(DECODER_RING_DATALINK, "DECODER_RING_API") == 0);
}

static void test_offline_build_label() {
    using namespace PieUI::ChatLinks;
    // A build link whose 3rd-slot spec is Mirage (59) on Mesmer (7).
    DecodedBuildLink b{};
    b.profession = PROF_MESMER;
    b.specs[2].spec_id = 59;  // Mirage
    std::string link = EncodeBuild(b);

    DecoderRecord r{};
    bool ok = Decoder::ResolveOffline(LINK_BUILD, 0, link, r);
    CHECK(ok);
    CHECK(r.status == DR_Resolved);
    CHECK(std::strcmp(r.name, "Mirage Build") == 0);

    // Core profession fallback when no elite spec in the 3rd slot.
    DecodedBuildLink c{}; c.profession = PROF_MESMER;
    DecoderRecord r2{};
    CHECK(Decoder::ResolveOffline(LINK_BUILD, 0, EncodeBuild(c), r2));
    CHECK(std::strcmp(r2.name, "Mesmer Build") == 0);
}

static void test_offline_waypoint() {
    using namespace PieUI::ChatLinks;
    // Find any id in [1,200] that resolves from the compiled-in waypoint table.
    // Pass an EMPTY chat code to prove a waypoint resolves from its id alone.
    DecoderRecord r{};
    bool any = false;
    for (uint32_t id = 1; id <= 200 && !any; ++id) {
        DecoderRecord t{};
        if (Decoder::ResolveOffline(LINK_MAP, id, std::string(), t) && t.status == DR_Resolved) {
            any = true; r = t;
        }
    }
    CHECK(any);                 // at least one id in [1,200] resolves from the table
    CHECK(r.name[0] != '\0');   // resolved waypoint has a name
}

// A trivial Meta + Traits to test the generic core in isolation.
struct FakeMeta { std::string value; };
struct FakeTraits {
    using Meta = FakeMeta;
    static std::string Url(uint32_t id) { return "fake://" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& m) {
        m.value.assign(body.begin(), body.end());
        return !m.value.empty();
    }
    static std::string FallbackUrl(uint32_t) { return ""; }                  // no fallback source
    static bool ParseFallback(const std::vector<char>&, Meta&) { return false; }
    static bool ResolveDeps(Meta&, const Decoder::HttpFetch&) { return true; } // no dependent fetches
    static std::string EnrichUrl(uint32_t, const Meta&) { return ""; }       // no enrichment
    static bool ParseEnrich(const std::vector<char>&, Meta&) { return false; }
    // No disk in this test.
    static const char* FileName() { return ""; }
    static nlohmann::json ToJson(const Meta& m) { return m.value; }
    static void FromJson(const nlohmann::json& j, Meta& m) { if (j.is_string()) m.value = j.get<std::string>(); }
};

struct DepsTraits {
    using Meta = FakeMeta;
    static std::string Url(uint32_t id) { return "fake://" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& m) { m.value.assign(body.begin(), body.end()); return !m.value.empty(); }
    static std::string FallbackUrl(uint32_t) { return ""; }
    static bool ParseFallback(const std::vector<char>&, Meta&) { return false; }
    static bool ResolveDeps(Meta& m, const Decoder::HttpFetch& fetch) {
        std::vector<char> b; if (!fetch("dep://x", b)) return false;
        m.value += "+" + std::string(b.begin(), b.end()); return true;
    }
    static std::string EnrichUrl(uint32_t, const Meta&) { return ""; }
    static bool ParseEnrich(const std::vector<char>&, Meta&) { return false; }
    static const char* FileName() { return ""; }
    static nlohmann::json ToJson(const Meta& m) { return m.value; }
    static void FromJson(const nlohmann::json& j, Meta& m) { if (j.is_string()) m.value = j.get<std::string>(); }
};
static void test_async_resolve_deps() {
    using R = Decoder::AsyncResolver<DepsTraits>;
    R res;
    res.Initialize("", [](const std::string& url, std::vector<char>& out) {
        std::string s = (url.rfind("dep://",0)==0) ? "DEP" : "OK"; out.assign(s.begin(), s.end()); return true;
    });
    using GS = Decoder::GetState; FakeMeta m;
    CHECK(res.Get(7, m) == GS::Pending);
    std::vector<std::pair<uint32_t,bool>> done;
    for (int i=0;i<200 && done.empty();++i){ res.DrainCompleted(done); R::SleepMs(5); }
    CHECK(done.size()==1 && done[0].second==true);
    CHECK(res.Get(7, m) == GS::Warm);
    CHECK(m.value == "OK+DEP");          // ResolveDeps ran before warm + before completion
    res.Shutdown();
}

static void test_async_state_machine() {
    using R = Decoder::AsyncResolver<FakeTraits>;
    R res;
    std::atomic<int> calls{0};
    bool failNext = true;
    // Fake fetch: first call for any id FAILS, subsequent calls succeed.
    res.Initialize("", [&](const std::string& url, std::vector<char>& out) {
        ++calls;
        if (failNext) { failNext = false; return false; }   // first call fails
        std::string s = "OK:" + url; out.assign(s.begin(), s.end()); return true;
    });

    using GS = Decoder::GetState;
    FakeMeta m;
    // Knowably-invalid id -> synchronous Failed: no fetch, no completion.
    CHECK(res.Get(0, m) == GS::Failed);
    // Cold query -> Pending (kicks fetch; a completion is guaranteed to follow).
    CHECK(res.Get(42, m) == GS::Pending);
    // Drive the worker to completion (poll the completed queue).
    std::vector<std::pair<uint32_t,bool>> done;
    for (int i = 0; i < 200 && done.empty(); ++i) { res.DrainCompleted(done); R::SleepMs(5); }
    CHECK(done.size() == 1);
    CHECK(done[0].first == 42);
    CHECK(done[0].second == false);            // first fetch failed
    CHECK(res.Get(42, m) == GS::Failed);       // known failure, still in cooldown -> synchronous Failed

    // Re-query with cooldown bypassed -> retried (Pending again), this time succeeds.
    res.SetFailCooldownSec(0);
    CHECK(res.Get(42, m) == GS::Pending);      // cooldown expired -> kicks a fresh retry
    done.clear();
    for (int i = 0; i < 200 && done.empty(); ++i) { res.DrainCompleted(done); R::SleepMs(5); }
    CHECK(done.size() == 1);
    CHECK(done[0].second == true);             // retry succeeded
    CHECK(res.Get(42, m) == GS::Warm);         // now warm
    CHECK(m.value == "OK:fake://42");
    res.Shutdown();
}

static std::vector<char> Bytes(const char* s) { return std::vector<char>(s, s + std::strlen(s)); }

static void test_item_parse() {
    const char* json = R"({"name":"Berserker's Sword","icon":"https://x/sword.png",
        "vendor_value":33,"rarity":"Exotic","flags":["AccountBound","NoSell"],"type":"Weapon",
        "details":{"type":"Sword"}})";
    Decoder::ItemMeta m;
    CHECK(Decoder::ItemTraits::Parse(Bytes(json), m));
    CHECK(m.name == "Berserker's Sword");
    CHECK(m.icon == "https://x/sword.png");
    CHECK(m.vendorValue == 33);
    CHECK(m.noSell == true);
    CHECK(m.bound == DB_AccountOnAcquire);
    CHECK(m.tradeable == false);   // account-bound-on-acquire => not tradeable
    CHECK(m.rarity == DR_Exotic);  // rarity distilled from the same /v2 body
}

// Rarity: every GW2 tier maps; an unknown/absent rarity stays DR_RarityUnknown.
static void test_item_rarity_values() {
    struct { const char* s; uint8_t want; } cases[] = {
        {"Junk",DR_Junk},{"Basic",DR_Basic},{"Fine",DR_Fine},{"Masterwork",DR_Masterwork},
        {"Rare",DR_Rare},{"Exotic",DR_Exotic},{"Ascended",DR_Ascended},{"Legendary",DR_Legendary},
    };
    for (auto& c : cases) {
        std::string json = std::string("{\"name\":\"x\",\"rarity\":\"") + c.s + "\"}";
        Decoder::ItemMeta m;
        CHECK(Decoder::ItemTraits::Parse(Bytes(json.c_str()), m));
        CHECK(m.rarity == c.want);
    }
    // No rarity field at all -> Unknown, still parses fine.
    Decoder::ItemMeta none;
    CHECK(Decoder::ItemTraits::Parse(Bytes(R"({"name":"x"})"), none));
    CHECK(none.rarity == DR_RarityUnknown);
    // Unrecognised tier string -> Unknown (never a bogus tier).
    Decoder::ItemMeta weird;
    CHECK(Decoder::ItemTraits::Parse(Bytes(R"({"name":"x","rarity":"Mythic"})"), weird));
    CHECK(weird.rarity == DR_RarityUnknown);
}

// Disk-cache compatibility: rarity round-trips, and a PRE-v2 cache entry lacking
// the "rr" key must read back as DR_RarityUnknown with no error (no poison/discard).
static void test_item_rarity_cache_compat() {
    // Round-trip through the disk JSON keeps the tier.
    Decoder::ItemMeta in; in.name = "Sword"; in.rarity = DR_Ascended;
    nlohmann::json j = Decoder::ItemTraits::ToJson(in);
    Decoder::ItemMeta out; Decoder::ItemTraits::FromJson(j, out);
    CHECK(out.rarity == DR_Ascended);

    // Old cache file shape (no "rr") -> defaults to Unknown, other fields still read.
    nlohmann::json old = { {"n","Old Sword"},{"ic","i"},{"b",DB_None},
                           {"ns",false},{"tr",true},{"vv",10} };
    Decoder::ItemMeta legacy; Decoder::ItemTraits::FromJson(old, legacy);
    CHECK(legacy.name == "Old Sword");
    CHECK(legacy.rarity == DR_RarityUnknown);   // missing field -> Unknown, no crash
}

// Service level: a stubbed /v2 item carrying a rarity resolves to a record that
// carries the matching DecoderRarity value end-to-end.
static void test_service_item_rarity() {
    using namespace PieUI::ChatLinks;
    std::vector<DecoderRecord> events;
    Decoder::DecoderService svc;
    svc.Initialize("",
        [&](const std::string&, std::vector<char>& out){
            const char* j = R"({"name":"Twilight","icon":"i","rarity":"Legendary","vendor_value":100})";
            out.assign(j, j + std::strlen(j)); return true;
        },
        [&](const DecoderRecord& r){ events.push_back(r); });

    DecoderRecord r;
    CHECK(svc.Resolve(LINK_ITEM, 30704, r) == DR_NotReady);   // cold -> fetch kicked
    for (int i=0;i<200 && events.empty();++i){ svc.Tick(); Decoder::DecoderService::SleepMs(5); }
    CHECK(events.size() == 1);
    CHECK(events[0].status == DR_Resolved);
    CHECK(events[0].rarity == DR_Legendary);                  // event record carries rarity
    // Warm re-query also carries it.
    CHECK(svc.Resolve(LINK_ITEM, 30704, r) == DR_Resolved);
    CHECK(r.rarity == DR_Legendary);
    svc.Shutdown();
}

// --- Item full-tooltip surfacing (schema v3) ---------------------------------
// Real, untrimmed /v2/items bodies. Exercise the line mapping against genuine
// API output (incl. the CritDamage->Ferocity mapping and <c=…>/<br> markup).
static const char* kItemArmor = R"ITEM({"name":"Zojja's Breastplate","description":"<c=@flavor>Crafted in the style of the renowned asuran genius, Zojja.</c>","type":"Armor","level":80,"rarity":"Ascended","vendor_value":240,"default_skin":107,"game_types":["Activity","Wvw","Dungeon","Pve"],"flags":["HideSuffix","AccountBound","AccountBindOnUse"],"restrictions":[],"id":48073,"chat_link":"[&AgHJuwAA]","icon":"https://render.guildwars2.com/file/64725E0CEBDA22C75C16B70B0CDE58E4E6E7400A/699218.png","details":{"type":"Coat","weight_class":"Heavy","defense":381,"infusion_slots":[{"flags":["Infusion"]}],"attribute_adjustment":403.326,"infix_upgrade":{"id":161,"attributes":[{"attribute":"Power","modifier":141},{"attribute":"Precision","modifier":101},{"attribute":"CritDamage","modifier":101}]},"secondary_suffix_item_id":""}})ITEM";

static const char* kItemWeapon = R"ITEM({"name":"Judgment Trident","description":"<c=@flavor>\"Great for environmentally friendly sabotage.\"<br>—Environmental Activist Jenrys</c>","type":"Weapon","level":69,"rarity":"Masterwork","vendor_value":174,"default_skin":3899,"game_types":["Activity","Wvw","Dungeon","Pve"],"flags":["HideSuffix","NoSalvage","NoSell","SoulbindOnAcquire","SoulBindOnUse"],"restrictions":[],"id":30680,"chat_link":"[&AgHYdwAA]","icon":"https://render.guildwars2.com/file/195C62FCE31ECE47100E4020CE6C909AB69FEA9C/562000.png","details":{"type":"Trident","damage_type":"Physical","min_power":617,"max_power":681,"defense":0,"infusion_slots":[],"attribute_adjustment":451.792,"infix_upgrade":{"id":162,"attributes":[{"attribute":"Power","modifier":158},{"attribute":"Toughness","modifier":113},{"attribute":"Vitality","modifier":113}]},"suffix_item_id":24613,"secondary_suffix_item_id":""}})ITEM";

static const char* kItemRune = R"ITEM({"name":"Superior Rune of the Scholar","description":"<c=@abilitytype>Element: </c>Brilliance<br>Double-click to apply to a piece of armor.","type":"UpgradeComponent","level":60,"rarity":"Exotic","vendor_value":65,"game_types":["Activity","Wvw","Dungeon","Pve"],"flags":[],"restrictions":[],"id":24836,"chat_link":"[&AgEEYQAA]","icon":"https://render.guildwars2.com/file/4378ABC0415950DAC6A05C76920392D72E242EC2/220736.png","details":{"type":"Rune","flags":["HeavyArmor","LightArmor","MediumArmor"],"infusion_upgrade_flags":[],"bonuses":["+25 Power","+35 Ferocity","+50 Power","+65 Ferocity","+100 Power","+125 Ferocity"],"attribute_adjustment":0,"infix_upgrade":{"id":112,"attributes":[]},"suffix":"of the Scholar"}})ITEM";

static const char* kItemCoin = R"ITEM({"name":"Mystic Coin","description":"Coins are used to create high-level weapons at the Mystic Forge in Lion's Arch. \nPart of Zommoros's favorite trades.","type":"Trophy","level":0,"rarity":"Rare","vendor_value":50,"game_types":["Activity","Wvw","Dungeon","Pve"],"flags":["NoSalvage","NoSell"],"restrictions":[],"id":19976,"chat_link":"[&AgEITgAA]","icon":"https://render.guildwars2.com/file/AB0317DF5B0E1BA47436A5420248660765154C08/62864.png"})ITEM";

static bool LinesHave(const Decoder::ItemMeta& m, const char* want) {
    for (auto& l : m.lines) if (l == want) return true;
    return false;
}
static bool LinesContain(const Decoder::ItemMeta& m, const char* sub) {
    for (auto& l : m.lines) if (l.find(sub) != std::string::npos) return true;
    return false;
}

// Armour: defense, attributes (incl. CritDamage->Ferocity), infusion, weight,
// subtype, level, flavour text. Also doubles as the bound-on-acquire case.
static void test_item_armor_tooltip() {
    Decoder::ItemMeta m;
    CHECK(Decoder::ItemTraits::Parse(Bytes(kItemArmor), m));
    CHECK(m.rarity == DR_Ascended);
    CHECK(m.description == "Crafted in the style of the renowned asuran genius, Zojja.");
    CHECK(LinesHave(m, "Defense: 381"));
    CHECK(LinesHave(m, "+141 Power"));
    CHECK(LinesHave(m, "+101 Precision"));
    CHECK(LinesHave(m, "+101 Ferocity"));          // CritDamage -> Ferocity display mapping
    CHECK(LinesHave(m, "Unused Infusion Slot"));
    CHECK(LinesHave(m, "Heavy Armor"));
    CHECK(LinesHave(m, "Coat"));
    CHECK(LinesHave(m, "Required Level: 80"));
    CHECK(m.bound == DB_AccountOnAcquire);         // AccountBound flag
    CHECK(m.tradeable == false);                   // bound-on-acquire -> not tradeable
}

// Weapon: weapon strength present, NO defense line; flavour markup stripped clean.
static void test_item_weapon_tooltip() {
    Decoder::ItemMeta m;
    CHECK(Decoder::ItemTraits::Parse(Bytes(kItemWeapon), m));
    CHECK(LinesHave(m, "Weapon Strength: 617 - 681"));
    CHECK(!LinesContain(m, "Defense"));            // a weapon never gets a defense line
    CHECK(LinesHave(m, "+158 Power"));
    CHECK(m.description.rfind("\"Great for environmentally friendly sabotage.\"", 0) == 0);
    CHECK(m.description.find('<') == std::string::npos);   // <c=…>/<br> stripped
}

// Rune: details.bonuses appear verbatim (so a consumer renders them when it
// resolves the parent item's upgrade ids).
static void test_item_rune_bonuses() {
    Decoder::ItemMeta m;
    CHECK(Decoder::ItemTraits::Parse(Bytes(kItemRune), m));
    CHECK(LinesHave(m, "+25 Power"));
    CHECK(LinesHave(m, "+35 Ferocity"));
    CHECK(LinesHave(m, "+125 Ferocity"));
}

// NoSell must NOT gate tradeable (Mystic Coin: NoSell yet TP-traded).
static void test_item_nosell_tradeable() {
    Decoder::ItemMeta m;
    CHECK(Decoder::ItemTraits::Parse(Bytes(kItemCoin), m));
    CHECK(m.noSell == true);
    CHECK(m.tradeable == true);
}

// Disk cache v3: filename bumped (old shape refetched) and the new fields round-trip.
static void test_item_cache_roundtrip_v3() {
    CHECK(std::strcmp(Decoder::ItemTraits::FileName(), "iteminfo_v2.json") == 0);
    Decoder::ItemMeta in; in.name = "X"; in.rarity = DR_Exotic; in.description = "flav";
    in.lines = { "Defense: 100", "+10 Power" };
    nlohmann::json j = Decoder::ItemTraits::ToJson(in);
    Decoder::ItemMeta out; Decoder::ItemTraits::FromJson(j, out);
    CHECK(out.description == "flav");
    CHECK(out.lines.size() == 2 && out.lines[0] == "Defense: 100" && out.lines[1] == "+10 Power");
}

// Record mapping: a warm item resolve carries the tooltip lines as facts, bounded
// to the cap and to text[160], with description set.
static void test_service_item_tooltip() {
    using namespace PieUI::ChatLinks;
    std::vector<DecoderRecord> events;
    Decoder::DecoderService svc;
    svc.Initialize("",
        [&](const std::string&, std::vector<char>& out){ out.assign(kItemArmor, kItemArmor+std::strlen(kItemArmor)); return true; },
        [&](const DecoderRecord& r){ events.push_back(r); });
    DecoderRecord r;
    CHECK(svc.Resolve(LINK_ITEM, 48073, r) == DR_NotReady);
    for (int i=0;i<200 && events.empty();++i){ svc.Tick(); Decoder::DecoderService::SleepMs(5); }
    CHECK(events.size() == 1);
    const DecoderRecord& e = events[0];
    CHECK(e.status == DR_Resolved);
    CHECK(e.factCount > 0 && e.factCount <= 16);            // never overflows the cap
    bool defense = false;
    for (uint8_t i = 0; i < e.factCount; ++i) {
        CHECK(std::strlen(e.facts[i].text) < 160);          // every line fits text[160]
        if (std::strcmp(e.facts[i].text, "Defense: 381") == 0) defense = true;
    }
    CHECK(defense);
    CHECK(e.description[0] != '\0');                         // flavour text carried into the record
    svc.Shutdown();
}

// Subtype line wording: aquatic armour reads naturally, "Default" (unidentified /
// generic) is dropped, real slots pass through verbatim.
static Decoder::ItemMeta ParseItemType(const std::string& detailsType) {
    std::string j = "{\"name\":\"x\",\"details\":{\"type\":\"" + detailsType + "\"}}";
    Decoder::ItemMeta m; Decoder::ItemTraits::Parse(Bytes(j.c_str()), m); return m;
}
static void test_item_subtype_wording() {
    CHECK(LinesHave(ParseItemType("HelmAquatic"), "Aquatic Helm"));
    CHECK(!LinesContain(ParseItemType("HelmAquatic"), "HelmAquatic"));
    CHECK(LinesHave(ParseItemType("GauntletsAquatic"), "Aquatic Gauntlets"));  // generic *Aquatic rule
    CHECK(!LinesContain(ParseItemType("Default"), "Default"));                 // omitted, not a real slot
    CHECK(LinesHave(ParseItemType("Coat"), "Coat"));                           // verbatim
    CHECK(LinesHave(ParseItemType("Rifle"), "Rifle"));                         // verbatim
    CHECK(LinesHave(ParseItemType("Trident"), "Trident"));                     // aquatic *weapon* untouched
    CHECK(LinesHave(ParseItemType("Speargun"), "Speargun"));
}

static void test_skin_parse() {
    const char* json = R"({"name":"Mistforged Hero's","icon":"https://x/skin.png"})";
    Decoder::SkinMeta m;
    CHECK(Decoder::SkinTraits::Parse(Bytes(json), m));
    CHECK(m.name == "Mistforged Hero's");
    CHECK(m.icon == "https://x/skin.png");
}

static void test_skill_parse() {
    const char* json = R"({"name":"Fireball","icon":"https://x/fb.png",
        "description":"Lob a fireball.","facts":[
          {"type":"Range","text":"Range","value":1200},
          {"type":"Damage","text":"Damage","hit_count":3}]})";
    Decoder::SkillMeta m;
    CHECK(Decoder::SkillTraits::Parse(Bytes(json), m));
    CHECK(m.name == "Fireball");
    CHECK(m.description == "Lob a fireball.");
    CHECK(m.facts.size() == 2);
    CHECK(m.facts[0].text == "Range: 1200");
    CHECK(m.facts[1].text == "Damage (x3)");
}

// --- Wiki skill-name fallback -------------------------------------------------
// Real, untrimmed action=ask responses (wiki.guildwars2.com SMW) captured for
// skills the /v2/skills API 404s on. These exercise the markup cleanup against
// genuine wiki output, not invented HTML.
static const char* kWiki63440 = R"WIKI({"query":{"printrequests":[{"label":"","key":"","redi":"","typeid":"_wpg","mode":2},{"label":"Has canonical name","key":"Has_canonical_name","redi":"","typeid":"_txt","mode":1,"format":""},{"label":"Has game description","key":"Has_game_description","redi":"","typeid":"_txt","mode":1,"format":""},{"label":"Has skill facts","key":"Has_skill_facts","redi":"","typeid":"_txt","mode":1,"format":""}],"results":{"Open Access":{"printouts":{"Has canonical name":["Open Access"],"Has game description":["Click to allow anyone to mount your turtle in the passenger slot."],"Has skill facts":[]},"fulltext":"Open Access","fullurl":"//wiki.guildwars2.com/wiki/Open_Access","namespace":0,"exists":"1","displaytitle":""}},"serializer":"SMW\\Serializers\\QueryResultSerializer","version":2,"meta":{"hash":"4b8dba87a385aef2e3c2736b74e40df2","count":1,"offset":0,"source":"","time":"0.006392"}}})WIKI";

static const char* kWiki63475 = R"WIKI({"query":{"printrequests":[{"label":"","key":"","redi":"","typeid":"_wpg","mode":2},{"label":"Has canonical name","key":"Has_canonical_name","redi":"","typeid":"_txt","mode":1,"format":""},{"label":"Has game description","key":"Has_game_description","redi":"","typeid":"_txt","mode":1,"format":""},{"label":"Has skill facts","key":"Has_skill_facts","redi":"","typeid":"_txt","mode":1,"format":""}],"results":{"Slam":{"printouts":{"Has canonical name":["Slam"],"Has game description":["Leap and knock nearby foes back."],"Has skill facts":[":<span class=\"inline-icon effect\">[[File:Damage.png|20pxpx|link=Damage|]]</span>&nbsp;[[Damage|Damage]]: 8,180 <span style=\"color:gray\">(5.5)</span><sup><abbr title=\"(weapon strength) * 5.5 * Power / (target's Armor)\">?</abbr></sup>\n:<span class=\"inline-icon effect\">[[File:Crippled.png|20pxpx|link=Crippled|]]</span>&nbsp;[[Crippled|Cripple]]&nbsp;(6s): -50% [[Movement Speed]]\n:<span class=\"inline-icon effect\">[[File:Stun.png|20pxpx|link=Stun|]]</span>&nbsp;[[Stun|Bonus Defiance Damage]]: 2 seconds\n:<span class=\"inline-icon effect\">[[File:Number of targets.png|20px|link=Effect|]]</span>&nbsp;[[Number of Targets|Number of Targets]]: 10\n:<span class=\"inline-icon effect\">[[File:Evade.png|20px|link=Defiance Break|]]</span>&nbsp;<span class=\"hiddenlinks\" style=\"color: teal;\">[[Defiance Break|Defiance Break]]: 200</span>\n:<span class=\"inline-icon effect\">[[File:Range.png|20px|link=Range|]]</span>&nbsp;[[Range|Range]]: 600"]},"fulltext":"Slam","fullurl":"//wiki.guildwars2.com/wiki/Slam","namespace":0,"exists":"1","displaytitle":""}},"serializer":"SMW\\Serializers\\QueryResultSerializer","version":2,"meta":{"hash":"fb91ec07438e2810ea0c0e6511ea76e1","count":1,"offset":0,"source":"","time":"0.005416"}}})WIKI";

static const char* kWiki55536 = R"WIKI({"query":{"printrequests":[{"label":"","key":"","redi":"","typeid":"_wpg","mode":2},{"label":"Has canonical name","key":"Has_canonical_name","redi":"","typeid":"_txt","mode":1,"format":""},{"label":"Has game description","key":"Has_game_description","redi":"","typeid":"_txt","mode":1,"format":""},{"label":"Has skill facts","key":"Has_skill_facts","redi":"","typeid":"_txt","mode":1,"format":""}],"results":{"Blast":{"printouts":{"Has canonical name":["Blast"],"Has game description":["'''[[Engage]].'''&#32;While on land, leap into the air and breathe fire ahead of you. If in the air, descend toward the ground before attacking."],"Has skill facts":[":<span class=\"inline-icon effect\">[[File:Damage.png|20pxpx|link=Damage|]]</span>&nbsp;[[Damage|Damage]]&nbsp;(4x): 3,448 <span style=\"color:gray\">(4.22)</span><sup><abbr title=\"(weapon strength) * 4.22 * Power / (target's Armor)\">?</abbr></sup>\n:<span class=\"inline-icon effect\">[[File:Burning.png|20pxpx|link=Burning|]]</span><sub>5</sub>&nbsp;[[Burning|Burning]]&nbsp;(1s): 655 Damage\n:<span class=\"inline-icon effect\">[[File:Miscellaneous effect.png|20px|link=Effect|]]</span>&nbsp;Dismounts\n:<span class=\"inline-icon effect\">[[File:Combo.png|20pxpx|link=Combo|]]</span>&nbsp;[[Combo|Combo Field]]: [[Fire field|Fire]][[Category:Fire field skills]]"]},"fulltext":"Blast","fullurl":"//wiki.guildwars2.com/wiki/Blast","namespace":0,"exists":"1","displaytitle":""}},"serializer":"SMW\\Serializers\\QueryResultSerializer","version":2,"meta":{"hash":"7755fa28c01836f4a896b7df4da3cc6b","count":1,"offset":0,"source":"","time":"0.007057"}}})WIKI";

static bool FactsHave(const Decoder::SkillMeta& m, const char* want) {
    for (auto& f : m.facts) if (f.text == want) return true;
    return false;
}

// The collision filter: id 63440 also names item "Defender's Staff". The
// [[Has context::Skill]] constraint must keep ParseFallback on the skill page.
static void test_skill_fallback_collision() {
    Decoder::SkillMeta m;
    CHECK(Decoder::SkillTraits::ParseFallback(Bytes(kWiki63440), m));
    CHECK(m.name == "Open Access");                  // NOT "Defender's Staff"
    std::string u = Decoder::SkillTraits::FallbackUrl(63440);
    CHECK(u.find("63440") != std::string::npos);     // id present
    CHECK(u.find("Skill") != std::string::npos);     // [[Has context::Skill]] carried (url-encoded)
}

// Slam: canonical name (not the "(turtle)" page title), description, and the full
// fact set incl. Defiance Break kept as an ordinary text-only fact.
static void test_skill_fallback_facts() {
    Decoder::SkillMeta m;
    CHECK(Decoder::SkillTraits::ParseFallback(Bytes(kWiki63475), m));
    CHECK(m.name == "Slam");
    CHECK(m.description == "Leap and knock nearby foes back.");
    CHECK(FactsHave(m, "Damage: 8,180 (5.5)"));
    CHECK(FactsHave(m, "Cripple (6s): -50% Movement Speed"));
    CHECK(FactsHave(m, "Number of Targets: 10"));
    CHECK(FactsHave(m, "Defiance Break: 200"));      // kept as a normal fact, not special-cased
    CHECK(FactsHave(m, "Range: 600"));
    for (auto& f : m.facts) CHECK(f.icon.empty());   // wiki facts carry File: images -> text only
}

// Blast: bold/entity cleanup in the description and [[Category:…]] stripped from a
// fact (must NOT leak as "FireCategory:Fire field skills").
static void test_skill_fallback_markup() {
    Decoder::SkillMeta m;
    CHECK(Decoder::SkillTraits::ParseFallback(Bytes(kWiki55536), m));
    CHECK(m.name == "Blast");
    CHECK(m.description.rfind("Engage. While on land, leap into the air", 0) == 0);
    CHECK(FactsHave(m, "Combo Field: Fire"));
    CHECK(!FactsHave(m, "Combo Field: FireCategory:Fire field skills"));
}

// An empty result set serialises as [] (array). Must fail -> caller degrades to a
// plain [Skill], never invents a name.
static void test_skill_fallback_empty() {
    Decoder::SkillMeta m;
    CHECK(!Decoder::SkillTraits::ParseFallback(Bytes(R"({"query":{"results":[]}})"), m));
    CHECK(m.name.empty());
}

// AsyncResolver fallback wiring: a skill id whose primary /v2 Parse fails but whose
// wiki ParseFallback succeeds must end up Warm and emit a (id,true) completion.
static void test_async_skill_fallback_path() {
    using R = Decoder::AsyncResolver<Decoder::SkillTraits>;
    R res;
    res.Initialize("", [&](const std::string& url, std::vector<char>& out){
        if (url.find("api.guildwars2.com") != std::string::npos) {
            const char* j = R"({"text":"no such id"})";   // 404-style body: valid JSON, no "name"
            out.assign(j, j + std::strlen(j)); return true;
        }
        out.assign(kWiki63475, kWiki63475 + std::strlen(kWiki63475)); return true;  // wiki ask
    });
    using GS = Decoder::GetState;
    Decoder::SkillMeta m;
    CHECK(res.Get(63475, m) == GS::Pending);          // cold -> kicks primary fetch
    std::vector<std::pair<uint32_t,bool>> done;
    for (int i=0;i<200 && done.empty();++i){ res.DrainCompleted(done); R::SleepMs(5); }
    CHECK(done.size() == 1 && done[0].first == 63475);
    CHECK(done[0].second == true);                     // primary failed -> fallback resolved
    CHECK(res.Get(63475, m) == GS::Warm);
    CHECK(m.name == "Slam");                           // named via the wiki, not the API
    res.Shutdown();
}

// --- Defiance/breakbar enrichment for API-resolved skills --------------------
// Real wiki `Has skill facts` blobs. 6154 Overcharged Shot carries a breakbar the
// /v2 API lacks (note the wiki's own "sic" annotation embeds a stray "332" after
// the real value 232 — ParseDefiance must read 232). 5492 Fire Attunement has none.
static const char* kDefiance6154 = R"DEF({"query":{"results":{"Overcharged Shot":{"printouts":{"Has skill facts":[":<div class=\"gamemode pve\"><span class=\"inline-icon effect\">[[File:Damage.png|20pxpx|link=Damage|]]</span>&nbsp;[[Damage|Damage]]: 422 <span style=\"color:gray\">(1.0)</span><sup><abbr title=\"(weapon strength) * 1.0 * Power / (target's Armor)\">?</abbr></sup></div>\n:<div class=\"gamemode pvp wvw\"><span class=\"inline-icon effect\">[[File:Damage.png|20pxpx|link=Damage|]]</span>&nbsp;[[Damage|Damage]]: 4 <span style=\"color:gray\">(0.01)</span><sup><abbr title=\"(weapon strength) * 0.01 * Power / (target's Armor)\">?</abbr></sup></div>\n:<span class=\"inline-icon effect\">[[File:Evade.png|20px|link=Defiance Break|]]</span>&nbsp;<span class=\"hiddenlinks\" style=\"color: teal;\">[[Defiance Break|Defiance Break]]: 232</span><span style=\"margin: 0em 0.1em; font-size: 85%; -webkit-user-select: none; -moz-user-select: none; -ms-user-select: none; user-select: none;\">&#0091;''[[wikipedia:sic|<span style=\"cursor: help; border-bottom: 1px dotted silver;\" title=\"332\">sic</span>]]''&#0093;</span>[[Category:Text errors]][[Category:Skills with incorrect defiance break tooltips]]"]},"fulltext":"Overcharged Shot","fullurl":"//wiki.guildwars2.com/wiki/Overcharged_Shot","namespace":0,"exists":"1","displaytitle":""},"Overcharged Shot#WvW,PvP":{"printouts":{"Has skill facts":[":<span class=\"inline-icon effect\">[[File:Evade.png|20px|link=Defiance Break|]]</span>&nbsp;<span class=\"hiddenlinks\" style=\"color: teal;\">[[Defiance Break|Defiance Break]]: 232</span>"]},"fulltext":"Overcharged Shot#WvW,PvP","fullurl":"//wiki.guildwars2.com/wiki/Overcharged_Shot#WvW,PvP","namespace":0,"exists":"1","displaytitle":""}},"version":2}})DEF";

static const char* kDefiance5492 = R"DEF({"query":{"results":{"Fire Attunement":{"printouts":{"Has skill facts":[]},"fulltext":"Fire Attunement","fullurl":"//wiki.guildwars2.com/wiki/Fire_Attunement","namespace":0,"exists":"1","displaytitle":""}},"version":2}})DEF";

// API-resolved skill with a breakbar gains the "Defiance Break: N" line the API lacks.
static void test_skill_enrich_defiance() {
    CHECK(std::strcmp(Decoder::SkillTraits::FileName(), "skillinfo_v2.json") == 0);  // old cache re-resolved
    Decoder::SkillMeta m; m.name = "Overcharged Shot";
    CHECK(Decoder::SkillTraits::ParseEnrich(Bytes(kDefiance6154), m));
    CHECK(FactsHave(m, "Defiance Break: 232"));   // the stray "332" sic-note is ignored
}

// A skill with no breakbar gets no defiance fact (and never a "Defiance Break: 0").
static void test_skill_enrich_none() {
    Decoder::SkillMeta m; m.name = "Fire Attunement";
    CHECK(!Decoder::SkillTraits::ParseEnrich(Bytes(kDefiance5492), m));
    for (auto& f : m.facts) CHECK(f.text.rfind("Defiance Break", 0) != 0);
}

// EnrichUrl skips when a defiance fact already exists (wiki-fallback skills already
// carry it) — no redundant fetch — and ParseEnrich never doubles the line.
static void test_skill_enrich_guard() {
    Decoder::SkillMeta plain; plain.name = "Overcharged Shot";
    std::string u = Decoder::SkillTraits::EnrichUrl(6154, plain);
    CHECK(!u.empty());
    CHECK(u.find("6154") != std::string::npos);
    CHECK(u.find("Skill") != std::string::npos);

    Decoder::SkillMeta already; already.name = "Overcharged Shot";
    Decoder::SkillFactM sf; sf.text = "Defiance Break: 232"; already.facts.push_back(sf);
    CHECK(Decoder::SkillTraits::EnrichUrl(6154, already).empty());            // already enriched -> skip
    CHECK(!Decoder::SkillTraits::ParseEnrich(Bytes(kDefiance6154), already)); // and won't double
    int n = 0; for (auto& f : already.facts) if (f.text.rfind("Defiance Break", 0) == 0) ++n;
    CHECK(n == 1);
}

// AsyncResolver two-phase: the API resolve emits first (name, no defiance), then the
// enrichment lands a SECOND completion and the warm record carries the breakbar.
static void test_async_skill_enrich_path() {
    using R = Decoder::AsyncResolver<Decoder::SkillTraits>;
    R res;
    res.Initialize("", [&](const std::string& url, std::vector<char>& out){
        if (url.find("api.guildwars2.com") != std::string::npos) {
            const char* j = R"({"name":"Overcharged Shot","description":"d","facts":[{"type":"Damage","text":"Damage","hit_count":1}]})";
            out.assign(j, j+std::strlen(j)); return true;          // API: resolves, no breakbar
        }
        out.assign(kDefiance6154, kDefiance6154+std::strlen(kDefiance6154)); return true;  // wiki enrich
    });
    using GS = Decoder::GetState;
    Decoder::SkillMeta m;
    CHECK(res.Get(6154, m) == GS::Pending);
    std::vector<std::pair<uint32_t,bool>> done;
    for (int i=0;i<400 && done.size()<2;++i){ res.DrainCompleted(done); R::SleepMs(5); }
    CHECK(done.size() == 2);                                        // API resolve + enrichment re-emit
    CHECK(done[0].second == true && done[1].second == true);
    CHECK(res.Get(6154, m) == GS::Warm);
    CHECK(m.name == "Overcharged Shot");
    CHECK(FactsHave(m, "Defiance Break: 232"));                     // breakbar in the warm record
    res.Shutdown();
}

static void test_price_cache() {
    Decoder::PriceCache pc;
    pc.Initialize([](const std::string& url, std::vector<char>& out){
        const char* j = R"({"buys":{"unit_price":100},"sells":{"unit_price":150}})";
        out.assign(j, j + std::strlen(j)); return true;
    });
    DecoderPrice p;
    CHECK(pc.Get(24, p) == false);   // cold -> kicks fetch
    std::vector<uint32_t> done;
    for (int i=0;i<200 && done.empty();++i){ pc.DrainCompleted(done); Decoder::PriceCache::SleepMs(5); }
    CHECK(done.size()==1 && done[0]==24);
    CHECK(pc.Get(24, p) == true);    // warm
    CHECK(p.buy == 100); CHECK(p.sell == 150);
    pc.Shutdown();
}

static void test_service_end_to_end() {
    using namespace PieUI::ChatLinks;
    std::vector<DecoderRecord> events;       // captured completion sink
    bool failFirst = true;
    Decoder::DecoderService svc;
    svc.Initialize("",
        [&](const std::string& url, std::vector<char>& out){
            if (failFirst) { failFirst = false; return false; }      // first fetch fails
            const char* j = R"({"name":"Fireball","icon":"i","description":"d","facts":[]})";
            out.assign(j, j + std::strlen(j)); return true;
        },
        [&](const DecoderRecord& r){ events.push_back(r); });

    DecoderRecord r;
    // (a) cold skill query -> NotReady, no event yet.
    CHECK(svc.Resolve(LINK_SKILL, 5492, r) == DR_NotReady);
    // Pump until an event lands (first fetch fails -> DR_Failed event).
    for (int i=0;i<200 && events.empty();++i){ svc.Tick(); Decoder::DecoderService::SleepMs(5); }
    CHECK(events.size() == 1);
    CHECK(events[0].linkType == LINK_SKILL);
    CHECK(events[0].id == 5492);
    CHECK(events[0].status == DR_Failed);

    // (b) re-query after failure (cooldown bypassed) -> retried -> Resolved event.
    svc.SetFailCooldownSec(0);
    events.clear();
    CHECK(svc.Resolve(LINK_SKILL, 5492, r) == DR_NotReady);
    for (int i=0;i<200 && events.empty();++i){ svc.Tick(); Decoder::DecoderService::SleepMs(5); }
    CHECK(events.size() == 1);
    CHECK(events[0].status == DR_Resolved);
    CHECK(std::strcmp(events[0].name, "Fireball") == 0);

    // (c) warm query now returns data immediately, no new event.
    events.clear();
    CHECK(svc.Resolve(LINK_SKILL, 5492, r) == DR_Resolved);
    CHECK(std::strcmp(r.name, "Fireball") == 0);
    svc.Tick();
    CHECK(events.empty());

    // (d) offline build type resolves synchronously, never NotReady.
    DecodedBuildLink b{}; b.profession = PROF_MESMER; b.specs[2].spec_id = 59;
    DecoderRecord br;
    CHECK(svc.Resolve(LINK_BUILD, 0, EncodeBuild(b), br) == DR_Resolved);
    CHECK(std::strcmp(br.name, "Mirage Build") == 0);

    svc.Shutdown();
}

// Fix A — synchronous strand paths must surface as DR_Failed, never not-ready.
static void test_resolve_synchronous_failed_paths() {
    using namespace PieUI::ChatLinks;
    std::vector<DecoderRecord> events;
    std::atomic<int> fetches{0};
    Decoder::DecoderService svc;
    svc.Initialize("",
        [&](const std::string&, std::vector<char>&)->bool{ ++fetches; return false; },  // every fetch fails
        [&](const DecoderRecord& r){ events.push_back(r); });

    DecoderRecord r;
    // (1) Knowably-invalid id 0 -> synchronous DR_Failed, no fetch, no event EVER.
    CHECK(svc.Resolve(LINK_ITEM, 0, r) == DR_Failed);
    CHECK(r.status == DR_Failed);
    CHECK(r.linkType == LINK_ITEM && r.id == 0);          // correlation key still carried
    for (int i=0;i<20;++i){ svc.Tick(); Decoder::DecoderService::SleepMs(2); }
    CHECK(fetches.load() == 0);                            // id 0 never scheduled a fetch
    CHECK(events.empty());                                 // and was never left awaiting an event

    // Prime a real (async) failure for id 77, then confirm the in-cooldown re-query
    // resolves SYNCHRONOUSLY to failed rather than stranding on not-ready.
    CHECK(svc.Resolve(LINK_ITEM, 77, r) == DR_NotReady);  // kicks a doomed fetch
    for (int i=0;i<200 && events.empty();++i){ svc.Tick(); Decoder::DecoderService::SleepMs(5); }
    CHECK(events.size() == 1);
    CHECK(events[0].status == DR_Failed && events[0].id == 77);  // async terminal event landed

    // (2) Re-query 77 while still inside the (default) cooldown -> synchronous DR_Failed.
    int fetchesBefore = fetches.load();
    events.clear();
    CHECK(svc.Resolve(LINK_ITEM, 77, r) == DR_Failed);
    svc.Tick();
    CHECK(fetches.load() == fetchesBefore);               // no new fetch scheduled
    CHECK(events.empty());                                 // synchronous -> emitted nothing

    // (3) Once cooldown is bypassed, the same id is allowed to retry-fetch again.
    svc.SetFailCooldownSec(0);
    CHECK(svc.Resolve(LINK_ITEM, 77, r) == DR_NotReady);  // retry kicked
    for (int i=0;i<200 && events.empty();++i){ svc.Tick(); Decoder::DecoderService::SleepMs(5); }
    CHECK(events.size() == 1 && events[0].status == DR_Failed);  // retry ran and emitted again
    svc.Shutdown();
}

// Fix A — the core invariant: across resolved / async-failed(timeout) / invalid /
// in-cooldown, the number of terminal outcomes (synchronous answers + events) equals
// the number of requests issued. No request ends without exactly one terminal outcome.
static void test_terminal_outcome_invariant() {
    using namespace PieUI::ChatLinks;
    int events = 0;
    Decoder::DecoderService svc;
    // Succeed only for the item whose id ends in '8'. Everything else returns false —
    // exactly how a WinINet connect/receive TIMEOUT (or 404/malformed body) surfaces —
    // and must follow the failed path, never strand.
    svc.Initialize("",
        [&](const std::string& url, std::vector<char>& out)->bool{
            if (!url.empty() && url.back()=='8') {
                const char* j = R"({"name":"Sword","icon":"i"})";
                out.assign(j, j+std::strlen(j)); return true;
            }
            return false;   // models timeout / 404 / malformed -> failed
        },
        [&](const DecoderRecord&){ ++events; });

    int requests = 0, syncTerminal = 0;
    auto issue = [&](uint8_t t, uint32_t id)->DecoderStatus{
        DecoderRecord r; ++requests;
        DecoderStatus s = svc.Resolve(t, id, r);
        if (s == DR_Resolved || s == DR_Failed) ++syncTerminal;   // a terminal answer, synchronously
        return s;
    };

    CHECK(issue(LINK_ITEM, 0) == DR_Failed);     // invalid -> synchronous terminal
    CHECK(issue(LINK_ITEM, 8) == DR_NotReady);   // async success -> event later
    CHECK(issue(LINK_ITEM, 7) == DR_NotReady);   // async timeout/fail -> event later

    for (int i=0;i<400 && events<2;++i){ svc.Tick(); Decoder::DecoderService::SleepMs(5); }
    CHECK(events == 2);                            // both async requests reached a terminal event

    int before = events;
    CHECK(issue(LINK_ITEM, 7) == DR_Failed);      // in-cooldown re-query -> synchronous terminal
    svc.Tick();
    CHECK(events == before);                       // emitted nothing extra

    CHECK(requests == 4);
    CHECK(syncTerminal + events == requests);      // INVARIANT: exactly one terminal outcome per request
    svc.Shutdown();
}

// Provider-side lifetime guarantee for the unload/crash fix: tearing the service
// down while a fetch is IN FLIGHT must (a) block until that fetch returns — so no
// worker outlives the object and no completion fires into freed state — and (b)
// emit nothing afterwards. Also asserts the facade is safe to call post-teardown.
static void test_service_teardown_awaits_inflight_fetch() {
    using namespace PieUI::ChatLinks;
    std::atomic<bool> started{false}, release{false};
    std::atomic<int> events{0};

    Decoder::DecoderService svc;
    svc.Initialize("",
        [&](const std::string&, std::vector<char>&) -> bool {
            started = true;
            while (!release.load()) Decoder::DecoderService::SleepMs(2);   // hold the worker mid-fetch
            return false;                                                  // then fail (irrelevant here)
        },
        [&](const DecoderRecord&){ ++events; });

    DecoderRecord r;
    CHECK(svc.Resolve(LINK_ITEM, 99, r) == DR_NotReady);                   // kicks the (blocking) fetch
    for (int i=0;i<200 && !started.load();++i) Decoder::DecoderService::SleepMs(2);
    CHECK(started.load());                                                 // worker is now inside the fetch

    std::atomic<bool> shutdownDone{false};
    std::thread t([&]{ svc.Shutdown(); shutdownDone = true; });

    // Shutdown MUST NOT return while the fetch is held — it joins the worker.
    for (int i=0;i<25 && !shutdownDone.load();++i) Decoder::DecoderService::SleepMs(2);
    CHECK(!shutdownDone.load());                                           // proves teardown awaits in-flight fetch

    release = true;                                                       // let the fetch (and worker) finish
    t.join();
    CHECK(shutdownDone.load());
    CHECK(events == 0);                                                    // nothing emitted into freed state

    // Facade is safe to call after teardown: graceful DR_Failed, never a crash.
    CHECK(svc.Resolve(LINK_ITEM, 99, r) == DR_Failed);
}

int main() {
    test_price_cache();
    test_resolve_synchronous_failed_paths();
    test_terminal_outcome_invariant();
    test_service_teardown_awaits_inflight_fetch();
    test_abi_is_pod();
    test_offline_build_label();
    test_offline_waypoint();
    test_async_state_machine();
    test_async_resolve_deps();
    test_item_parse();
    test_item_rarity_values();
    test_item_rarity_cache_compat();
    test_service_item_rarity();
    test_item_armor_tooltip();
    test_item_weapon_tooltip();
    test_item_rune_bonuses();
    test_item_nosell_tradeable();
    test_item_cache_roundtrip_v3();
    test_service_item_tooltip();
    test_item_subtype_wording();
    test_skin_parse();
    test_skill_parse();
    test_skill_fallback_collision();
    test_skill_fallback_facts();
    test_skill_fallback_markup();
    test_skill_fallback_empty();
    test_async_skill_fallback_path();
    test_skill_enrich_defiance();
    test_skill_enrich_none();
    test_skill_enrich_guard();
    test_async_skill_enrich_path();
    test_service_end_to_end();
    std::printf(g_fail ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}

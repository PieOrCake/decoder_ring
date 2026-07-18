#include "resolve/DecoderService.h"
#include "resolve/AsyncResolver.h"
#include "resolve/ItemResolver.h"
#include "resolve/SkinResolver.h"
#include "resolve/SkillResolver.h"
#include "resolve/RecipeResolver.h"
#include "resolve/TraitResolver.h"
#include "resolve/PriceCache.h"
#include "resolve/OfflineResolve.h"
#include "resolve/RecordFill.h"
#include "chat/ChatLinks.h"
#include <vector>
#include <utility>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <memory>

namespace Decoder {
using namespace PieUI::ChatLinks;

void DecoderService::SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct DecoderService::Impl {
    struct LangSet {
        AsyncResolver<ItemTraits>   item;
        AsyncResolver<SkinTraits>   skin;
        AsyncResolver<SkillTraits>  skill;
        AsyncResolver<RecipeTraits> recipe;
        AsyncResolver<TraitTraits>  trait;
    };
    std::string dir;
    HttpFetch   fetch;
    int         failCooldown = 60;
    std::string activeLang = "en";
    std::unordered_map<std::string, std::unique_ptr<LangSet>> byLang;
    PriceCache      price;
    CompletionSink  sink;

    LangSet& Set(const std::string& lang) {
        auto it = byLang.find(lang);
        if (it != byLang.end()) return *it->second;
        auto s = std::make_unique<LangSet>();
        s->item.Initialize(dir, fetch, lang);   s->item.SetFailCooldownSec(failCooldown);
        s->skin.Initialize(dir, fetch, lang);   s->skin.SetFailCooldownSec(failCooldown);
        s->skill.Initialize(dir, fetch, lang);  s->skill.SetFailCooldownSec(failCooldown);
        s->recipe.Initialize(dir, fetch, lang); s->recipe.SetFailCooldownSec(failCooldown);
        s->trait.Initialize(dir, fetch, lang);  s->trait.SetFailCooldownSec(failCooldown);
        auto& ref = *s; byLang[lang] = std::move(s); return ref;
    }

    static void FillItem(DecoderRecord& r, const ItemMeta& m) {
        CopyField(r.name, m.name); CopyField(r.iconUrl, m.icon);
        r.bound = m.bound; r.noSell = m.noSell ? 1 : 0; r.tradeable = m.tradeable ? 1 : 0; r.vendorValue = m.vendorValue;
        r.rarity = m.rarity;
        // v3: full tooltip — flavour text + pre-formatted lines as icon-less facts,
        // bounded by CopyField and capped at the facts[] size (same as the skill path).
        CopyField(r.description, m.description);
        uint8_t n = (uint8_t)(m.lines.size() < 16 ? m.lines.size() : 16);
        for (uint8_t i = 0; i < n; ++i) { r.facts[i].icon[0] = '\0'; CopyField(r.facts[i].text, m.lines[i]); }
        r.factCount = n;
    }
    static void FillSkin(DecoderRecord& r, const SkinMeta& m) {
        CopyField(r.name, m.name); CopyField(r.iconUrl, m.icon);
    }
    static void FillSkill(DecoderRecord& r, const SkillMeta& m) {
        CopyField(r.name, m.name); CopyField(r.iconUrl, m.icon); CopyField(r.description, m.description);
        uint8_t n = (uint8_t)(m.facts.size() < 16 ? m.facts.size() : 16);
        for (uint8_t i = 0; i < n; ++i) { CopyField(r.facts[i].icon, m.facts[i].icon); CopyField(r.facts[i].text, m.facts[i].text); }
        r.factCount = n;
    }
    static void FillRecipe(DecoderRecord& r, const RecipeMeta& m) {
        CopyField(r.name, m.name); CopyField(r.iconUrl, m.icon);
        r.vendorValue = (int32_t)m.outputItemId;   // overloaded: recipe links carry output_item_id here
        uint8_t n = (uint8_t)(m.lines.size() < 16 ? m.lines.size() : 16);
        for (uint8_t i = 0; i < n; ++i) { r.facts[i].icon[0] = '\0'; CopyField(r.facts[i].text, m.lines[i]); }
        r.factCount = n;
    }
    static void FillTrait(DecoderRecord& r, const TraitMeta& m) {
        CopyField(r.name, m.name); CopyField(r.iconUrl, m.icon); CopyField(r.description, m.description);
        uint8_t n = (uint8_t)(m.facts.size() < 16 ? m.facts.size() : 16);
        for (uint8_t i = 0; i < n; ++i) { CopyField(r.facts[i].icon, m.facts[i].icon); CopyField(r.facts[i].text, m.facts[i].text); }
        r.factCount = n;
    }

    template <typename Res, typename Fill>
    void DrainOne(Res& res, uint8_t type, Fill fill) {
        std::vector<std::pair<uint32_t,bool>> done; res.DrainCompleted(done);
        for (auto& d : done) {
            DecoderRecord r;
            if (d.second) { typename Res::Meta m;
                if (res.Get(d.first, m) == GetState::Warm) { InitRecord(r, type, d.first, DR_Resolved); fill(r, m); sink(r); } }
            else { InitRecord(r, type, d.first, DR_Failed); sink(r); }
        }
    }
    void DrainSet(LangSet& s) {
        DrainOne(s.item,   LINK_ITEM,   [](DecoderRecord& r, const ItemMeta& m){ FillItem(r, m); });
        DrainOne(s.skin,   LINK_SKIN,   [](DecoderRecord& r, const SkinMeta& m){ FillSkin(r, m); });
        DrainOne(s.skill,  LINK_SKILL,  [](DecoderRecord& r, const SkillMeta& m){ FillSkill(r, m); });
        DrainOne(s.recipe, LINK_RECIPE, [](DecoderRecord& r, const RecipeMeta& m){ FillRecipe(r, m); });
        DrainOne(s.trait,  LINK_TRAIT,  [](DecoderRecord& r, const TraitMeta& m){ FillTrait(r, m); });
        s.item.Tick(); s.skin.Tick(); s.skill.Tick(); s.recipe.Tick(); s.trait.Tick();
    }
};

void DecoderService::Initialize(const std::string& dir, HttpFetch fetch, CompletionSink sink) {
    m_p = new Impl();
    m_p->dir = dir; m_p->fetch = fetch; m_p->sink = std::move(sink);
    m_p->price.Initialize(fetch);
    m_p->Set("en");   // eagerly create the default-language set
}
void DecoderService::Shutdown() {
    if (!m_p) return;
    for (auto& kv : m_p->byLang) {
        kv.second->item.Shutdown(); kv.second->skin.Shutdown();
        kv.second->skill.Shutdown(); kv.second->recipe.Shutdown(); kv.second->trait.Shutdown();
    }
    m_p->price.Shutdown();
    delete m_p; m_p = nullptr;
}
void DecoderService::SetFailCooldownSec(int s) {
    if (!m_p) return;
    m_p->failCooldown = s;
    for (auto& kv : m_p->byLang) {
        kv.second->item.SetFailCooldownSec(s); kv.second->skin.SetFailCooldownSec(s);
        kv.second->skill.SetFailCooldownSec(s); kv.second->recipe.SetFailCooldownSec(s); kv.second->trait.SetFailCooldownSec(s);
    }
}

void DecoderService::SetLanguage(const std::string& apiLang) {
    if (!m_p) return;
    m_p->activeLang = apiLang;
    m_p->Set(apiLang);   // ensure the set exists so it is ready to serve
}

DecoderStatus DecoderService::Resolve(uint8_t type, uint32_t id, const std::string& chatCode, DecoderRecord& out) {
    if (!m_p) { InitRecord(out, type, id, DR_Failed); return DR_Failed; }

    // Offline types resolve synchronously from compiled-in data.
    if (type == LINK_BUILD || type == LINK_MAP)
        return ResolveOffline(type, id, chatCode, out) ? DR_Resolved : DR_Failed;

    // Map the warm-lookup tri-state to a status. Pending is the ONLY not-ready
    // outcome and it guarantees a later completion event; Warm/Failed are terminal
    // here and now (Failed = knowably-invalid id or a known in-cooldown failure),
    // so no request leaves this function stranded on not-ready.
    if (type == LINK_ITEM) {
        ItemMeta m;
        switch (m_p->Set(m_p->activeLang).item.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillItem(out, m); return DR_Resolved;
            case GetState::Pending: InitRecord(out, type, id, DR_NotReady); return DR_NotReady;
            case GetState::Failed:  InitRecord(out, type, id, DR_Failed);   return DR_Failed;
        }
    }
    if (type == LINK_SKIN) {
        SkinMeta m;
        switch (m_p->Set(m_p->activeLang).skin.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillSkin(out, m); return DR_Resolved;
            case GetState::Pending: InitRecord(out, type, id, DR_NotReady); return DR_NotReady;
            case GetState::Failed:  InitRecord(out, type, id, DR_Failed);   return DR_Failed;
        }
    }
    if (type == LINK_SKILL) {
        SkillMeta m;
        switch (m_p->Set(m_p->activeLang).skill.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillSkill(out, m); return DR_Resolved;
            case GetState::Pending: InitRecord(out, type, id, DR_NotReady); return DR_NotReady;
            case GetState::Failed:  InitRecord(out, type, id, DR_Failed);   return DR_Failed;
        }
    }
    if (type == LINK_RECIPE) {
        RecipeMeta m;
        switch (m_p->Set(m_p->activeLang).recipe.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillRecipe(out, m); return DR_Resolved;
            case GetState::Pending: InitRecord(out, type, id, DR_NotReady); return DR_NotReady;
            case GetState::Failed:  InitRecord(out, type, id, DR_Failed);   return DR_Failed;
        }
    }
    if (type == LINK_TRAIT) {
        TraitMeta m;
        switch (m_p->Set(m_p->activeLang).trait.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillTrait(out, m); return DR_Resolved;
            case GetState::Pending: InitRecord(out, type, id, DR_NotReady); return DR_NotReady;
            case GetState::Failed:  InitRecord(out, type, id, DR_Failed);   return DR_Failed;
        }
    }

    InitRecord(out, type, id, DR_Failed);
    return DR_Failed;   // unsupported type
}

DecoderStatus DecoderService::QueryPrice(uint32_t itemId, DecoderPrice& out) {
    if (!m_p) return DR_Failed;
    return m_p->price.Get(itemId, out) ? DR_Resolved : DR_NotReady;
}

void DecoderService::Tick() {
    if (!m_p) return;
    // Snapshot language sets before draining to avoid iterator invalidation on reentrant byLang insertion.
    std::vector<Impl::LangSet*> sets;
    sets.reserve(m_p->byLang.size());
    for (auto& kv : m_p->byLang) sets.push_back(kv.second.get());
    for (auto* s : sets) m_p->DrainSet(*s);
    // Price completions don't carry a durable record; consumers re-query QueryPrice.
    std::vector<uint32_t> pdone; m_p->price.DrainCompleted(pdone);
}
}

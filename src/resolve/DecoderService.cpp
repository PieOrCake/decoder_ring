#include "resolve/DecoderService.h"
#include "resolve/AsyncResolver.h"
#include "resolve/ItemResolver.h"
#include "resolve/SkinResolver.h"
#include "resolve/SkillResolver.h"
#include "resolve/PriceCache.h"
#include "resolve/OfflineResolve.h"
#include "resolve/RecordFill.h"
#include "chat/ChatLinks.h"
#include <vector>
#include <utility>
#include <thread>
#include <chrono>

namespace Decoder {
using namespace PieUI::ChatLinks;

void DecoderService::SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct DecoderService::Impl {
    AsyncResolver<ItemTraits>  item;
    AsyncResolver<SkinTraits>  skin;
    AsyncResolver<SkillTraits> skill;
    PriceCache                 price;
    CompletionSink             sink;

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

    // Build a resolved record from warm meta for emission.
    void EmitResolved(uint8_t type, uint32_t id) {
        DecoderRecord r;
        if (type == LINK_ITEM)       { ItemMeta m;  if (item.Get(id, m)  == GetState::Warm) { InitRecord(r, type, id, DR_Resolved); FillItem(r, m);  sink(r); } }
        else if (type == LINK_SKIN)  { SkinMeta m;  if (skin.Get(id, m)  == GetState::Warm) { InitRecord(r, type, id, DR_Resolved); FillSkin(r, m);  sink(r); } }
        else if (type == LINK_SKILL) { SkillMeta m; if (skill.Get(id, m) == GetState::Warm) { InitRecord(r, type, id, DR_Resolved); FillSkill(r, m); sink(r); } }
    }
    void EmitFailed(uint8_t type, uint32_t id) {
        DecoderRecord r; InitRecord(r, type, id, DR_Failed); sink(r);
    }
    template <typename Resolver>
    void DrainResolver(Resolver& res, uint8_t type) {
        std::vector<std::pair<uint32_t,bool>> done;
        res.DrainCompleted(done);
        for (auto& d : done) { if (d.second) EmitResolved(type, d.first); else EmitFailed(type, d.first); }
    }
};

void DecoderService::Initialize(const std::string& dir, HttpFetch fetch, CompletionSink sink) {
    m_p = new Impl();
    m_p->sink = std::move(sink);
    m_p->item.Initialize(dir, fetch);
    m_p->skin.Initialize(dir, fetch);
    m_p->skill.Initialize(dir, fetch);
    m_p->price.Initialize(fetch);
}
void DecoderService::Shutdown() {
    if (!m_p) return;
    m_p->item.Shutdown(); m_p->skin.Shutdown(); m_p->skill.Shutdown(); m_p->price.Shutdown();
    delete m_p; m_p = nullptr;
}
void DecoderService::SetFailCooldownSec(int s) {
    if (!m_p) return;
    m_p->item.SetFailCooldownSec(s); m_p->skin.SetFailCooldownSec(s); m_p->skill.SetFailCooldownSec(s);
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
        switch (m_p->item.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillItem(out, m); return DR_Resolved;
            case GetState::Pending: InitRecord(out, type, id, DR_NotReady); return DR_NotReady;
            case GetState::Failed:  InitRecord(out, type, id, DR_Failed);   return DR_Failed;
        }
    }
    if (type == LINK_SKIN) {
        SkinMeta m;
        switch (m_p->skin.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillSkin(out, m); return DR_Resolved;
            case GetState::Pending: InitRecord(out, type, id, DR_NotReady); return DR_NotReady;
            case GetState::Failed:  InitRecord(out, type, id, DR_Failed);   return DR_Failed;
        }
    }
    if (type == LINK_SKILL) {
        SkillMeta m;
        switch (m_p->skill.Get(id, m)) {
            case GetState::Warm:    InitRecord(out, type, id, DR_Resolved); Impl::FillSkill(out, m); return DR_Resolved;
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
    m_p->DrainResolver(m_p->item, LINK_ITEM);
    m_p->DrainResolver(m_p->skin, LINK_SKIN);
    m_p->DrainResolver(m_p->skill, LINK_SKILL);
    m_p->item.Tick(); m_p->skin.Tick(); m_p->skill.Tick();
    // Price completions don't carry a durable record; consumers re-query QueryPrice.
    std::vector<uint32_t> pdone; m_p->price.DrainCompleted(pdone);
}
}

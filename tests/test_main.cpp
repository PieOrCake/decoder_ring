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
    // No disk in this test.
    static const char* FileName() { return ""; }
    static nlohmann::json ToJson(const Meta& m) { return m.value; }
    static void FromJson(const nlohmann::json& j, Meta& m) { if (j.is_string()) m.value = j.get<std::string>(); }
};

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

    FakeMeta m;
    // Cold query -> NotReady (kicks fetch).
    CHECK(res.Get(42, m) == false);
    // Drive the worker to completion (poll the completed queue).
    std::vector<std::pair<uint32_t,bool>> done;
    for (int i = 0; i < 200 && done.empty(); ++i) { res.DrainCompleted(done); R::SleepMs(5); }
    CHECK(done.size() == 1);
    CHECK(done[0].first == 42);
    CHECK(done[0].second == false);            // first fetch failed
    CHECK(res.Get(42, m) == false);            // still not warm (in fail cooldown)

    // Re-query with cooldown bypassed -> retried, this time succeeds.
    res.SetFailCooldownSec(0);
    CHECK(res.Get(42, m) == false);            // kicks retry
    done.clear();
    for (int i = 0; i < 200 && done.empty(); ++i) { res.DrainCompleted(done); R::SleepMs(5); }
    CHECK(done.size() == 1);
    CHECK(done[0].second == true);             // retry succeeded
    CHECK(res.Get(42, m) == true);             // now warm
    CHECK(m.value == "OK:fake://42");
    res.Shutdown();
}

static std::vector<char> Bytes(const char* s) { return std::vector<char>(s, s + std::strlen(s)); }

static void test_item_parse() {
    const char* json = R"({"name":"Berserker's Sword","icon":"https://x/sword.png",
        "vendor_value":33,"flags":["AccountBound","NoSell"],"type":"Weapon",
        "details":{"type":"Sword"}})";
    Decoder::ItemMeta m;
    CHECK(Decoder::ItemTraits::Parse(Bytes(json), m));
    CHECK(m.name == "Berserker's Sword");
    CHECK(m.icon == "https://x/sword.png");
    CHECK(m.vendorValue == 33);
    CHECK(m.noSell == true);
    CHECK(m.bound == DB_AccountOnAcquire);
    CHECK(m.tradeable == false);   // account-bound-on-acquire => not tradeable
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
    test_service_teardown_awaits_inflight_fetch();
    test_abi_is_pod();
    test_offline_build_label();
    test_offline_waypoint();
    test_async_state_machine();
    test_item_parse();
    test_skin_parse();
    test_skill_parse();
    test_service_end_to_end();
    std::printf(g_fail ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}

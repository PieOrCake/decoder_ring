// Decoder Ring host test suite. Pure logic only — no Nexus, no Win32, no real HTTP.
#include "DecoderRingApi.h"
#include "resolve/OfflineResolve.h"
#include "chat/ChatLinks.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
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
    bool ok = Decoder::ResolveOffline(LINK_BUILD, link, r);
    CHECK(ok);
    CHECK(r.status == DR_Resolved);
    CHECK(std::strcmp(r.name, "Mirage Build") == 0);

    // Core profession fallback when no elite spec in the 3rd slot.
    DecodedBuildLink c{}; c.profession = PROF_MESMER;
    DecoderRecord r2{};
    CHECK(Decoder::ResolveOffline(LINK_BUILD, EncodeBuild(c), r2));
    CHECK(std::strcmp(r2.name, "Mesmer Build") == 0);
}

static void test_offline_waypoint() {
    using namespace PieUI::ChatLinks;
    // Find any id in [1,200] that resolves from the compiled-in waypoint table.
    DecoderRecord r{};
    bool any = false;
    for (uint32_t id = 1; id <= 200 && !any; ++id) {
        DecoderRecord t{};
        if (Decoder::ResolveOffline(LINK_MAP, EncodeMap(id), t) && t.status == DR_Resolved) {
            any = true; r = t;
        }
    }
    CHECK(any);                 // at least one id in [1,200] resolves from the table
    CHECK(r.name[0] != '\0');   // resolved waypoint has a name
}

int main() {
    test_abi_is_pod();
    test_offline_build_label();
    test_offline_waypoint();
    std::printf(g_fail ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}

# Decoder Ring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone Nexus addon (Windows DLL) that resolves GW2 chat-link IDs into a versioned metadata record and serves it to other addons via an exported function table (Nexus DataLink) plus a miss-completion event.

**Architecture:** A pure, host-testable core (vendored `ChatLinks` codec + offline resolvers + a generic async resolver with an *injectable* HTTP fetch seam + a `DecoderService` facade parameterised by a fetch function and a completion-sink callback) wrapped by a thin Nexus glue layer (`dllmain.cpp`) that publishes the API via `DataLink_Share`, runs the deadlock-free ready handshake, and pumps completions on a render callback that draws nothing.

**Tech Stack:** C++17, MinGW cross-compile to Windows DLL, CMake, WinINet (production HTTP, behind a seam), nlohmann/json, g++ host-test target.

**Canonical upstream (read-only, never edit):** `/home/tony/Dev/pie_ui/` — codec in `src/chat/`, patterns in `ItemInfoCache.cpp`, `SkillInfoCache.cpp`, `AlterEgoBridge.cpp`, `dllmain.cpp`, Nexus headers in `include/`.

**Design spec:** `plans/2026-06-10-decoder-ring-design.md`.

---

## File Structure

```
decoder_ring/
  CMakeLists.txt                 # MinGW DLL + host-test option (DECODER_TESTS_ONLY)
  DecoderRing.def                # EXPORTS GetAddonDef
  include/
    nexus/Nexus.h                # copied from pie_ui (read-only vendor)
    nlohmann/json.hpp            # copied from pie_ui
  public/
    DecoderRingApi.h             # CONSUMER-FACING ABI: POD record, status, price, api table,
                                 #   event names, DataLink id, apiVersion. Pure, no Nexus.
  src/
    AddonShared.h                # APIDefs global + Log helper
    chat/                        # VENDORED byte-identical from pie_ui/src/chat (no fork)
      ChatLinks.h ChatLinks.cpp SpecData.h
      WaypointNames.h WaypointNames.cpp WaypointTable.h
    resolve/
      Http.h                     # HttpFetch typedef (the injectable seam)
      Http_WinINet.cpp           # production fetch (excluded from host tests)
      AsyncResolver.h            # generic worker+cache+pending+fail+disk core (header-only template)
      ItemResolver.h ItemResolver.cpp   # /v2/items parse -> ItemMeta
      SkinResolver.h SkinResolver.cpp   # /v2/skins  parse -> SkinMeta
      SkillResolver.h SkillResolver.cpp # /v2/skills parse -> SkillMeta (+ FormatFact)
      PriceCache.h PriceCache.cpp       # memory-only TTL price
      OfflineResolve.h OfflineResolve.cpp # build/AE2 label + waypoint -> record fields (pure)
      RecordFill.h RecordFill.cpp       # safe fixed-buffer copy + Meta->DecoderRecord mapping
      DecoderService.h DecoderService.cpp # facade: Resolve/QueryPrice/Tick + completion sink
    dllmain.cpp                  # Nexus lifecycle, DataLink publish, handshake, render-pump
  tools/
    gen_waypoints.py             # vendored generator (regen on CONTENT patch)
  tests/
    test_main.cpp                # host unit + end-to-end self-test (fake fetch, capturing sink)
    isolation_main.cpp           # codec isolation proof
  docs/
    API.md                       # integration guide + graceful-degradation contract
  README.md
```

---

## Task 0: Project scaffold + build system

**Files:**
- Create: `CMakeLists.txt`, `DecoderRing.def`, `src/AddonShared.h`
- Create (copy): `include/nexus/Nexus.h`, `include/nlohmann/json.hpp`

- [ ] **Step 1: Copy the vendored third-party headers**

```bash
mkdir -p include/nexus include/nlohmann
cp /home/tony/Dev/pie_ui/include/nexus/Nexus.h include/nexus/Nexus.h
cp /home/tony/Dev/pie_ui/lib/nlohmann/json.hpp include/nlohmann/json.hpp
```

Expected: both files exist. (If `pie_ui/lib/nlohmann/json.hpp` is absent, find it: `find /home/tony/Dev/pie_ui -name json.hpp`.)

- [ ] **Step 2: Create `src/AddonShared.h`**

```cpp
#pragma once
#include "nexus/Nexus.h"

// Single global handle to the Nexus API, set in AddonLoad. Nexus-coupled code
// only. The pure resolve/ core never includes this file.
extern AddonAPI_t* APIDefs;

namespace Decoder {
    inline void Log(ELogLevel lvl, const char* msg) {
        if (APIDefs && APIDefs->Log) APIDefs->Log(lvl, "Decoder Ring", msg);
    }
}
```

- [ ] **Step 3: Create `DecoderRing.def`**

```
EXPORTS
GetAddonDef
```

- [ ] **Step 4: Create `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)

# Host-test target uses the system g++; the DLL target cross-compiles with MinGW.
option(DECODER_TESTS_ONLY "Build only the host unit-test target" OFF)

if(NOT DECODER_TESTS_ONLY)
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
    set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
    set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
endif()

project(DecoderRing CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/public
                    ${CMAKE_SOURCE_DIR}/src)

# Pure core sources shared by BOTH the DLL and the host tests (no Nexus, no Win32).
set(CORE_SOURCES
    src/chat/ChatLinks.cpp
    src/chat/WaypointNames.cpp
    src/resolve/ItemResolver.cpp
    src/resolve/SkinResolver.cpp
    src/resolve/SkillResolver.cpp
    src/resolve/PriceCache.cpp
    src/resolve/OfflineResolve.cpp
    src/resolve/RecordFill.cpp
    src/resolve/DecoderService.cpp)

if(DECODER_TESTS_ONLY)
    add_executable(decoder_tests tests/test_main.cpp ${CORE_SOURCES})
    target_compile_definitions(decoder_tests PRIVATE DECODER_HOSTTEST)

    add_executable(chatlinks_isolation tests/isolation_main.cpp src/chat/ChatLinks.cpp)
    target_include_directories(chatlinks_isolation PRIVATE src)
    return()
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_SHARED_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++ -DWIN32_LEAN_AND_MEAN")

add_library(DecoderRing SHARED
    src/dllmain.cpp
    src/resolve/Http_WinINet.cpp
    ${CORE_SOURCES})
set_target_properties(DecoderRing PROPERTIES SUFFIX ".dll" PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(DecoderRing wininet)
target_link_options(DecoderRing PRIVATE -Wl,--strip-all)
target_sources(DecoderRing PRIVATE ${CMAKE_SOURCE_DIR}/DecoderRing.def)
```

- [ ] **Step 5: Verify both build configs *configure*** (sources don't exist yet, so do not build)

Run:
```bash
mkdir -p build-host && cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. && cd ..
```
Expected: CMake configures without error (it does not compile yet — that needs later tasks). If `x86_64-w64-mingw32-g++` is missing, note it for the DLL build but the host config still succeeds.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt DecoderRing.def src/AddonShared.h include/
git commit -m "Scaffold build system and vendored third-party headers"
```

---

## Task 1: Vendor the codec + waypoint table (byte-identical, no fork)

**Files:**
- Create (copy): `src/chat/ChatLinks.h`, `src/chat/ChatLinks.cpp`, `src/chat/SpecData.h`,
  `src/chat/WaypointNames.h`, `src/chat/WaypointNames.cpp`, `src/chat/WaypointTable.h`,
  `tools/gen_waypoints.py`
- Create: `tests/isolation_main.cpp`

- [ ] **Step 1: Copy the vendored files byte-for-byte**

```bash
mkdir -p src/chat tools
cp /home/tony/Dev/pie_ui/src/chat/ChatLinks.h      src/chat/ChatLinks.h
cp /home/tony/Dev/pie_ui/src/chat/ChatLinks.cpp    src/chat/ChatLinks.cpp
cp /home/tony/Dev/pie_ui/src/chat/SpecData.h       src/chat/SpecData.h
cp /home/tony/Dev/pie_ui/src/chat/WaypointNames.h  src/chat/WaypointNames.h
cp /home/tony/Dev/pie_ui/src/chat/WaypointNames.cpp src/chat/WaypointNames.cpp
cp /home/tony/Dev/pie_ui/src/chat/WaypointTable.h  src/chat/WaypointTable.h
cp /home/tony/Dev/pie_ui/tools/gen_waypoints.py    tools/gen_waypoints.py
```

- [ ] **Step 2: Verify byte-identical (the no-fork discipline is a hard requirement)**

Run:
```bash
for f in ChatLinks.h ChatLinks.cpp SpecData.h WaypointNames.h WaypointNames.cpp WaypointTable.h; do
  diff -q /home/tony/Dev/pie_ui/src/chat/$f src/chat/$f || echo "DRIFT: $f";
done
```
Expected: no output (all identical). Any `DRIFT:` line is a failure — re-copy.

- [ ] **Step 3: Create `tests/isolation_main.cpp`** (proves the codec links with no other TU)

```cpp
// Isolation proof: this TU + ChatLinks.cpp are the ONLY sources linked.
// If it builds and links, the vendored codec has no hidden dependency.
#include "chat/ChatLinks.h"
#include <cstdio>

int main() {
    using namespace PieUI::ChatLinks;
    std::string skin = EncodeSkin(8585);
    bool ok = DetectType(skin) == LINK_SKIN;
    auto segs = SegmentLine("hello " + skin);
    ok = ok && !segs.empty();
    std::printf("chatlinks isolation: %s (%zu segments)\n", ok ? "OK" : "FAIL", segs.size());
    return ok ? 0 : 1;
}
```

- [ ] **Step 4: Build and run the isolation test**

Run:
```bash
cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make chatlinks_isolation 2>&1 | tail -5 && ./chatlinks_isolation; cd ..
```
Expected: `chatlinks isolation: OK (2 segments)` and exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/chat/ tools/gen_waypoints.py tests/isolation_main.cpp
git commit -m "Vendor ChatLinks codec + waypoint table (byte-identical, no-fork)"
```

---

## Task 2: Public ABI header (the consumer contract)

**Files:**
- Create: `public/DecoderRingApi.h`
- Test: `tests/test_main.cpp` (start the file here)

- [ ] **Step 1: Write the failing test** (create `tests/test_main.cpp`)

```cpp
// Decoder Ring host test suite. Pure logic only — no Nexus, no Win32, no real HTTP.
#include "DecoderRingApi.h"
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

int main() {
    test_abi_is_pod();
    std::printf(g_fail ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 2: Run it to verify it fails to compile** (header missing)

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make decoder_tests 2>&1 | tail -5; cd ..`
Expected: FAIL — `DecoderRingApi.h: No such file or directory`.

- [ ] **Step 3: Create `public/DecoderRingApi.h`**

```cpp
// ============================================================================
// Decoder Ring — public ABI for consumer addons.
// ----------------------------------------------------------------------------
// This is the ONE file a consumer copies to integrate. It is pure: depends only
// on <cstdint> — no Nexus, no Win32, no STL containers across the boundary.
//
// Delivery is two coupled halves, both keyed by the (linkType, id) correlation
// tuple:
//   1. Warm query  — DataLink_Get(DECODER_RING_DATALINK) yields a DecoderRingApi*;
//      call Resolve(): warm -> Resolved+record, cold -> NotReady (fetch kicked).
//   2. Miss event  — subscribe EV_DECODER_RING_RESOLVED; payload is DecoderRecord*
//      (delivered synchronously — copy what you need during the handler).
//
// VERSIONING: schemaVersion is the FIRST field of DecoderRecord; apiVersion is
// the FIRST field of DecoderRingApi. A consumer MUST check both before use.
// ============================================================================
#pragma once
#include <cstdint>

// Bump on ANY change to DecoderRingApi or DecoderRecord layout/semantics.
#define DECODER_RING_API_VERSION   1u
// DataLink identifier the service publishes the DecoderRingApi struct under.
#define DECODER_RING_DATALINK      "DECODER_RING_API"
// Service announces its API is live (raised on load + in reply to a ping).
#define EV_DECODER_RING_READY      "EV_DECODER_RING_READY"
// A consumer pings to ask the service to (re-)announce readiness.
#define EV_DECODER_RING_PING       "EV_DECODER_RING_PING"
// A background resolution landed. Payload: DecoderRecord* (synchronous).
#define EV_DECODER_RING_RESOLVED   "EV_DECODER_RING_RESOLVED"

// Resolution status of a record / query result.
enum DecoderStatus : uint8_t {
    DR_NotReady = 0,  // not in warm cache; a background fetch was kicked (watch the event)
    DR_Resolved = 1,  // fully resolved; fields below are valid for this linkType
    DR_Failed   = 2,  // last fetch failed and is in cooldown; retryable on a later query
};

// Bound status for item links (mirrors GW2 item flags, first-match-wins order).
enum DecoderBound : uint8_t {
    DB_None = 0, DB_AccountOnAcquire = 1, DB_SoulOnAcquire = 2,
    DB_AccountOnUse = 3, DB_SoulOnUse = 4,
};

// POI kind for waypoint/map links.
enum DecoderPoiKind : uint8_t { DP_Waypoint = 0, DP_PointOfInterest = 1, DP_Vista = 2 };

// One pre-formatted skill tooltip fact: a render-service icon URL + a label.
struct DecoderFact {
    char icon[128];   // render-service icon URL ("" if none)
    char text[160];   // pre-formatted label, e.g. "Range: 1200", "Bleeding (8s)"
};

// Versioned, fixed-size POD metadata record. Trivially copyable; safe in shared
// memory and event payloads. Variable-length data is bounded (facts[16]).
struct DecoderRecord {
    uint16_t schemaVersion;   // == DECODER_RING_API_VERSION. FIRST FIELD. Always check.
    uint8_t  linkType;        // correlation key part 1: ChatLinkType byte (0x02 item, ...)
    uint8_t  status;          // DecoderStatus
    uint32_t id;              // correlation key part 2: resolved id

    char     name[128];       // display name / build label / waypoint name; "" if unresolved
    char     iconUrl[256];    // render-service icon URL; may be "" (consumer downloads it)

    // --- Item (linkType == 0x02) ---
    uint8_t  bound;           // DecoderBound
    uint8_t  noSell;          // 1 = cannot vendor to an NPC (does NOT imply untradeable)
    uint8_t  tradeable;       // 1 = eligible for the trading post
    uint8_t  _pad0;           // explicit padding (keep layout deterministic)
    int32_t  vendorValue;     // vendor sale value, copper

    // --- Skill (linkType == 0x06) ---
    char     description[512]; // skill description text
    uint8_t  factCount;        // number of valid entries in facts[]
    uint8_t  _pad1[3];
    DecoderFact facts[16];     // pre-formatted facts; entries past 16 are dropped

    // --- Waypoint / POI (linkType == 0x04) ---
    char     mapName[96];      // map the POI sits on
    uint8_t  poiType;          // DecoderPoiKind
    uint8_t  _pad2[3];

    // Build/AE2 (linkType == 0x0D): the spec label (e.g. "Mirage Build") is in name[].
};

// Volatile trading-post price. In-memory-only on the service, ~5-min TTL, never
// disk. Queried separately — NOT part of the durable DecoderRecord.
struct DecoderPrice {
    int32_t buy;   // highest buy order, copper; -1 = no listings that side
    int32_t sell;  // lowest sell listing, copper; -1 = no listings that side
};

// Exported function table, published in shared memory under DECODER_RING_DATALINK.
struct DecoderRingApi {
    uint32_t apiVersion;   // == DECODER_RING_API_VERSION. FIRST FIELD. Check before calling.
    // Warm resolve. Returns immediately; never blocks on the network.
    //   DR_Resolved -> *out filled from warm cache.
    //   DR_NotReady -> background fetch kicked (or already in flight); watch the event.
    //   DR_Failed   -> last fetch failed, still in cooldown (retryable later).
    DecoderStatus (*Resolve)(uint8_t linkType, uint32_t id, DecoderRecord* out);
    // Volatile price. DR_Resolved with *out filled, or DR_NotReady (fetch kicked).
    DecoderStatus (*QueryPrice)(uint32_t itemId, DecoderPrice* out);
};
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd build-host && make decoder_tests 2>&1 | tail -3 && ./decoder_tests; cd ..`
Expected: `ALL TESTS PASSED` and exit 0.

- [ ] **Step 5: Commit**

```bash
git add public/DecoderRingApi.h tests/test_main.cpp
git commit -m "Add public ABI header (versioned POD record + api table)"
```

---

## Task 3: Offline resolvers (build/AE2 label + waypoint) — pure, fully tested

**Files:**
- Create: `src/resolve/OfflineResolve.h`, `src/resolve/OfflineResolve.cpp`
- Create: `src/resolve/RecordFill.h`, `src/resolve/RecordFill.cpp`
- Modify: `tests/test_main.cpp`

- [ ] **Step 1: Write `src/resolve/RecordFill.h`** (safe fixed-buffer copy helper)

```cpp
#pragma once
#include "DecoderRingApi.h"
#include <string>

namespace Decoder {
// Copy src into a fixed char buffer, always NUL-terminated, truncating safely.
template <size_t N>
inline void CopyField(char (&dst)[N], const std::string& src) {
    size_t n = src.size() < (N - 1) ? src.size() : (N - 1);
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = '\0';
}
// Zero a record and stamp the common header (version, key, status).
void InitRecord(DecoderRecord& r, uint8_t linkType, uint32_t id, DecoderStatus status);
}
```

- [ ] **Step 2: Write `src/resolve/RecordFill.cpp`**

```cpp
#include "resolve/RecordFill.h"
#include <cstring>

namespace Decoder {
void InitRecord(DecoderRecord& r, uint8_t linkType, uint32_t id, DecoderStatus status) {
    std::memset(&r, 0, sizeof(r));
    r.schemaVersion = (uint16_t)DECODER_RING_API_VERSION;
    r.linkType = linkType;
    r.id = id;
    r.status = (uint8_t)status;
}
}
```

- [ ] **Step 3: Write the failing tests** (append to `tests/test_main.cpp` before `main`, and call them in `main`)

```cpp
#include "resolve/OfflineResolve.h"
#include "chat/ChatLinks.h"

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
    // Pick any id present in the vendored WaypointTable. If WaypointNames::Get
    // returns true, the record must carry the name + map and DP_Waypoint type.
    // (Use a known id from the table; e.g. Lion's Arch waypoints. If unsure,
    // iterate a few ids in the test until one resolves — table is compiled in.)
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
```

Add to `main()` before the print line:
```cpp
    test_offline_build_label();
    test_offline_waypoint();
```

- [ ] **Step 4: Run to verify it fails** (`OfflineResolve.h` missing)

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make decoder_tests 2>&1 | tail -5; cd ..`
Expected: FAIL — `OfflineResolve.h: No such file or directory`.

- [ ] **Step 5: Write `src/resolve/OfflineResolve.h`**

```cpp
#pragma once
#include "DecoderRingApi.h"
#include <string>

namespace Decoder {
// Resolve the OFFLINE link types (no network): build/AE2 spec label and
// waypoint/POI name. Fills `out` and returns true on success (out.status set to
// DR_Resolved). Returns false for non-offline types or undecodable input.
//   chatCode: the full "[&...]" chat link (for LINK_BUILD / LINK_MAP).
bool ResolveOffline(uint8_t linkType, const std::string& chatCode, DecoderRecord& out);
}
```

- [ ] **Step 6: Write `src/resolve/OfflineResolve.cpp`**

```cpp
#include "resolve/OfflineResolve.h"
#include "resolve/RecordFill.h"
#include "chat/ChatLinks.h"
#include "chat/SpecData.h"
#include "chat/WaypointNames.h"
#include <string>

namespace Decoder {
using namespace PieUI;
using namespace PieUI::ChatLinks;

namespace {
// "Mirage Build" / "Mesmer Build" from a decoded build link's profession + 3rd-slot spec.
std::string BuildLabel(const DecodedBuildLink& b) {
    const char* spec = SpecData::GetEliteSpecName(b.specs[2].spec_id);
    if (b.specs[2].spec_id != 0 && spec) return std::string(spec) + " Build";
    const char* prof = SpecData::GetProfessionName(b.profession);
    if (prof && prof[0]) return std::string(prof) + " Build";
    return "Build";
}
}

bool ResolveOffline(uint8_t linkType, const std::string& chatCode, DecoderRecord& out) {
    if (linkType == LINK_BUILD) {
        DecodedBuildLink b{};
        if (!DecodeBuild(chatCode, b)) return false;
        InitRecord(out, linkType, 0, DR_Resolved);
        CopyField(out.name, BuildLabel(b));
        return true;
    }
    if (linkType == LINK_MAP) {
        DecodedItemLink dummy; (void)dummy;
        // Map links carry the POI id as the primary id; pull it via SegmentLine.
        auto segs = SegmentLine(chatCode);
        uint32_t poiId = 0;
        for (auto& s : segs) if (s.kind == SegmentKind::Link && s.linkType == LINK_MAP) { poiId = s.primaryId; break; }
        if (poiId == 0) return false;
        std::string name, mapName; const char* typeLabel = nullptr;
        if (!WaypointNames::Get(poiId, name, mapName, &typeLabel)) return false;
        InitRecord(out, linkType, poiId, DR_Resolved);
        CopyField(out.name, name);
        CopyField(out.mapName, mapName);
        out.poiType = (typeLabel && std::string(typeLabel) == "Point of Interest") ? DP_PointOfInterest
                    : (typeLabel && std::string(typeLabel) == "Vista") ? DP_Vista : DP_Waypoint;
        return true;
    }
    return false;
}
}
```

> Note: AE2 build codes arrive as a code string, not a `[&...]` link. `ResolveOffline` handles `LINK_BUILD` from the embedded `[&...]`; the AE2 wrapper label ("… AE2 Build") is added in Task 7 where the service sees the original code. Keep this function focused on the two decodable offline types.

- [ ] **Step 7: Run to verify pass**

Run: `cd build-host && make decoder_tests 2>&1 | tail -3 && ./decoder_tests; cd ..`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 8: Commit**

```bash
git add src/resolve/OfflineResolve.* src/resolve/RecordFill.* tests/test_main.cpp
git commit -m "Add offline resolvers (build label + waypoint) with host tests"
```

---

## Task 4: HTTP seam + generic AsyncResolver core (host-tested with a fake fetch)

**Files:**
- Create: `src/resolve/Http.h`
- Create: `src/resolve/AsyncResolver.h` (header-only template)
- Modify: `tests/test_main.cpp`

- [ ] **Step 1: Write `src/resolve/Http.h`** (the injectable seam)

```cpp
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Decoder {
// Injectable HTTP GET. Returns true and fills `out` with the response body on
// success; false on any failure. Production wires WinINet (Http_WinINet.cpp);
// tests inject a fake. The pure core depends ONLY on this typedef.
using HttpFetch = std::function<bool(const std::string& url, std::vector<char>& out)>;
}
```

- [ ] **Step 2: Write the failing test** (append to `tests/test_main.cpp`; exercises warm/cold/failed/retry)

```cpp
#include "resolve/AsyncResolver.h"
#include <atomic>

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
```

Add `test_async_state_machine();` to `main()`.

- [ ] **Step 3: Run to verify it fails** (`AsyncResolver.h` missing)

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make decoder_tests 2>&1 | tail -5; cd ..`
Expected: FAIL — `AsyncResolver.h: No such file or directory`.

- [ ] **Step 4: Write `src/resolve/AsyncResolver.h`**

```cpp
#pragma once
#include "resolve/Http.h"
#include "nlohmann/json.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>

namespace Decoder {

// Generic async id->Meta resolver: a worker thread fetches via an injected
// HttpFetch, parses with Traits::Parse, never persists empties/failures, applies
// a fail cooldown, and (optionally) disk-caches via Traits::ToJson/FromJson.
//
// Traits must provide:
//   using Meta;                                  // the cached value type
//   static std::string Url(uint32_t id);
//   static bool Parse(const std::vector<char>& body, Meta&);
//   static const char* FileName();               // "" disables disk
//   static nlohmann::json ToJson(const Meta&);
//   static void FromJson(const nlohmann::json&, Meta&);
template <typename Traits>
class AsyncResolver {
public:
    using Meta = typename Traits::Meta;

    static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

    // dir: directory for the disk cache ("" disables disk). fetch: injected HTTP.
    void Initialize(const std::string& dir, HttpFetch fetch) {
        m_Fetch = std::move(fetch);
        m_Dir = dir;
        if (!m_Dir.empty() && Traits::FileName()[0]) LoadFromDisk();
        m_Stop = false;
        m_Worker = std::thread([this]{ Run(); });
    }

    void Shutdown() {
        m_Stop = true; m_CV.notify_all();
        if (m_Worker.joinable()) m_Worker.join();
        if (m_Dirty.exchange(false)) FlushToDisk();
    }

    void SetFailCooldownSec(int s) { m_FailCooldownSec = s; }

    // Warm -> copies into out, true. Cold/failed-in-cooldown -> kicks a fetch
    // (unless already pending or cooling down) and returns false.
    bool Get(uint32_t id, Meta& out) {
        if (id == 0) return false;
        std::lock_guard<std::mutex> lk(m_Mtx);
        auto it = m_Items.find(id);
        if (it != m_Items.end()) { out = it->second; return true; }
        if (m_Pending.count(id)) return false;
        auto fit = m_Fail.find(id);
        if (fit != m_Fail.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - fit->second).count();
            if (age < m_FailCooldownSec) return false;
        }
        m_Pending.insert(id);
        m_Queue.push_back(id);
        m_CV.notify_one();
        return false;
    }

    // Move completed (id, success) pairs out for the service to emit events from.
    void DrainCompleted(std::vector<std::pair<uint32_t,bool>>& out) {
        std::lock_guard<std::mutex> lk(m_Mtx);
        if (m_Completed.empty()) return;
        out.insert(out.end(), m_Completed.begin(), m_Completed.end());
        m_Completed.clear();
    }

    // Throttled disk flush; call from the main-thread pump.
    void Tick() {
        if (!m_Dirty.load() || m_Dir.empty() || !Traits::FileName()[0]) return;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastFlush).count() < 30000) return;
        m_LastFlush = now;
        if (m_Dirty.exchange(false)) FlushToDisk();
    }

private:
    std::string FilePath() const { return m_Dir + "/" + Traits::FileName(); }

    void LoadFromDisk() {
        std::ifstream f(FilePath());
        if (!f) return;
        try {
            nlohmann::json j; f >> j;
            if (j.is_object())
                for (auto it = j.begin(); it != j.end(); ++it) {
                    Meta m; Traits::FromJson(it.value(), m);
                    m_Items[(uint32_t)std::stoul(it.key())] = std::move(m);
                }
        } catch (...) {}
    }

    void FlushToDisk() {
        nlohmann::json j = nlohmann::json::object();
        { std::lock_guard<std::mutex> lk(m_Mtx);
          for (auto& kv : m_Items) j[std::to_string(kv.first)] = Traits::ToJson(kv.second); }
        std::ofstream f(FilePath(), std::ios::trunc);
        if (f) f << j.dump();
    }

    void Run() {
        for (;;) {
            uint32_t id = 0;
            { std::unique_lock<std::mutex> lk(m_Mtx);
              m_CV.wait(lk, [this]{ return m_Stop.load() || !m_Queue.empty(); });
              if (m_Stop.load()) return;
              id = m_Queue.back(); m_Queue.pop_back(); }
            std::vector<char> body; Meta meta;
            bool ok = m_Fetch && m_Fetch(Traits::Url(id), body) && Traits::Parse(body, meta);
            std::lock_guard<std::mutex> lk(m_Mtx);
            m_Pending.erase(id);
            if (ok) { m_Items[id] = std::move(meta); m_Fail.erase(id); m_Dirty = true; }   // never persist empties
            else    { m_Fail[id] = std::chrono::steady_clock::now(); }
            m_Completed.emplace_back(id, ok);
        }
    }

    HttpFetch m_Fetch;
    std::string m_Dir;
    std::unordered_map<uint32_t, Meta> m_Items;
    std::unordered_set<uint32_t> m_Pending;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> m_Fail;
    std::vector<uint32_t> m_Queue;
    std::vector<std::pair<uint32_t,bool>> m_Completed;
    std::mutex m_Mtx;
    std::condition_variable m_CV;
    std::atomic<bool> m_Stop{false};
    std::atomic<bool> m_Dirty{false};
    std::thread m_Worker;
    std::chrono::steady_clock::time_point m_LastFlush{std::chrono::steady_clock::now()};
    int m_FailCooldownSec = 60;
};
}
```

- [ ] **Step 5: Run to verify pass**

Run: `cd build-host && make decoder_tests 2>&1 | tail -3 && ./decoder_tests; cd ..`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add src/resolve/Http.h src/resolve/AsyncResolver.h tests/test_main.cpp
git commit -m "Add HTTP seam + generic AsyncResolver with state-machine tests"
```

---

## Task 5: Networked parsers — item / skin / skill

**Files:**
- Create: `src/resolve/ItemResolver.{h,cpp}`, `src/resolve/SkinResolver.{h,cpp}`, `src/resolve/SkillResolver.{h,cpp}`
- Modify: `tests/test_main.cpp`

Each file defines a Meta struct + a Traits providing `Url/Parse/FileName/ToJson/FromJson`. Parsers
are the *lean* distilled set from the record (much smaller than Pie UI's full caches).

- [ ] **Step 1: Write the failing parser tests** (append to `tests/test_main.cpp`)

```cpp
#include "resolve/ItemResolver.h"
#include "resolve/SkinResolver.h"
#include "resolve/SkillResolver.h"

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
```

Add the three calls to `main()`.

- [ ] **Step 2: Run to verify it fails** (headers missing)

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make decoder_tests 2>&1 | tail -5; cd ..`
Expected: FAIL — `ItemResolver.h: No such file or directory`.

- [ ] **Step 3: Write `src/resolve/ItemResolver.h`**

```cpp
#pragma once
#include "DecoderRingApi.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct ItemMeta {
    std::string name, icon;
    uint8_t bound = DB_None;
    bool noSell = false, tradeable = false;
    int32_t vendorValue = 0;
};
struct ItemTraits {
    using Meta = ItemMeta;
    static std::string Url(uint32_t id) { return "https://api.guildwars2.com/v2/items/" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& out);
    static const char* FileName() { return "iteminfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m);
    static void FromJson(const nlohmann::json& j, Meta& m);
};
}
```

- [ ] **Step 4: Write `src/resolve/ItemResolver.cpp`**

```cpp
#include "resolve/ItemResolver.h"

namespace Decoder {
namespace {
std::string S(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string();
}
uint8_t BoundOf(const nlohmann::json& flags) {
    bool acc=false, soulAcq=false, accUse=false, soulUse=false;
    for (auto& f : flags) if (f.is_string()) { auto s=f.get<std::string>();
        if (s=="AccountBound") acc=true; else if (s=="SoulbindOnAcquire") soulAcq=true;
        else if (s=="AccountBindOnUse") accUse=true; else if (s=="SoulBindOnUse") soulUse=true; }
    if (soulAcq) return DB_SoulOnAcquire;
    if (acc)     return DB_AccountOnAcquire;
    if (soulUse) return DB_SoulOnUse;
    if (accUse)  return DB_AccountOnUse;
    return DB_None;
}
}

bool ItemTraits::Parse(const std::vector<char>& body, Meta& out) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_object() || !j.contains("name")) return false;
        out.name = S(j, "name");
        out.icon = S(j, "icon");
        out.vendorValue = (j.contains("vendor_value") && j["vendor_value"].is_number_integer())
                          ? j["vendor_value"].get<int>() : 0;
        if (j.contains("flags") && j["flags"].is_array()) {
            out.bound = BoundOf(j["flags"]);
            for (auto& f : j["flags"]) if (f.is_string() && f.get<std::string>()=="NoSell") out.noSell = true;
        }
        // Tradeable iff not bound-on-acquire (bind-on-use stays tradeable; NoSell does NOT gate TP).
        out.tradeable = !(out.bound == DB_AccountOnAcquire || out.bound == DB_SoulOnAcquire);
        return true;
    } catch (...) { return false; }
}

nlohmann::json ItemTraits::ToJson(const Meta& m) {
    return nlohmann::json{ {"n",m.name},{"ic",m.icon},{"b",m.bound},
                           {"ns",m.noSell},{"tr",m.tradeable},{"vv",m.vendorValue} };
}
void ItemTraits::FromJson(const nlohmann::json& j, Meta& m) {
    if (!j.is_object()) return;
    if (j.contains("n"))  m.name = j["n"].get<std::string>();
    if (j.contains("ic")) m.icon = j["ic"].get<std::string>();
    if (j.contains("b"))  m.bound = j["b"].get<uint8_t>();
    if (j.contains("ns")) m.noSell = j["ns"].get<bool>();
    if (j.contains("tr")) m.tradeable = j["tr"].get<bool>();
    if (j.contains("vv")) m.vendorValue = j["vv"].get<int>();
}
}
```

- [ ] **Step 5: Write `src/resolve/SkinResolver.h`**

```cpp
#pragma once
#include "DecoderRingApi.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct SkinMeta { std::string name, icon; };
struct SkinTraits {
    using Meta = SkinMeta;
    static std::string Url(uint32_t id) { return "https://api.guildwars2.com/v2/skins/" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& out);
    static const char* FileName() { return "skininfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m) { return nlohmann::json{ {"n",m.name},{"ic",m.icon} }; }
    static void FromJson(const nlohmann::json& j, Meta& m) {
        if (j.is_object()) { if (j.contains("n")) m.name=j["n"].get<std::string>();
                             if (j.contains("ic")) m.icon=j["ic"].get<std::string>(); }
    }
};
}
```

- [ ] **Step 6: Write `src/resolve/SkinResolver.cpp`**

```cpp
#include "resolve/SkinResolver.h"
namespace Decoder {
bool SkinTraits::Parse(const std::vector<char>& body, Meta& out) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        if (!j.is_object() || !j.contains("name")) return false;
        if (j["name"].is_string()) out.name = j["name"].get<std::string>();
        if (j.contains("icon") && j["icon"].is_string()) out.icon = j["icon"].get<std::string>();
        return true;
    } catch (...) { return false; }
}
}
```

- [ ] **Step 7: Write `src/resolve/SkillResolver.h`**

```cpp
#pragma once
#include "DecoderRingApi.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace Decoder {
struct SkillFactM { std::string icon, text; };
struct SkillMeta { std::string name, icon, description; std::vector<SkillFactM> facts; };
struct SkillTraits {
    using Meta = SkillMeta;
    static std::string Url(uint32_t id) { return "https://api.guildwars2.com/v2/skills/" + std::to_string(id); }
    static bool Parse(const std::vector<char>& body, Meta& out);
    static const char* FileName() { return "skillinfo_v1.json"; }
    static nlohmann::json ToJson(const Meta& m);
    static void FromJson(const nlohmann::json& j, Meta& m);
};
}
```

- [ ] **Step 8: Write `src/resolve/SkillResolver.cpp`** (FormatFact ported from Pie UI `SkillInfoCache.cpp`)

```cpp
#include "resolve/SkillResolver.h"
#include <cstdio>

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
    if (type=="Time"||type=="Duration"||type=="Recharge") { std::string v=NumStr(f,"duration"); return v.empty()?text:(text+": "+v+"s"); }
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
}

bool SkillTraits::Parse(const std::vector<char>& body, Meta& out) {
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
}
```

- [ ] **Step 9: Run to verify pass**

Run: `cd build-host && make decoder_tests 2>&1 | tail -3 && ./decoder_tests; cd ..`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 10: Commit**

```bash
git add src/resolve/ItemResolver.* src/resolve/SkinResolver.* src/resolve/SkillResolver.* tests/test_main.cpp
git commit -m "Add item/skin/skill parsers with canned-JSON tests"
```

---

## Task 6: Price cache (memory-only, TTL, injectable fetch)

**Files:**
- Create: `src/resolve/PriceCache.{h,cpp}`
- Modify: `tests/test_main.cpp`

- [ ] **Step 1: Write the failing test** (append to `tests/test_main.cpp`)

```cpp
#include "resolve/PriceCache.h"

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
```

Add `test_price_cache();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make decoder_tests 2>&1 | tail -5; cd ..`
Expected: FAIL — `PriceCache.h: No such file or directory`.

- [ ] **Step 3: Write `src/resolve/PriceCache.h`**

```cpp
#pragma once
#include "DecoderRingApi.h"
#include "resolve/Http.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>

namespace Decoder {
// Volatile trading-post prices: in-memory only, short TTL, never disk. A warm
// entry within TTL returns immediately; expired/missing kicks a background fetch.
class PriceCache {
public:
    static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    void Initialize(HttpFetch fetch);
    void Shutdown();
    bool Get(uint32_t itemId, DecoderPrice& out);            // warm within TTL -> true
    void DrainCompleted(std::vector<uint32_t>& out);          // ids that finished a fetch
private:
    struct Entry { DecoderPrice price; std::chrono::steady_clock::time_point at; };
    void Run();
    HttpFetch m_Fetch;
    std::unordered_map<uint32_t, Entry> m_Items;
    std::unordered_set<uint32_t> m_Pending;
    std::vector<uint32_t> m_Queue, m_Completed;
    std::mutex m_Mtx; std::condition_variable m_CV;
    std::atomic<bool> m_Stop{false}; std::thread m_Worker;
    int m_TtlSec = 300;
};
}
```

- [ ] **Step 4: Write `src/resolve/PriceCache.cpp`**

```cpp
#include "resolve/PriceCache.h"
#include "nlohmann/json.hpp"

namespace Decoder {
namespace {
bool ParsePrice(const std::vector<char>& body, DecoderPrice& out) {
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());
        out.buy = (j.contains("buys") && j["buys"].contains("unit_price")) ? j["buys"]["unit_price"].get<int>() : -1;
        out.sell = (j.contains("sells") && j["sells"].contains("unit_price")) ? j["sells"]["unit_price"].get<int>() : -1;
        return true;
    } catch (...) { return false; }
}
}

void PriceCache::Initialize(HttpFetch fetch) {
    m_Fetch = std::move(fetch); m_Stop = false;
    m_Worker = std::thread([this]{ Run(); });
}
void PriceCache::Shutdown() { m_Stop = true; m_CV.notify_all(); if (m_Worker.joinable()) m_Worker.join(); }

bool PriceCache::Get(uint32_t itemId, DecoderPrice& out) {
    if (itemId == 0) return false;
    std::lock_guard<std::mutex> lk(m_Mtx);
    auto it = m_Items.find(itemId);
    if (it != m_Items.end()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - it->second.at).count();
        if (age < m_TtlSec) { out = it->second.price; return true; }   // fresh
    }
    if (m_Pending.count(itemId)) return false;
    m_Pending.insert(itemId); m_Queue.push_back(itemId); m_CV.notify_one();
    return false;
}

void PriceCache::DrainCompleted(std::vector<uint32_t>& out) {
    std::lock_guard<std::mutex> lk(m_Mtx);
    out.insert(out.end(), m_Completed.begin(), m_Completed.end());
    m_Completed.clear();
}

void PriceCache::Run() {
    for (;;) {
        uint32_t id = 0;
        { std::unique_lock<std::mutex> lk(m_Mtx);
          m_CV.wait(lk, [this]{ return m_Stop.load() || !m_Queue.empty(); });
          if (m_Stop.load()) return;
          id = m_Queue.back(); m_Queue.pop_back(); }
        std::vector<char> body; DecoderPrice p{-1,-1};
        bool ok = m_Fetch && m_Fetch("https://api.guildwars2.com/v2/commerce/prices/" + std::to_string(id), body)
                  && ParsePrice(body, p);
        std::lock_guard<std::mutex> lk(m_Mtx);
        m_Pending.erase(id);
        if (ok) m_Items[id] = Entry{ p, std::chrono::steady_clock::now() };   // memory only
        m_Completed.push_back(id);
    }
}
}
```

- [ ] **Step 5: Run to verify pass**

Run: `cd build-host && make decoder_tests 2>&1 | tail -3 && ./decoder_tests; cd ..`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add src/resolve/PriceCache.* tests/test_main.cpp
git commit -m "Add memory-only TTL price cache"
```

---

## Task 7: DecoderService facade + end-to-end self-test (warm / cold→event / failed→event / retry)

**Files:**
- Create: `src/resolve/DecoderService.{h,cpp}`
- Modify: `tests/test_main.cpp`

- [ ] **Step 1: Write `src/resolve/DecoderService.h`**

```cpp
#pragma once
#include "DecoderRingApi.h"
#include "resolve/Http.h"
#include <functional>
#include <string>

namespace Decoder {
// The facade consumers call (through the published function table). Owns every
// resolver, dispatches by linkType, and on Tick() drains completed fetches and
// emits one completion per id through the injected sink.
//
// Parameterised for testability: a HttpFetch (production: WinINet; test: fake)
// and a CompletionSink (production: Events_Raise; test: capture).
class DecoderService {
public:
    using CompletionSink = std::function<void(const DecoderRecord&)>;

    // dir: disk-cache directory ("" disables disk). Safe to call once at load.
    void Initialize(const std::string& dir, HttpFetch fetch, CompletionSink sink);
    void Shutdown();

    // Warm resolve. For offline types fills synchronously (DR_Resolved). For
    // networked types: warm -> DR_Resolved, cold -> DR_NotReady (fetch kicked).
    // chatCode is required for offline types (build/waypoint); networked types
    // may pass "" since (linkType,id) is sufficient.
    DecoderStatus Resolve(uint8_t linkType, uint32_t id, const std::string& chatCode, DecoderRecord& out);
    // ABI-facing overload: same, without the chat code (networked types + ids only).
    DecoderStatus Resolve(uint8_t linkType, uint32_t id, DecoderRecord& out) { return Resolve(linkType, id, std::string(), out); }

    DecoderStatus QueryPrice(uint32_t itemId, DecoderPrice& out);

    // Main-thread pump: drain completed fetches -> build records -> sink; flush disk.
    void Tick();

    // For tests: bypass the fail cooldown so a retry fires immediately.
    void SetFailCooldownSec(int s);

private:
    struct Impl; Impl* m_p = nullptr;
};
}
```

- [ ] **Step 2: Write `src/resolve/DecoderService.cpp`**

```cpp
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

namespace Decoder {
using namespace PieUI::ChatLinks;

struct DecoderService::Impl {
    AsyncResolver<ItemTraits>  item;
    AsyncResolver<SkinTraits>  skin;
    AsyncResolver<SkillTraits> skill;
    PriceCache                 price;
    CompletionSink             sink;

    // Build a resolved record from warm meta for emission.
    void EmitResolved(uint8_t type, uint32_t id) {
        DecoderRecord r;
        if (type == LINK_ITEM)  { ItemMeta m; if (item.Get(id,m)) { InitRecord(r,type,id,DR_Resolved); CopyField(r.name,m.name); CopyField(r.iconUrl,m.icon); r.bound=m.bound; r.noSell=m.noSell?1:0; r.tradeable=m.tradeable?1:0; r.vendorValue=m.vendorValue; sink(r);} }
        else if (type == LINK_SKIN)  { SkinMeta m; if (skin.Get(id,m)) { InitRecord(r,type,id,DR_Resolved); CopyField(r.name,m.name); CopyField(r.iconUrl,m.icon); sink(r);} }
        else if (type == LINK_SKILL) { SkillMeta m; if (skill.Get(id,m)) { InitRecord(r,type,id,DR_Resolved); CopyField(r.name,m.name); CopyField(r.iconUrl,m.icon); CopyField(r.description,m.description);
            uint8_t n = (uint8_t)(m.facts.size() < 16 ? m.facts.size() : 16);
            for (uint8_t i=0;i<n;++i){ CopyField(r.facts[i].icon,m.facts[i].icon); CopyField(r.facts[i].text,m.facts[i].text);} r.factCount=n; sink(r);} }
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
    // Offline types resolve synchronously from compiled-in data.
    if (type == LINK_BUILD || type == LINK_MAP)
        return ResolveOffline(type, chatCode, out) ? DR_Resolved : DR_Failed;

    if (type == LINK_ITEM) {
        ItemMeta m;
        if (m_p->item.Get(id, m)) { InitRecord(out,type,id,DR_Resolved); CopyField(out.name,m.name); CopyField(out.iconUrl,m.icon);
            out.bound=m.bound; out.noSell=m.noSell?1:0; out.tradeable=m.tradeable?1:0; out.vendorValue=m.vendorValue; return DR_Resolved; }
        InitRecord(out,type,id,DR_NotReady); return DR_NotReady;
    }
    if (type == LINK_SKIN) {
        SkinMeta m;
        if (m_p->skin.Get(id, m)) { InitRecord(out,type,id,DR_Resolved); CopyField(out.name,m.name); CopyField(out.iconUrl,m.icon); return DR_Resolved; }
        InitRecord(out,type,id,DR_NotReady); return DR_NotReady;
    }
    if (type == LINK_SKILL) {
        SkillMeta m;
        if (m_p->skill.Get(id, m)) { InitRecord(out,type,id,DR_Resolved); CopyField(out.name,m.name); CopyField(out.iconUrl,m.icon); CopyField(out.description,m.description);
            uint8_t n=(uint8_t)(m.facts.size()<16?m.facts.size():16);
            for (uint8_t i=0;i<n;++i){ CopyField(out.facts[i].icon,m.facts[i].icon); CopyField(out.facts[i].text,m.facts[i].text);} out.factCount=n; return DR_Resolved; }
        InitRecord(out,type,id,DR_NotReady); return DR_NotReady;
    }
    InitRecord(out, type, id, DR_Failed);
    return DR_Failed;   // unsupported type
}

DecoderStatus DecoderService::QueryPrice(uint32_t itemId, DecoderPrice& out) {
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
```

- [ ] **Step 3: Write the end-to-end self-test** (append to `tests/test_main.cpp`)

```cpp
#include "resolve/DecoderService.h"
#include "chat/ChatLinks.h"
#include <vector>

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
```

Add `test_service_end_to_end();` to `main()`. Add `static void DecoderServiceSleep(){}`? No — `DecoderService::SleepMs` is needed; add it to the class:

In `DecoderService.h`, add inside the class public section:
```cpp
    static void SleepMs(int ms);
```
In `DecoderService.cpp`, add:
```cpp
void DecoderService::SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
```
and `#include <thread>` + `#include <chrono>` at the top of the .cpp.

- [ ] **Step 4: Run to verify it fails, then passes**

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make decoder_tests 2>&1 | tail -5 && ./decoder_tests; cd ..`
Expected: after writing the files above, `ALL TESTS PASSED`. This is the prompt's mandatory state-machine proof (warm / cold→event / failed→event / retry / offline-sync), correlation key asserted on every event.

- [ ] **Step 5: Commit**

```bash
git add src/resolve/DecoderService.* tests/test_main.cpp
git commit -m "Add DecoderService facade + end-to-end warm/cold/failed self-test"
```

---

## Task 8: Nexus integration — production fetch, DataLink publish, handshake, render-pump

**Files:**
- Create: `src/resolve/Http_WinINet.cpp`
- Create: `src/dllmain.cpp`

- [ ] **Step 1: Write `src/resolve/Http_WinINet.cpp`** (production fetch behind the seam)

```cpp
#include "resolve/Http.h"
#include <windows.h>
#include <wininet.h>

namespace Decoder {
// Production HttpFetch — WinINet, mirroring Pie UI's cache HttpGet.
bool WinINetFetch(const std::string& url, std::vector<char>& out) {
    HINTERNET hI = InternetOpenA("DecoderRing/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hI) return false;
    HINTERNET hU = InternetOpenUrlA(hI, url.c_str(), NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hU) { InternetCloseHandle(hI); return false; }
    out.clear();
    char chunk[4096]; DWORD n = 0;
    while (InternetReadFile(hU, chunk, sizeof(chunk), &n) && n > 0)
        out.insert(out.end(), chunk, chunk + n);
    InternetCloseHandle(hU); InternetCloseHandle(hI);
    return !out.empty();
}
}
```

- [ ] **Step 2: Write `src/dllmain.cpp`** (lifecycle + handshake + pump)

```cpp
#include "AddonShared.h"
#include "DecoderRingApi.h"
#include "resolve/DecoderService.h"
#include "resolve/Http.h"
#include <windows.h>
#include <string>

// One display-name constant so the addon is trivial to rename.
static constexpr const char* DR_DISPLAY_NAME = "Decoder Ring";

AddonAPI_t* APIDefs = nullptr;
static AddonDefinition_t AddonDef{};
static HMODULE hSelf = nullptr;

namespace Decoder { bool WinINetFetch(const std::string& url, std::vector<char>& out); }

static Decoder::DecoderService g_service;
static DecoderRingApi* g_api = nullptr;     // published in shared memory
static bool g_subscribed = false;

// --- C-ABI thunks the published table points at (forward to the service) ---
static DecoderStatus Api_Resolve(uint8_t type, uint32_t id, DecoderRecord* out) {
    if (!out) return DR_Failed;
    return g_service.Resolve(type, id, *out);
}
static DecoderStatus Api_QueryPrice(uint32_t itemId, DecoderPrice* out) {
    if (!out) return DR_Failed;
    return g_service.QueryPrice(itemId, *out);
}

// Completion sink: raise the miss-event with a pointer to the record (Nexus
// delivers synchronously, so a stack pointer is safe for the duration).
static void OnCompletion(const DecoderRecord& r) {
    if (APIDefs && APIDefs->Events_Raise)
        APIDefs->Events_Raise(EV_DECODER_RING_RESOLVED, (void*)&r);
}

// Consumer asked us to (re-)announce. Idempotent.
static void OnPing(void*) {
    if (APIDefs && APIDefs->Events_Raise) APIDefs->Events_Raise(EV_DECODER_RING_READY, nullptr);
}

// Render callback: draws NOTHING. Pure main-thread pump (drain completions ->
// raise events; throttled disk flush).
static void AddonRender() { g_service.Tick(); }

static void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;

    const char* dir = APIDefs->Paths_GetAddonDirectory ? APIDefs->Paths_GetAddonDirectory("DecoderRing") : nullptr;
    g_service.Initialize(dir ? dir : ".", &Decoder::WinINetFetch, &OnCompletion);

    // Publish the exported function table in shared memory (version FIRST).
    if (APIDefs->DataLink_Share) {
        g_api = (DecoderRingApi*)APIDefs->DataLink_Share(DECODER_RING_DATALINK, sizeof(DecoderRingApi));
        if (g_api) {
            g_api->apiVersion = DECODER_RING_API_VERSION;
            g_api->Resolve = &Api_Resolve;
            g_api->QueryPrice = &Api_QueryPrice;
        }
    }

    // Render-pump (no drawing) + ready handshake (mirror AlterEgoBridge).
    if (APIDefs->GUI_Register) APIDefs->GUI_Register(RT_Render, AddonRender);
    if (APIDefs->Events_Subscribe) { APIDefs->Events_Subscribe(EV_DECODER_RING_PING, OnPing); g_subscribed = true; }
    if (APIDefs->Events_Raise) APIDefs->Events_Raise(EV_DECODER_RING_READY, nullptr);
}

static void AddonUnload() {
    if (APIDefs && g_subscribed && APIDefs->Events_Unsubscribe)
        APIDefs->Events_Unsubscribe(EV_DECODER_RING_PING, OnPing);
    if (APIDefs && APIDefs->GUI_Deregister) APIDefs->GUI_Deregister(AddonRender);
    g_subscribed = false;
    g_api = nullptr;          // shared resource is owned/freed by Nexus
    g_service.Shutdown();
    APIDefs = nullptr;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    AddonDef.Signature = 0x44524e47; // "DRNG"
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = DR_DISPLAY_NAME;
    AddonDef.Version.Major = 0; AddonDef.Version.Minor = 1;
    AddonDef.Version.Build = 0; AddonDef.Version.Revision = 0;
    AddonDef.Author = "PieOrCake.7635";
    AddonDef.Description = "Resolves GW2 chat-link IDs to metadata for other addons.";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = AF_None;
    AddonDef.Provider = UP_GitHub;
    AddonDef.UpdateLink = "https://github.com/PieOrCake/decoder_ring";
    return &AddonDef;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) hSelf = hModule;
    return TRUE;
}
```

> Note: verify `GUI_Deregister` is the exact deregister name in `include/nexus/Nexus.h` (Pie UI calls `GUI_Register(RT_Render, …)`; the matching deregister symbol is in the AddonAPI struct — grep for `GUI_Deregister` / `GUI_Unregister` and use the real name).

- [ ] **Step 3: Build the DLL** (cross-compile)

Run:
```bash
mkdir -p build && cd build && cmake .. >/dev/null && make -j$(nproc) 2>&1 | tail -15; cd ..
```
Expected: `DecoderRing.dll` produced in `build/`. If `GUI_Deregister` is misnamed, the compile error names it — fix to the real symbol and rebuild. If MinGW is absent, install `mingw-w64`.

- [ ] **Step 4: Confirm the DLL exports `GetAddonDef`**

Run: `x86_64-w64-mingw32-objdump -p build/DecoderRing.dll | grep -i GetAddonDef`
Expected: a line listing `GetAddonDef` in the export table.

- [ ] **Step 5: Re-run host tests** (integration must not have broken the core)

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make decoder_tests 2>&1 | tail -3 && ./decoder_tests; cd ..`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add src/dllmain.cpp src/resolve/Http_WinINet.cpp
git commit -m "Wire Nexus: DataLink publish, ready handshake, render-pump, WinINet fetch"
```

---

## Task 9: Documentation — API.md integration guide + README

**Files:**
- Create: `docs/API.md`, `README.md`

- [ ] **Step 1: Write `docs/API.md`** covering, with concrete code:
  - How to copy `public/DecoderRingApi.h` into a consumer.
  - **Presence detection:** `DataLink_Get(DECODER_RING_DATALINK)` → non-null + `apiVersion == DECODER_RING_API_VERSION`; subscribe `EV_DECODER_RING_READY`; raise `EV_DECODER_RING_PING` on load. Show the deadlock-free handshake from both load orders.
  - **Warm query:** call `api->Resolve(type, id, &record)`; handle `DR_Resolved` / `DR_NotReady` / `DR_Failed`.
  - **Miss event:** subscribe `EV_DECODER_RING_RESOLVED`, payload `DecoderRecord*`, match on `(linkType, id)`, copy during the synchronous handler.
  - **Price:** `api->QueryPrice` is separate and volatile.
  - **Graceful degradation:** with the service absent, `DataLink_Get` stays null; the consumer falls back to its own vendored `ChatLinks` structural decode (type+IDs+spans) + build/waypoint labels; only item/skin/skill names go missing. The service being absent must never crash a consumer.

- [ ] **Step 2: Write `README.md`** — brief: what it is (a chat-link → metadata resolver service for Nexus addons), what link types it resolves (item/skin/skill via `/v2`; build/waypoint offline), the two-part API in one paragraph, build instructions (`cmake .. && make` for the DLL; `-DDECODER_TESTS_ONLY=ON` for host tests), and "the user deploys the DLL; this project never copies it into the addons folder."

- [ ] **Step 3: Commit**

```bash
git add docs/API.md README.md
git commit -m "Add API integration guide + README"
```

---

## Task 10: Final verification + written summary

- [ ] **Step 1: Full host test suite**

Run: `cd build-host && cmake -DDECODER_TESTS_ONLY=ON .. >/dev/null && make 2>&1 | tail -5 && ./decoder_tests && ./chatlinks_isolation; cd ..`
Expected: `ALL TESTS PASSED` and `chatlinks isolation: OK`.

- [ ] **Step 2: DLL builds clean**

Run: `cd build && cmake .. >/dev/null && make -j$(nproc) 2>&1 | tail -5 && ls -la DecoderRing.dll; cd ..`
Expected: `DecoderRing.dll` present. **Do not deploy it** — the user copies it into the addons folder themselves.

- [ ] **Step 3: Re-confirm vendored files never drifted**

Run:
```bash
for f in ChatLinks.h ChatLinks.cpp SpecData.h WaypointNames.h WaypointNames.cpp WaypointTable.h; do
  diff -q /home/tony/Dev/pie_ui/src/chat/$f src/chat/$f || echo "DRIFT: $f"; done
```
Expected: no output.

- [ ] **Step 4: Write the summary** (in the final report to the user, not a file): the API surface (DataLink function table + `EV_DECODER_RING_RESOLVED`), the handshake design (announce/ping/ready + immediate `DataLink_Get`, deadlock-free both orders), and the test results (host suite + isolation pass; DLL builds + exports `GetAddonDef`).

---

## Self-Review (completed during authoring)

- **Spec coverage:** two-part API (T2/T7/T8); versioned POD record version-first (T2); handshake (T8) + documented (T9); item/skin/skill async+disk (T4/T5), price memory-only TTL (T6), waypoint+build offline (T3); never-persist-empty + retryable failed (T4); end-to-end warm/cold/failed self-test (T7); vendoring byte-identical + isolation (T1); API.md + degradation (T9); feature branch + no deploy (T0/T10). All mapped.
- **Placeholder scan:** none — every code step carries full code; the two `Note:` items (AE2 wrapper label location; verify `GUI_Deregister` symbol) are explicit verification instructions, not deferred work.
- **Type consistency:** `DecoderRecord`, `DecoderStatus`, `DecoderRingApi`, `DecoderPrice`, `*Meta`/`*Traits`, `AsyncResolver<>`, `DecoderService::{Resolve,QueryPrice,Tick,SetFailCooldownSec,SleepMs}`, event/DataLink macros — consistent across tasks.

# Decoder Ring — design spec

**Date:** 2026-06-10
**Status:** Approved (brainstorm) — pending written-spec review
**Source prompt:** `build-decoder-addon-prompt.md`

## What this is

A standalone Raidcore Nexus addon (Windows DLL, cross-compiled from Linux with MinGW) whose
single job is **resolving GW2 chat-link identities into displayable metadata** and serving that to
other Nexus addons over a clean exported-function + events interface. It is a **data provider, not a
UI addon**: it draws no widgets, owns no chat window, hooks no game calls, does no memory RE, and
passes no textures or image bytes across its boundary — icon **URLs only**.

Working display name lives in **one constant** so it is trivial to rename. Working name: "Decoder
Ring".

## Architecture — three tiers

| Tier | Owner | What |
|------|-------|------|
| Structural decode | **Vendored** (`ChatLinks.{h,cpp}` + `SpecData.h`, copied byte-identical from Pie UI `src/chat/`) | base64 → link type + raw IDs + ordered spans. Pure, offline. |
| **Identity resolution** | **THIS ADDON** | type+id → distilled metadata record. Async, networked (`/v2`), disk-cached. Build/AE2 + waypoint resolve from compiled-in data, no network. |
| Rendering | Each **consumer** (not here) | draw a chip, upload icon URL to a texture, hit-test, open map. |

**Two hard boundary rules:** (1) no textures/image bytes cross the boundary — URLs (strings) only;
(2) the service returns DATA, never UI — no ImGui, no draw calls, no game-call hooks.

## Decision 1 — API surface

### Warm query: exported function table via Nexus `DataLink`

On load Decoder publishes a shared struct under a fixed identifier (`DECODER_RING_API`) via
`DataLink_Share`. The struct is **versioned (version field first)** and holds function pointers.
This mirrors how RTAPI publishes `RealTimeData` on this codebase. Consumers `DataLink_Get` it,
verify non-null + matching `apiVersion`, then call through it.

```c
// Published in shared memory; ABI-stable, version-first.
struct DecoderRingApi {
    uint32_t apiVersion;   // bump on ANY signature/layout change; consumer must check
    // Warm resolve. Returns immediately. Fills *out and returns the status.
    //   Resolved  -> *out fully populated from warm cache
    //   NotReady  -> background fetch kicked (or in flight); watch EV_DECODER_RING_RESOLVED
    //   Failed    -> last fetch failed and still in cooldown (retryable later)
    DecoderStatus (*Resolve)(uint8_t linkType, uint32_t id, DecoderRecord* out);
    // Volatile trading-post price, in-memory-only, short TTL. Separate from the durable record.
    DecoderStatus (*QueryPrice)(uint32_t itemId, DecoderPrice* out);
};
```

`Resolve` **never blocks on the network** — warm returns data, cold returns `NotReady` and
schedules the fetch. Safe to call inline on a render thread.

### Miss-completion event: `EV_DECODER_RING_RESOLVED`

When a background fetch lands, Decoder raises `EV_DECODER_RING_RESOLVED` with a pointer to the
completed `DecoderRecord` (which carries its own correlation key). Nexus delivers events
synchronously, so the subscriber copies what it needs during the handler — same pattern as
`AlterEgoBridge` / `GroupRoster`.

**Thread discipline:** worker threads do HTTP + parse, but the **event is raised from Decoder's own
main-thread pump** — a registered Nexus render callback that draws nothing and only drains
completed fetches (raising one event per completion) and does the throttled disk flush. This keeps
every `Events_Raise` on the main thread, matching Pie UI's `Tick()` discipline and avoiding
cross-thread event hazards.

### Correlation key = `(linkType, id)` tuple

Not the chat-code string. The consumer already has type+id from the vendored codec; the tuple is
compact and name/icon resolution does not depend on item count/skin. Query and event both carry it.

## Decision 2 — metadata record

**One versioned, fixed-size POD struct.** `schemaVersion` **first**. Plain fixed `char` buffers —
**no `std::string`, no pointers, no STL across the DLL boundary** — so it is `memcpy`-able and safe
in shared memory and event payloads. This matches RTAPI's public structs (`char AccountName[…]`).
Tagged by `linkType`; union of per-type fields. Fields are the prompt's *distilled* set (leaner
than Pie UI's internal caches).

```c
enum DecoderStatus : uint8_t { DR_NotReady=0, DR_Resolved=1, DR_Failed=2 };

struct DecoderRecord {
    uint16_t schemaVersion;     // FIRST. Layout version of THIS record.
    uint8_t  linkType;          // correlation: ChatLinkType byte
    uint32_t id;                // correlation: resolved id
    uint8_t  status;            // DecoderStatus

    char     name[128];         // display name (or build/waypoint label); "" if unresolved
    char     iconUrl[256];      // render-service icon URL; may be ""

    // --- Item (linkType == LINK_ITEM) ---
    uint8_t  bound;             // BoundKind enum (none / acct-acquire / soul-acquire / on-use…)
    int32_t  vendorValue;       // copper
    uint8_t  noSell;            // cannot vendor to NPC (does NOT imply untradeable)
    uint8_t  tradeable;         // eligible for trading post

    // --- Skill (linkType == LINK_SKILL) ---
    char     description[512];  // skill description
    uint8_t  factCount;         // number of valid entries in facts[]
    struct { char icon[128]; char text[160]; } facts[16];  // pre-formatted; overflow dropped

    // --- Waypoint / POI (linkType == LINK_MAP) ---
    char     mapName[96];       // map the POI sits on
    uint8_t  poiType;           // PoiKind enum (waypoint / poi / vista)
    // Build/AE2 (LINK_BUILD): spec label carried in name[]; no extra fields.
};
```

Every field documented at its declaration in the header. Fixed caps (name/desc/facts) truncate
gracefully; the only variable-length data anywhere is skill facts, bounded by `facts[16]`.

### Volatile price (separate, never on the durable record)

```c
struct DecoderPrice { int32_t buy; int32_t sell; };  // copper; -1 = no listings that side
```

In-memory-only, ~5-min TTL, never disk — exactly `CommercePriceCache`. Fetched via `QueryPrice`,
not baked into `DecoderRecord`.

## Decision 3 — ready handshake (load-order-independent, cannot deadlock)

Mirror `AlterEgoBridge` — the proven deadlock-free pattern on this codebase.

- **Decoder on load:** `DataLink_Share` the API struct (gettable immediately) → subscribe
  `EV_DECODER_RING_PING` → raise `EV_DECODER_RING_READY`. On any later ping, re-raise READY
  (idempotent).
- **Consumer on load:** subscribe `EV_DECODER_RING_READY`; **immediately** `DataLink_Get`
  (covers "Decoder loaded first, I missed READY"); raise `EV_DECODER_RING_PING` (covers "Decoder
  loads later / re-announce").

**Guarantee — neither side ever blocks on the other:**
- Decoder first → consumer's immediate `DataLink_Get` succeeds; event is belt-and-suspenders.
- Consumer first → its `DataLink_Get` is null, but ping/READY (or simply polling `DataLink_Get`
  each frame) connects them the instant Decoder loads.
- Decoder absent forever → `DataLink_Get` stays null; consumer falls back to vendored structural
  decode. No hang either way.

## Resolution sources (ported from Pie UI, flipped poll→query+event)

Each networked type is its own worker-backed cache, archetype = `ItemInfoCache`: worker thread +
WinINet `HttpGet` + mutex-guarded maps + `s_Pending` (in-flight) + `s_Fail` (cooldown, retryable) +
throttled disk flush on `Tick` + **never-persist-empty** (only store on success; failures go to
`s_Fail`, never written to disk as truth).

| Type | Source | Cache | Net |
|------|--------|-------|-----|
| Item | `/v2/items/:id` | disk (`iteminfo_*.json`) | yes |
| Skin | `/v2/skins/:id` | disk (`skininfo_*.json`) | yes |
| Skill | `/v2/skills/:id` (desc + facts) | disk (`skillinfo_*.json`) | yes |
| Price | `/v2/commerce/prices/:id` | **memory-only, ~5-min TTL** | yes |
| Waypoint/POI | compiled-in `WaypointTable.h` (binary search) | none | **no** |
| Build/AE2 | `SpecData.h` name lookups | none | **no** |

Disk caches use the **bump-the-filename-on-schema-change** discipline. Caching is owned by the
service; consumers never cache resolution results. Failed fetches degrade to `DR_Failed` (not a
crash, not a poison entry); a later re-query retries after cooldown.

## Vendoring discipline

Copy byte-identical from Pie UI's canonical `src/chat/`, single physical copy each, `#pragma once`
intact, **never fork-edit**:
- `ChatLinks.h`, `ChatLinks.cpp`, `SpecData.h` (codec — required).
- `WaypointTable.h` + `WaypointNames.{h,cpp}` + `tools/gen_waypoints.py` (waypoint table + generator;
  keep the "re-run on CONTENT patch" note).

Canonical upstream is Pie UI `src/chat/`; codec changes happen there and re-vendor outward.

## Project structure (mirror Pie UI house style)

- `dllmain.cpp` — Nexus lifecycle (load/unload, API publish, handshake, render-pump registration).
- Pure, host-testable pieces split from Nexus-coupled pieces; **no Nexus/ImGui include creep** into
  the testable core.
- `DecoderRecord` / `DecoderRingApi` / event names in a single shared public header consumers can
  copy (the integration ABI).
- Resolver modules: one per source (item/skin/skill/price networked; waypoint/build offline), each
  exposing a warm `Get` + completion drain.
- Cross-compile to Windows DLL with MinGW: `cmake .. && make -j$(nproc)`; separate host-test build
  dir (`-DTESTS_ONLY=ON`-style) compiling on Linux g++.

## Proving the service without a real consumer

1. **Host test harness** (Linux g++, like Pie UI `tests/`): structural decode → resolution dispatch
   → record shaping → cache behaviour, for every link type. Build/AE2 + waypoint fully offline.
   Networked types tested with an **injectable fake fetch** (stub the HTTP layer) so the
   warm/miss/failed state machine is verified without real HTTP.
2. **Self-test / test-consumer** driving BOTH delivery halves end to end: warm query (pre-seeded →
   immediate data); cold query (`NotReady` → subsequent `EV_DECODER_RING_RESOLVED` with matching
   key + resolved record).
3. **State machine asserted explicitly:** warm→record; cold→not-ready then event fires once with
   matching key + resolved record; failed-fetch→not-ready then event fires with `DR_Failed`;
   re-query after failure → retried.

The HTTP layer is therefore an **injectable seam** (function pointer / interface) so tests supply a
fake fetcher; production wires WinINet.

## Graceful-degradation contract (for `API.md`)

With Decoder **absent**: consumers vendor `ChatLinks` themselves, so they can always decode
STRUCTURE locally (type + IDs + spans) and render structural chips + build/waypoint labels (the
latter need only vendored `SpecData.h` / waypoint table if the consumer also vendors it). Only
API-resolved names (item/skin/skill) go missing. Absence is detected by `DataLink_Get` returning
null (or no READY event); the warm-query pointer simply isn't there and the consumer falls back to
structural rendering. **The service being absent must never crash a consumer.**

## Deliverables / definition of done

1. Builds to a Windows DLL via MinGW; host tests build + pass on Linux g++.
2. Two-part API: synchronous warm-query exported function (warm→record, cold→not-ready) + miss
   event carrying correlation key + record, via Pie UI's proven event pattern.
3. Load-order-independent ready handshake that provably cannot deadlock either way; mechanism
   documented.
4. All link types resolved: build/AE2 + waypoint offline; item/skin/skill from `/v2` async + disk;
   price in-memory-only short-TTL. Failed fetches → `DR_Failed`, retryable; no empty/failed cache
   ever persisted as truth.
5. Versioned per-type record (version first), each field documented; no textures/bytes — icon URL
   strings only.
6. Self-test drives warm + cold + failed end to end and asserts the full state machine; offline
   types covered thoroughly; networked types covered with a stubbed fetch.
7. `API.md` integration guide + graceful-degradation contract.
8. Vendored codec files byte-identical to Pie UI's `src/chat/`, under the no-fork discipline.
9. Feature branch off `master`; DLL never deployed; short written summary of API surface, handshake,
   and test results.

## Explicitly out of scope

- Not migrating Pie UI onto the service (separate prompt).
- Not building the notepad addon.
- No rendering, no game-call hooks, no memory RE.
- No textures/byte arrays across the boundary — URLs only.
- Consumers do not cache resolution results — the service owns the cache.

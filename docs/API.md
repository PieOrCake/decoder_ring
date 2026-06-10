# Decoder Ring — Consumer Integration Guide

Decoder Ring is a headless Nexus addon that resolves GW2 chat-link IDs into metadata records and
serves them to other addons via a shared function table and a completion event. This document covers
everything a consumer addon needs to integrate correctly, including what to do when the service is
absent.

---

## 1. Setup — copy the header

Copy `public/DecoderRingApi.h` from the Decoder Ring repository into your addon's source tree. It
depends only on `<cstdint>` — no Nexus headers, no Win32, no STL containers cross the boundary.

```cpp
#include "DecoderRingApi.h"
```

---

## 2. Presence detection and the ready handshake

Load order between Nexus addons is not guaranteed. The handshake pattern below resolves correctly
regardless of which addon loads first, and cannot deadlock in either direction.

**How the service behaves on its own load:**
1. Publishes `DecoderRingApi` in shared memory under `DECODER_RING_DATALINK`.
2. Subscribes `EV_DECODER_RING_PING` — replies to any ping by re-raising `EV_DECODER_RING_READY`.
3. Raises `EV_DECODER_RING_READY` immediately.

**How the service behaves on its own unload:**
1. Zeroes the published function table (`apiVersion = 0`, pointers `nullptr`). The DataLink block
   itself survives — it is owned by Nexus, not the addon — but it is now neutralised.
2. Raises `EV_DECODER_RING_UNLOADING` so subscribed consumers can drop references immediately.
3. Tears down its threads and event subscriptions.

> **Critical:** the DataLink block outlives the DLL. After unload, `DataLink_Get` still returns a
> non-null pointer to the (now-zeroed) struct. **Never call a `DecoderRingApi*` without first
> checking `apiVersion == DECODER_RING_API_VERSION` — a stale pointer to an unloaded service is the
> single most likely way to crash the game.** See [section 7](#7-consumer-lifetime-contract-required).

**What your addon must do on its own load (in this order):**

```cpp
// Do NOT cache a DecoderRingApi* across calls. Re-validate before EVERY use via
// this accessor — it is a cheap registry lookup, and it is the only thing that is
// safe once Decoder Ring can unload at any time.
static DecoderRingApi* GetDecoder() {
    auto* api = static_cast<DecoderRingApi*>(
        APIDefs->DataLink_Get(DECODER_RING_DATALINK));
    return (api && api->apiVersion == DECODER_RING_API_VERSION) ? api : nullptr;
}

// READY/UNLOADING are notifications for UI state only — they let you flip a
// "present" indicator live. They are NOT a substitute for GetDecoder() at call time.
static void OnDecoderReady(void*)     { /* re-detect presence; refresh any "present" indicator */ }
static void OnDecoderUnloading(void*) { /* mark absent; drop any cached record/pending state    */ }

// In your AddonLoad:
void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;

    // 1. Subscribe appearance AND disappearance before anything else.
    APIDefs->Events_Subscribe(EV_DECODER_RING_READY,     OnDecoderReady);
    APIDefs->Events_Subscribe(EV_DECODER_RING_UNLOADING, OnDecoderUnloading);

    // 2. Raise a PING — if Decoder loaded after you, it replies with READY; if it
    //    loaded first, this still prompts a fresh READY. Either way GetDecoder()
    //    below will reflect current truth.
    APIDefs->Events_Raise(EV_DECODER_RING_PING, nullptr);
}
```

**Why this cannot deadlock:**
- Decoder first → `GetDecoder()` returns the pointer the moment you call it; the PING reply is
  belt-and-suspenders.
- Consumer first → `GetDecoder()` returns null until Decoder's later load triggers the PING reply
  (READY), after which `GetDecoder()` starts returning a valid pointer.
- Decoder absent forever → `GetDecoder()` always returns null and the consumer falls back gracefully
  (see section 6).

On unload, unsubscribe both events:

```cpp
APIDefs->Events_Unsubscribe(EV_DECODER_RING_READY,     OnDecoderReady);
APIDefs->Events_Unsubscribe(EV_DECODER_RING_UNLOADING, OnDecoderUnloading);
```

---

## 3. Warm query — `Resolve`

Call `api->Resolve` to ask for a record immediately. It **never blocks on the network** and is safe
to call on the render thread.

```cpp
DecoderStatus (*Resolve)(uint8_t linkType, uint32_t id,
                         const char* chatCode, DecoderRecord* out);
```

**`chatCode`** is the full `"[&...]"` chat link string.
- **Build links (linkType `0x0D`):** `chatCode` is **required**. Build links carry no integer id,
  so the chat-link bytes are the only source of identity. Pass `nullptr` and you will get
  `DR_Failed`.
- **Item (`0x02`), skin (`0x0A`), skill (`0x06`), waypoint/POI (`0x04`):** `chatCode` may be
  `nullptr`. The `(linkType, id)` pair is sufficient to identify and correlate the request.

```cpp
void ShowChatLinkTooltip(uint8_t linkType, uint32_t id, const char* chatCode) {
    DecoderRingApi* decoder = GetDecoder();          // re-validate every call — never cache
    if (!decoder) { RenderFallback(linkType, id); return; }

    DecoderRecord rec{};
    DecoderStatus status = decoder->Resolve(linkType, id, chatCode, &rec);

    switch (status) {
    case DR_Resolved:
        // rec is fully populated. Use it directly.
        RenderRecord(rec);
        break;

    case DR_NotReady:
        // A background fetch was kicked (or is already in flight).
        // rec.linkType and rec.id are set — store them to correlate the
        // incoming EV_DECODER_RING_RESOLVED event later.
        StoreCorrelationKey(rec.linkType, rec.id);
        RenderSpinner();
        break;

    case DR_Failed:
        // The most recent fetch failed and is in cooldown.
        // Re-calling Resolve later will retry once the cooldown expires.
        RenderError("name unavailable");
        break;
    }
}
```

Always check `rec.schemaVersion == DECODER_RING_API_VERSION` if you read the record outside the
immediate call context — the struct layout may change between service versions.

**Fields common to all link types (valid whenever `status == DR_Resolved`):**

| Field | Type | Notes |
|---|---|---|
| `schemaVersion` | `uint16_t` | Always first; check before reading other fields |
| `linkType` | `uint8_t` | Correlation key part 1 |
| `id` | `uint32_t` | Correlation key part 2 |
| `name` | `char[128]` | Display name, build label, or waypoint name |
| `iconUrl` | `char[256]` | Render-service icon URL, or `""`. **URL only — no texture bytes.** Download + upload the texture yourself. |

**Per-type fields:**

| Link type | byte | Extra fields |
|---|---|---|
| Item | `0x02` | `bound` (DecoderBound), `noSell`, `tradeable`, `vendorValue` (copper) |
| Skill | `0x06` | `description[512]`, `factCount`, `facts[16]` (icon URL + pre-formatted text per fact) |
| Waypoint/POI | `0x04` | `mapName[96]`, `poiType` (DecoderPoiKind) |
| Build/AE2 | `0x0D` | Spec label in `name[]`; no additional fields |
| Skin | `0x0A` | `name[]` and `iconUrl[]` only |

---

## 4. Miss event — `EV_DECODER_RING_RESOLVED`

When `Resolve` returns `DR_NotReady`, a background fetch is in flight. Subscribe
`EV_DECODER_RING_RESOLVED` to receive the record when it lands.

**The payload is a `DecoderRecord*` delivered synchronously.** The pointer is only valid for the
duration of the handler — copy what you need before returning.

```cpp
static void OnResolved(void* payload) {
    if (!payload) return;
    auto* rec = static_cast<const DecoderRecord*>(payload);

    // Verify the schema version matches what you compiled against.
    if (rec->schemaVersion != DECODER_RING_API_VERSION) return;

    // Match to a pending request using the (linkType, id) correlation key.
    if (rec->linkType == myPendingLinkType && rec->id == myPendingId) {
        // Copy — the pointer is only valid during this call.
        myRecord = *rec;
        myPendingLinkType = 0;
        myPendingId = 0;
    }
}

// In AddonLoad:
APIDefs->Events_Subscribe(EV_DECODER_RING_RESOLVED, OnResolved);

// In AddonUnload:
APIDefs->Events_Unsubscribe(EV_DECODER_RING_RESOLVED, OnResolved);
```

---

## 5. Volatile price — `QueryPrice`

Trading-post prices are kept separately from the durable record. They are in-memory only on the
service with a short TTL (~5 minutes) and are never written to disk.

```cpp
DecoderStatus (*QueryPrice)(uint32_t itemId, DecoderPrice* out);
```

Returns `DR_Resolved` with `*out` filled, or `DR_NotReady` if a fetch is in flight. There is no
miss event for prices — poll on the next frame or tooltip refresh.

```cpp
DecoderRingApi* decoder = GetDecoder();   // re-validate every call — never cache
DecoderPrice price{};
if (decoder && decoder->QueryPrice(itemId, &price) == DR_Resolved) {
    // price.buy  = highest buy order, copper (-1 if no listings)
    // price.sell = lowest sell listing, copper (-1 if no listings)
    RenderPrice(price.buy, price.sell);
}
```

---

## 6. Graceful-degradation contract

**Detecting absence:** `GetDecoder()` returns `nullptr` whenever `DataLink_Get` yields no struct,
or yields the post-unload struct whose `apiVersion` is now `0`. Call `GetDecoder()` before every
use — never a cached pointer — so "absent" and "just unloaded" are handled identically and safely.

**What still works without the service:**

Your addon should vendor `ChatLinks` (the codec in `src/chat/`) to decode chat-link structure
locally. This gives you type bytes, integer IDs, and byte spans for every link type regardless of
whether Decoder Ring is loaded. With vendored data:

- **Build links:** The spec label (profession + elite spec) can be derived offline from vendored
  `SpecData.h` — no network needed.
- **Waypoint/POI links:** Names can be resolved from a vendored waypoint table — no network needed.
- **Structural rendering:** You can always render a chip showing the link type and raw id.

**What goes missing without the service:**

Only API-resolved names and metadata for item, skin, and skill links go missing — those require
the GW2 `/v2` API and the service's disk cache. Render these as "unknown item" / "unknown skill"
placeholders rather than crashing or blocking.

**The rule:** A consumer must never crash because the service is absent. Guard every call site
through `GetDecoder()`:

```cpp
DecoderRingApi* decoder = GetDecoder();
if (!decoder) {
    // Fall back to structural / vendored rendering.
    RenderFallback(linkType, id);
    return;
}
DecoderRecord rec{};
decoder->Resolve(linkType, id, chatCode, &rec);
// ...
```

**Version mismatch:** If `DataLink_Get` returns non-null but `apiVersion !=
DECODER_RING_API_VERSION`, treat it as absent — the struct layout may have changed (or the service
just unloaded and zeroed it) and calling through mismatched/null pointers is undefined behaviour.
`GetDecoder()` already folds this check in. Do not store the pointer.

---

## 7. Consumer lifetime contract (REQUIRED — prevents crashes)

Decoder Ring is an optional provider that can **load or unload at any time** — disabled in the
Nexus addon manager, updated, or crashed. "Service absent" is a normal runtime state, not an error.
The provider hardens itself (it zeroes its published pointers and raises `EV_DECODER_RING_UNLOADING`
on the way out), but a consumer that caches a raw function pointer can still crash itself. These
four rules are mandatory; they are the canonical pattern every consumer should copy.

1. **Decoder Ring may appear and disappear at any time.** Tolerate both, live, with no game restart.
   It can also reappear (re-enabled) — handle the round trip absent → present → absent → present.

2. **Never cache the exported function pointer across calls.** The `DecoderRingApi*` from
   `DataLink_Get` points *into Decoder Ring's DLL code*. When the addon unloads, that code is gone
   but the DataLink block (Nexus-owned) remains — so a stale pointer still looks non-null and
   jumping through it crashes the game. **Re-resolve via `GetDecoder()` immediately before every
   use** and only call when it returns non-null. A cheap registry lookup per call is the price of
   safety.

3. **Subscribe both lifecycle events and drop references on disappearance.** Subscribe
   `EV_DECODER_RING_READY` (appearance) to refresh your "present" indicator, and
   `EV_DECODER_RING_UNLOADING` (disappearance) to immediately mark the service absent and discard
   any cached record or pending correlation key. Re-subscription is not needed across a toggle —
   your subscriptions persist for your own addon's lifetime; only your *view* of presence changes.

4. **With Decoder Ring absent, degrade to local structural decode and never crash.** Vendor
   `ChatLinks` (`src/chat/`) to decode link structure offline (type byte, id, byte spans), derive
   build labels from `SpecData.h` and waypoint names from the vendored table, and render
   "unknown item/skin/skill" placeholders for the API-only fields. Absence is a rendering mode, not
   a failure.

**Presence indicators must be live, not latched.** If your UI shows "Decoder Ring: present", drive
it from `GetDecoder() != nullptr` (or the READY/UNLOADING events), re-evaluated each frame — never
from a value captured once at load. A latched indicator is how a consumer convinces itself the
service is present after it has unloaded, and then calls into dead code.

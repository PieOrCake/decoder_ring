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

**What your addon must do on its own load (in this order):**

```cpp
static DecoderRingApi* g_decoder = nullptr;

// Called when Decoder Ring announces (or re-announces) that its API is live.
static void OnDecoderReady(void*) {
    auto* api = static_cast<DecoderRingApi*>(
        APIDefs->DataLink_Get(DECODER_RING_DATALINK));
    if (api && api->apiVersion == DECODER_RING_API_VERSION)
        g_decoder = api;
}

// In your AddonLoad:
void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;

    // 1. Subscribe the READY event before doing anything else.
    APIDefs->Events_Subscribe(EV_DECODER_RING_READY, OnDecoderReady);

    // 2. Try DataLink_Get immediately — covers the case where Decoder loaded first
    //    and you already missed the READY event.
    auto* api = static_cast<DecoderRingApi*>(
        APIDefs->DataLink_Get(DECODER_RING_DATALINK));
    if (api && api->apiVersion == DECODER_RING_API_VERSION)
        g_decoder = api;

    // 3. Raise a PING — covers the case where Decoder loads after you do.
    //    Decoder will respond by re-raising EV_DECODER_RING_READY, which fires
    //    your handler above.
    APIDefs->Events_Raise(EV_DECODER_RING_PING, nullptr);
}
```

**Why this cannot deadlock:**
- Decoder first → step 2 finds the pointer immediately; the event is belt-and-suspenders.
- Consumer first → step 2 returns null, but Decoder's later load triggers the PING reply, which
  fires `OnDecoderReady`.
- Decoder absent forever → `DataLink_Get` stays null and no READY event ever fires. `g_decoder`
  remains null and the consumer falls back gracefully (see section 6).

On unload, unsubscribe the event:

```cpp
APIDefs->Events_Unsubscribe(EV_DECODER_RING_READY, OnDecoderReady);
g_decoder = nullptr;
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
    if (!g_decoder) { RenderFallback(linkType, id); return; }

    DecoderRecord rec{};
    DecoderStatus status = g_decoder->Resolve(linkType, id, chatCode, &rec);

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
DecoderPrice price{};
if (g_decoder && g_decoder->QueryPrice(itemId, &price) == DR_Resolved) {
    // price.buy  = highest buy order, copper (-1 if no listings)
    // price.sell = lowest sell listing, copper (-1 if no listings)
    RenderPrice(price.buy, price.sell);
}
```

---

## 6. Graceful-degradation contract

**Detecting absence:** `g_decoder` remains `nullptr` if `DataLink_Get` never returns a valid
pointer and no `EV_DECODER_RING_READY` event fires. Check `g_decoder` before every call —
the function pointers simply do not exist when the service is absent.

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

**The rule:** A consumer must never crash because `g_decoder` is null. Guard every call site:

```cpp
if (!g_decoder) {
    // Fall back to structural / vendored rendering.
    RenderFallback(linkType, id);
    return;
}
DecoderRecord rec{};
g_decoder->Resolve(linkType, id, chatCode, &rec);
// ...
```

**Version mismatch:** If `DataLink_Get` returns non-null but `api->apiVersion !=
DECODER_RING_API_VERSION`, treat it as absent — the struct layout may have changed and calling
through mismatched function pointers is undefined behaviour. Do not store the pointer.

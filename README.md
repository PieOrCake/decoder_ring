# Decoder Ring

A headless Nexus addon that resolves GW2 chat-link IDs into metadata records and serves them to other addons.

## AI Notice

This addon has been largely created using Claude. I understand that some folks have a moral, financial or political objection to creating software using an LLM. I just wanted to make a useful tool for the GW2 community, and this was the only way I could do it.

If an LLM creating software upsets you, then perhaps this repo isn't for you. Move on, and enjoy your day.

## Features

It handles items, skins, and skills via the GW2 `/v2` API (async, disk-cached), and
resolves build/AE2 labels and waypoint/POI names offline from compiled-in data. It draws nothing
and passes icon URLs — not textures — across the boundary; each consumer downloads and uploads its
own texture.

**Resolved link types:** item, skin, skill (via `/v2` API) · build/AE2 spec label · waypoint/POI
name · volatile trading-post price (in-memory, ~5-min TTL, separate from the durable record).

- **Items** carry full tooltip data: name, icon, rarity tier (Junk through Legendary), bound/vendor
  info, flavour text, and pre-formatted stat lines (defense or weapon strength, attributes, infusion
  slots, rune/sigil bonuses, weight, required level).
- **Skills** that the `/v2` API doesn't have (mount, siege-turtle, transform and other `.dat`-only
  skills) are named and described via a wiki fallback, so they resolve too.
- **Effect & buff links** that players shift-click (food/nourishment, guild & WvW boosts) are named
  from the wiki (English), so they resolve instead of showing a bare chip.
- Resolves standalone **trait** chat links into a full tooltip (name, icon, description, facts).
- **Recipes** resolve to "Recipe: \<item\>" with the full ingredient list and required crafting
  rating.
- **Localization:** Resolves names, descriptions and tooltips in your Nexus language (English,
  German, French, Spanish), following the Nexus language switch.

**API:** Two coupled halves, both keyed by `(linkType, id)`. Call `api->Resolve()` for an
immediate warm result; subscribe `EV_DECODER_RING_RESOLVED` for the async completion when the
cache is cold. See [docs/API.md](docs/API.md) for the full integration guide including the
load-order-independent ready handshake and graceful-degradation contract.

## Build

**Windows DLL (MinGW cross-compile):**
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

**Linux host unit tests only:**
```bash
mkdir build-host && cd build-host
cmake -DDECODER_TESTS_ONLY=ON ..
make -j$(nproc)
./decoder_tests
```

## Deployment

Copy the built `DecoderRing.dll` into your Nexus addons folder. This project never deploys it
automatically.

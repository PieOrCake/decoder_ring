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

**Resolved link types:** item (name, icon, and rarity tier — Junk through Legendary), skin, skill
(via `/v2` API) · build/AE2 spec label · waypoint/POI name · volatile trading-post price (in-memory,
~5-min TTL, separate from the durable record).

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

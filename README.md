# Decoder Ring

A headless Nexus addon that resolves GW2 chat-link IDs into metadata records and serves them to
other addons. It handles items, skins, and skills via the GW2 `/v2` API (async, disk-cached), and
resolves build/AE2 labels and waypoint/POI names offline from compiled-in data. It draws nothing
and passes icon URLs — not textures — across the boundary; each consumer downloads and uploads its
own texture.

**Resolved link types:** item, skin, skill (via `/v2` API) · build/AE2 spec label · waypoint/POI
name · volatile trading-post price (in-memory, ~5-min TTL, separate from the durable record).

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

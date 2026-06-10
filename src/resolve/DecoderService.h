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

    static void SleepMs(int ms);

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

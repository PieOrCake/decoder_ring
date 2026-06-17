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
static DecoderStatus Api_Resolve(uint8_t type, uint32_t id, const char* chatCode, DecoderRecord* out) {
    if (!out) return DR_Failed;
    return g_service.Resolve(type, id, chatCode ? std::string(chatCode) : std::string(), *out);
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
    // (1) Neutralise the published function table FIRST. The DataLink block is
    // owned by Nexus and OUTLIVES this DLL — there is no DataLink_Free — so a
    // consumer that calls DataLink_Get after we unload gets this very struct back.
    // Left intact, its pointers would aim into unloaded code => crash on call.
    // Zeroing them makes a re-reading consumer see apiVersion 0 / null and decline.
    if (g_api) { g_api->apiVersion = 0; g_api->Resolve = nullptr; g_api->QueryPrice = nullptr; }

    // (2) Signal disappearance (mirror of the READY handshake) so subscribed
    // consumers drop any cached pointer immediately, not just on their next probe.
    if (APIDefs && APIDefs->Events_Raise) APIDefs->Events_Raise(EV_DECODER_RING_UNLOADING, nullptr);

    // (3) Stop our render pump and our own subscription so nothing of ours fires
    // mid- or post-teardown.
    if (APIDefs && APIDefs->GUI_Deregister) APIDefs->GUI_Deregister(AddonRender);
    if (APIDefs && g_subscribed && APIDefs->Events_Unsubscribe)
        APIDefs->Events_Unsubscribe(EV_DECODER_RING_PING, OnPing);
    g_subscribed = false;

    // (4) Join worker threads. Shutdown() blocks until any in-flight fetch returns,
    // so no fetch completes into freed state and no thread outlives the DLL.
    g_service.Shutdown();

    g_api = nullptr;          // block itself stays alive in Nexus; we just drop our handle
    APIDefs = nullptr;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    AddonDef.Signature = 0x44524e47; // "DRNG"
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = DR_DISPLAY_NAME;
    AddonDef.Version.Major = 0; AddonDef.Version.Minor = 9;
    AddonDef.Version.Build = 1; AddonDef.Version.Revision = 0;
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

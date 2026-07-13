#include "AddonShared.h"
#include "DecoderRingApi.h"
#include "resolve/DecoderService.h"
#include "resolve/Http.h"
#include "resolve/Language.h"
#include <windows.h>
#include <string>
#include "imgui.h"
#include "nlohmann/json.hpp"
#include <fstream>

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

// Detect the active Nexus language. Nexus exposes no direct getter, but its
// Localization system translates an identifier into the current active language.
// We register a sentinel whose value in each language IS that language's code, so
// Localization_Translate hands back "en"/"de"/"fr"/"es" when active (and the raw
// identifier for any unsupported language -> MapNexusToApi maps that to "en").
static constexpr const char* DR_LANG_SENTINEL = "DR_ActiveLanguageProbe";
static std::string g_lastApiLang;   // last language pushed to the service

// "auto" follows the Nexus language (default). Otherwise a forced en/de/fr/es.
static std::string g_langOverride = "auto";
static std::string g_addonDir;   // captured at load, for settings persistence

// Map the effective language to one of four IMMORTAL string literals (never a pointer
// into g_lastApiLang), so there is no lifetime hazard and no per-call allocation.
// g_lastApiLang is only ever "en"/"de"/"fr"/"es" (or empty before the first poll).
static const char* LangLiteral(const std::string& l) {
    if (l == "de") return "de";
    if (l == "fr") return "fr";
    if (l == "es") return "es";
    return "en";   // "en", empty (pre-first-poll), or any unexpected value -> English
}
static const char* Api_GetActiveLanguage() { return LangLiteral(g_lastApiLang); }

// Announce that the effective resolution language changed. Payload is the same
// immortal literal GetActiveLanguage() returns (valid for the handler's duration).
static void RaiseLanguageChanged() {
    if (APIDefs && APIDefs->Events_Raise)
        APIDefs->Events_Raise(EV_DECODER_RING_LANGUAGE_CHANGED, (void*)LangLiteral(g_lastApiLang));
}

static void RegisterLanguageSentinels() {
    if (!APIDefs || !APIDefs->Localization_Set) return;
    APIDefs->Localization_Set(DR_LANG_SENTINEL, "en", "en");
    APIDefs->Localization_Set(DR_LANG_SENTINEL, "de", "de");
    APIDefs->Localization_Set(DR_LANG_SENTINEL, "fr", "fr");
    APIDefs->Localization_Set(DR_LANG_SENTINEL, "es", "es");
}

static void PollLanguage() {
    if (g_langOverride != "auto") return;   // manual override wins over auto-detect
    if (!APIDefs || !APIDefs->Localization_Translate) return;
    const char* active = APIDefs->Localization_Translate(DR_LANG_SENTINEL);
    std::string api = Decoder::MapNexusToApi(active);   // handles the raw-identifier passthrough
    if (api != g_lastApiLang) { g_lastApiLang = api; g_service.SetLanguage(api); RaiseLanguageChanged(); }
}

// Apply the current selection: a forced language wins and suppresses the poll;
// "auto" hands control back to detection (re-read next PollLanguage).
static void ApplyLanguageSelection() {
    if (g_langOverride != "auto") {
        bool changed = (g_lastApiLang != g_langOverride);
        g_service.SetLanguage(g_langOverride);
        g_lastApiLang = g_langOverride;   // keep the poll's cache consistent
        if (changed) RaiseLanguageChanged();
    } else {
        g_lastApiLang.clear();            // force PollLanguage to re-detect + apply next frame
                                          // (PollLanguage raises the change event when it lands)
    }
}

// --- Settings persistence (just the language override) ---
static std::string SettingsPath() { return g_addonDir + "/settings.json"; }

static void LoadSettings() {
    try {
        std::ifstream f(SettingsPath());
        if (!f) return;
        nlohmann::json j; f >> j;
        if (j.is_object() && j.contains("language") && j["language"].is_string()) {
            std::string v = j["language"].get<std::string>();
            if (v=="auto"||v=="en"||v=="de"||v=="fr"||v=="es") g_langOverride = v;
        }
    } catch (...) {}
}
static void SaveSettings() {
    try {
        std::ofstream f(SettingsPath(), std::ios::trunc);
        if (f) f << nlohmann::json{{"language", g_langOverride}}.dump();
    } catch (...) {}
}

// Options panel: draws in the Nexus addon-options area (RT_OptionsRender).
static void AddonOptions() {
    ImGui::TextDisabled("Resolution language");
    struct Opt { const char* label; const char* code; };
    static const Opt kOpts[] = {
        {"Auto (follow Nexus)", "auto"},
        {"English",  "en"},
        {"Deutsch",  "de"},
        {"Français", "fr"},
        {"Español",  "es"},
    };
    for (const auto& o : kOpts) {
        if (ImGui::RadioButton(o.label, g_langOverride == o.code)) {
            g_langOverride = o.code;
            ApplyLanguageSelection();
            SaveSettings();
        }
    }
    ImGui::TextDisabled("Shift-click a chat link again after switching to see it re-resolved.");
}

// Render callback: draws NOTHING. Pure main-thread pump (drain completions ->
// raise events; throttled disk flush).
static void AddonRender() { PollLanguage(); g_service.Tick(); }

static void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;

    const char* dir = APIDefs->Paths_GetAddonDirectory ? APIDefs->Paths_GetAddonDirectory("DecoderRing") : nullptr;
    g_addonDir = dir ? dir : ".";
    LoadSettings();                       // may set g_langOverride
    g_service.Initialize(g_addonDir, &Decoder::WinINetFetch, &OnCompletion);

    // Publish the exported function table in shared memory (version FIRST).
    if (APIDefs->DataLink_Share) {
        g_api = (DecoderRingApi*)APIDefs->DataLink_Share(DECODER_RING_DATALINK, sizeof(DecoderRingApi));
        if (g_api) {
            g_api->apiVersion = DECODER_RING_API_VERSION;
            g_api->Resolve = &Api_Resolve;
            g_api->QueryPrice = &Api_QueryPrice;
            g_api->GetActiveLanguage = &Api_GetActiveLanguage;
        }
    }

    // ImGui bootstrap (needed to draw in the Nexus options panel).
    if (APIDefs->ImguiContext) {
        ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
        ImGui::SetAllocatorFunctions(
            (void* (*)(size_t, void*))APIDefs->ImguiMalloc,
            (void (*)(void*, void*))APIDefs->ImguiFree);
    }

    // Render-pump (no drawing) + options panel + ready handshake (mirror AlterEgoBridge).
    if (APIDefs->GUI_Register) { APIDefs->GUI_Register(RT_Render, AddonRender); }
    if (APIDefs->GUI_Register) { APIDefs->GUI_Register(RT_OptionsRender, AddonOptions); }
    if (APIDefs->Events_Subscribe) { APIDefs->Events_Subscribe(EV_DECODER_RING_PING, OnPing); g_subscribed = true; }
    if (APIDefs->Events_Raise) APIDefs->Events_Raise(EV_DECODER_RING_READY, nullptr);

    RegisterLanguageSentinels();
    ApplyLanguageSelection();             // apply the loaded override, or (auto) defer to detection
}

static void AddonUnload() {
    // (1) Neutralise the published function table FIRST. The DataLink block is
    // owned by Nexus and OUTLIVES this DLL — there is no DataLink_Free — so a
    // consumer that calls DataLink_Get after we unload gets this very struct back.
    // Left intact, its pointers would aim into unloaded code => crash on call.
    // Zeroing them makes a re-reading consumer see apiVersion 0 / null and decline.
    if (g_api) { g_api->apiVersion = 0; g_api->Resolve = nullptr; g_api->QueryPrice = nullptr; g_api->GetActiveLanguage = nullptr; }

    // (2) Signal disappearance (mirror of the READY handshake) so subscribed
    // consumers drop any cached pointer immediately, not just on their next probe.
    if (APIDefs && APIDefs->Events_Raise) APIDefs->Events_Raise(EV_DECODER_RING_UNLOADING, nullptr);

    // (3) Stop our render pump and our own subscription so nothing of ours fires
    // mid- or post-teardown.
    if (APIDefs && APIDefs->GUI_Deregister) { APIDefs->GUI_Deregister(AddonRender); APIDefs->GUI_Deregister(AddonOptions); }
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
    AddonDef.Version.Build = 5; AddonDef.Version.Revision = 0;
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

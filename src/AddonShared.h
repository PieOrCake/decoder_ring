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

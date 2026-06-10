#include "resolve/Http.h"
#include <windows.h>
#include <wininet.h>

namespace Decoder {
// Production HttpFetch — WinINet, mirroring Pie UI's cache HttpGet.
bool WinINetFetch(const std::string& url, std::vector<char>& out) {
    HINTERNET hI = InternetOpenA("DecoderRing/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hI) return false;
    // Bound every blocking phase so a hung socket FAILS within a few seconds instead
    // of stranding the request (no terminal event) and blocking Shutdown()'s worker
    // join (game freeze on unload). 5s each is generous for a single small /v2 GET
    // yet short enough that a stall surfaces as a normal retryable failure promptly.
    DWORD timeoutMs = 5000;
    InternetSetOptionA(hI, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(hI, INTERNET_OPTION_SEND_TIMEOUT,    &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(hI, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    HINTERNET hU = InternetOpenUrlA(hI, url.c_str(), NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hU) { InternetCloseHandle(hI); return false; }
    out.clear();
    char chunk[4096]; DWORD n = 0;
    while (InternetReadFile(hU, chunk, sizeof(chunk), &n) && n > 0)
        out.insert(out.end(), chunk, chunk + n);
    InternetCloseHandle(hU); InternetCloseHandle(hI);
    return !out.empty();
}
}

#include "resolve/Http.h"
#include <windows.h>
#include <wininet.h>

namespace Decoder {
// Production HttpFetch — WinINet, mirroring Pie UI's cache HttpGet.
bool WinINetFetch(const std::string& url, std::vector<char>& out) {
    HINTERNET hI = InternetOpenA("DecoderRing/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hI) return false;
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

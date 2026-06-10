#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Decoder {
// Injectable HTTP GET. Returns true and fills `out` with the response body on
// success; false on any failure. Production wires WinINet (Http_WinINet.cpp);
// tests inject a fake. The pure core depends ONLY on this typedef.
using HttpFetch = std::function<bool(const std::string& url, std::vector<char>& out)>;
}

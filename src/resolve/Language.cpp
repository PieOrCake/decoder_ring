#include "resolve/Language.h"
#include <cctype>

namespace Decoder {
std::string MapNexusToApi(const char* nexusCode) {
    if (!nexusCode) return "en";
    char a = (char)std::tolower((unsigned char)nexusCode[0]);
    if (a == '\0') return "en";
    char b = (char)std::tolower((unsigned char)nexusCode[1]);   // safe: nexusCode[0] != '\0'
    if (a == 'd' && b == 'e') return "de";
    if (a == 'f' && b == 'r') return "fr";
    if (a == 'e' && b == 's') return "es";
    return "en";   // en, and every unsupported language, resolve as English
}
}

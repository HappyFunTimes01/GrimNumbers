// Small string helpers shared by the config parsers and the renderers.
// Header-only: all three callers want the same four lines, and a translation
// unit for them would be larger than they are.
#pragma once

#include <windows.h>

#include <cctype>
#include <string>

namespace grimnumbers {

inline std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Config files and the wire protocol are both UTF-8; the Win32 drawing calls
// want UTF-16.
inline std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();

    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();

    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

} // namespace grimnumbers

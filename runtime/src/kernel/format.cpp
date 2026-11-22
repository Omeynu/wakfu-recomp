// Formateur printf invité (sans dépendance aux imports) : voir strings.h.
#include <cstdio>
#include <cstring>
#include "../guest.h"
#include "strings.h"

namespace wk {

std::u16string FormatGuest(const std::u16string& fmt, bool wide, ArgSource& args) {
    std::u16string out;
    auto put = [&](const std::string& s) { for (unsigned char c : s) out.push_back(c); };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != u'%') { out.push_back(fmt[i]); continue; }
        if (++i >= fmt.size()) break;
        if (fmt[i] == u'%') { out.push_back(u'%'); continue; }
        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ #0", char(fmt[i]))) spec += char(fmt[i++]);
        if (i < fmt.size() && fmt[i] == u'*') { spec += std::to_string(int32_t(args.Next())); i++; }
        while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9') spec += char(fmt[i++]);
        int precision = -1;
        if (i < fmt.size() && fmt[i] == u'.') {
            spec += '.';
            i++;
            if (i < fmt.size() && fmt[i] == u'*') { precision = int32_t(args.Next()); spec += std::to_string(precision); i++; }
            else { precision = 0; while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9') { precision = precision * 10 + (fmt[i] - u'0'); spec += char(fmt[i++]); } }
        }
        int size = 0;   // 0 défaut, 1 h, 2 l, 3 ll/I64
        bool forceWide = false, forceNarrow = false;
        for (;;) {
            if (i >= fmt.size()) break;
            const char16_t c = fmt[i];
            if (c == u'h') { size = 1; forceNarrow = true; i++; }
            else if (c == u'l') { size = size == 2 ? 3 : 2; forceWide = true; i++; }
            else if (c == u'w') { forceWide = true; i++; }
            else if (c == u'I' && i + 2 < fmt.size() && fmt[i + 1] == u'6' && fmt[i + 2] == u'4') { size = 3; i += 3; }
            else break;
        }
        if (i >= fmt.size()) break;
        const char conv = char(fmt[i]);
        char buf[512];
        switch (conv) {
            case 'd': case 'i': {
                const uint64_t v = args.Next();
                const long long n = size == 3 ? (long long)int64_t(v) : size == 1 ? (long long)int16_t(v) : (long long)int32_t(v);
                snprintf(buf, sizeof buf, (spec + "lld").c_str(), n);
                put(buf);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                const uint64_t v = args.Next();
                const unsigned long long n = size == 3 ? v : size == 1 ? uint16_t(v) : uint32_t(v);
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), n);
                put(buf);
                break;
            }
            case 'p':
                snprintf(buf, sizeof buf, "%08X", uint32_t(args.Next()));
                put(buf);
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                const uint64_t v = args.Next();
                double d;
                memcpy(&d, &v, 8);
                snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
                put(buf);
                break;
            }
            case 'c': case 'C': {
                const bool w = conv == 'C' ? !wide : wide;
                const uint32_t v = uint32_t(args.Next());
                out.push_back(char16_t((w || forceWide) && !forceNarrow ? (v & 0xFFFF) : (v & 0xFF)));
                break;
            }
            case 's': case 'S': {
                const bool w = forceWide ? true : forceNarrow ? false : (conv == 'S' ? !wide : wide);
                const uint32_t p = uint32_t(args.Next());
                std::u16string s;
                if (!p) s = u"(null)";
                else for (uint32_t k = 0; precision < 0 || k < uint32_t(precision); k++) {
                    const char16_t ch = w ? char16_t(Ld16(p + 2 * k)) : char16_t(Ld8(p + k));
                    if (!ch) break;
                    s.push_back(ch);
                }
                // largeur et alignement
                std::string flags = spec.substr(1);
                const bool left = flags.find('-') != std::string::npos;
                size_t width = 0;
                for (char c : flags) if (c >= '0' && c <= '9') { width = 0; break; }
                {
                    size_t k = 1, wv = 0;
                    while (k < spec.size() && strchr("-+ #0", spec[k])) k++;
                    while (k < spec.size() && spec[k] >= '0' && spec[k] <= '9') wv = wv * 10 + size_t(spec[k++] - '0');
                    width = wv;
                }
                if (!left) while (s.size() < width) s.insert(s.begin(), u' ');
                out += s;
                if (left) for (size_t k = s.size(); k < width; k++) out.push_back(u' ');
                break;
            }
            case 'n': args.Next(); break;
            default: out.push_back(u'%'); out.push_back(fmt[i]);
        }
    }
    return out;
}

}  // namespace wk

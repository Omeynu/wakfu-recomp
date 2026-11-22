#pragma once
#include <cstdint>
#include <string>

namespace wk {
// Source des arguments variables (emplacements de 64 bits)
struct ArgSource {
    virtual ~ArgSource() = default;
    virtual uint64_t Next() = 0;
};
// Formatage printf (format en UTF-16 ; wide = fonction « large » : %s UTF-16, %S étroit).
// Les chaînes pointées sont lues en mémoire invitée.
std::u16string FormatGuest(const std::u16string& fmt, bool wide, ArgSource& args);
}

// Formatage de chaînes du noyau (sprintf, swprintf, _snwprintf, _vsnprintf, vswprintf, _vscwprintf).
// Arguments variables Xbox 360 : emplacements de 64 bits, r3-r10 puis pile (r1+0x50) ; va_list = pointeur
// vers des emplacements de 64 bits. Fonctions « larges » : %s = UTF-16, %S = étroit (inverse en étroit).
#include "strings.h"
#include <cstdio>
#include <cstring>
#include "../process.h"
#include "stub.h"

namespace wk {

namespace {
struct RegArgs : ArgSource {
    PPCContext& ctx;
    int index;
    RegArgs(PPCContext& c, int first) : ctx(c), index(first) {}
    uint64_t Next() override {
        const int n = index++;
        if (n < 8) {
            const PPCRegister* r[8] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
            return r[n]->u64;
        }
        return Ld64(ctx.r1.u32 + 0x50 + (n - 8) * 8);
    }
};
struct VaArgs : ArgSource {
    uint32_t ptr;
    int index = 0;
    explicit VaArgs(uint32_t p) : ptr(p) {}
    uint64_t Next() override { return Ld64(ptr + 8 * uint32_t(index++)); }
};
std::u16string ReadFmt(uint32_t p, bool wide) {
    std::u16string s;
    if (!p) return s;
    for (uint32_t k = 0; k < 65536; k++) {
        const char16_t c = wide ? char16_t(Ld16(p + 2 * k)) : char16_t(Ld8(p + k));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}
// Écrit `s` (avec zéro final si la place le permet) ; count = UINT32_MAX : pas de limite.
// Retour : nombre de caractères, ou -1 si tronqué (sémantique _snprintf).
int32_t Store(uint32_t buf, uint32_t count, const std::u16string& s, bool wide) {
    const size_t n = s.size();
    const size_t written = count == UINT32_MAX ? n : std::min<size_t>(n, count);
    for (size_t k = 0; k < written; k++) { if (wide) St16(buf + 2 * uint32_t(k), s[k]); else St8(buf + uint32_t(k), uint8_t(s[k])); }
    if (count == UINT32_MAX || n < count) { if (wide) St16(buf + 2 * uint32_t(n), 0); else St8(buf + uint32_t(n), 0); }
    return (count != UINT32_MAX && n > count) ? -1 : int32_t(n);
}
}  // namespace
}  // namespace wk

using namespace wk;

WK_IMPORT(sprintf) {
    WK_TRACE(sprintf);
    RegArgs a(ctx, 2);
    Ret(ctx, uint32_t(Store(ctx.r3.u32, UINT32_MAX, FormatGuest(ReadFmt(ctx.r4.u32, false), false, a), false)));
}
WK_IMPORT(swprintf) {
    WK_TRACE(swprintf);
    RegArgs a(ctx, 2);
    Ret(ctx, uint32_t(Store(ctx.r3.u32, UINT32_MAX, FormatGuest(ReadFmt(ctx.r4.u32, true), true, a), true)));
}
WK_IMPORT(_snwprintf) {
    WK_TRACE(_snwprintf);
    RegArgs a(ctx, 3);
    Ret(ctx, uint32_t(Store(ctx.r3.u32, ctx.r4.u32, FormatGuest(ReadFmt(ctx.r5.u32, true), true, a), true)));
}
WK_IMPORT(_vsnprintf) {
    WK_TRACE(_vsnprintf);
    VaArgs a(ctx.r6.u32);
    Ret(ctx, uint32_t(Store(ctx.r3.u32, ctx.r4.u32, FormatGuest(ReadFmt(ctx.r5.u32, false), false, a), false)));
}
WK_IMPORT(vswprintf) {
    WK_TRACE(vswprintf);
    VaArgs a(ctx.r5.u32);
    Ret(ctx, uint32_t(Store(ctx.r3.u32, UINT32_MAX, FormatGuest(ReadFmt(ctx.r4.u32, true), true, a), true)));
}
WK_IMPORT(_vscwprintf) {
    WK_TRACE(_vscwprintf);
    VaArgs a(ctx.r4.u32);
    Ret(ctx, uint32_t(FormatGuest(ReadFmt(ctx.r3.u32, true), true, a).size()));
}

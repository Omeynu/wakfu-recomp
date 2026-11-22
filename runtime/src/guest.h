// Mémoire invitée (4 Go big-endian) et accès aux arguments des imports.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include "ppc_context.h"

namespace wk {

extern uint8_t* g_base;

inline uint8_t* Host(uint32_t a) { return g_base + a; }
inline uint8_t Ld8(uint32_t a) { return g_base[a]; }
inline uint16_t Ld16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t Ld32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t Ld64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void St8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void St16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void St32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void St64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

// Chaînes invitées (ANSI et UTF-16BE) -> std::string (UTF-16 réduit à l'ASCII, '?' sinon).
std::string ReadAnsi(uint32_t a, size_t max = 4096);
std::string ReadWide(uint32_t a, size_t max = 4096);

// Tas invités à pointeur croissant (pas de libération au jalon 1) ; 0 si épuisé.
enum class Heap { Kernel, Virtual, Physical, Stack };
uint32_t Alloc(Heap h, uint32_t size, uint32_t align = 16);

// Statuts NT courants
constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_TIMEOUT = 0x102;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_NOT_FOUND = 0xC0000225;

inline void Ret(PPCContext& ctx, uint64_t v) { ctx.r3.u64 = v; }

// LR invité pour les diagnostics ; 0 quand skip_lr (config/wakfu.toml) retire LR du contexte.
inline uint32_t DiagLr(const PPCContext& ctx) {
#ifdef PPC_CONFIG_SKIP_LR
    (void)ctx;
    return 0;
#else
    return uint32_t(ctx.lr);
#endif
}

}  // namespace wk

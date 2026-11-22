// Cales wakfu-recomp : définitions attendues par les fichiers Xenia repris (memory.cc, cvars).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include "xenia/base/memory.h"

namespace cvars { std::filesystem::path dump_shaders; }

namespace xe {
void copy_and_swap_16_unaligned(void* d, const void* s, size_t n) {
    auto* o = static_cast<uint8_t*>(d); auto* i = static_cast<const uint8_t*>(s);
    for (size_t k = 0; k < n; k++) { uint16_t v; memcpy(&v, i + 2 * k, 2); v = __builtin_bswap16(v); memcpy(o + 2 * k, &v, 2); }
}
void copy_and_swap_32_unaligned(void* d, const void* s, size_t n) {
    auto* o = static_cast<uint8_t*>(d); auto* i = static_cast<const uint8_t*>(s);
    for (size_t k = 0; k < n; k++) { uint32_t v; memcpy(&v, i + 4 * k, 4); v = __builtin_bswap32(v); memcpy(o + 4 * k, &v, 4); }
}
void copy_and_swap_64_unaligned(void* d, const void* s, size_t n) {
    auto* o = static_cast<uint8_t*>(d); auto* i = static_cast<const uint8_t*>(s);
    for (size_t k = 0; k < n; k++) { uint64_t v; memcpy(&v, i + 8 * k, 8); v = __builtin_bswap64(v); memcpy(o + 8 * k, &v, 8); }
}
void copy_and_swap_16_aligned(void* d, const void* s, size_t n) { copy_and_swap_16_unaligned(d, s, n); }
void copy_and_swap_32_aligned(void* d, const void* s, size_t n) { copy_and_swap_32_unaligned(d, s, n); }
void copy_and_swap_64_aligned(void* d, const void* s, size_t n) { copy_and_swap_64_unaligned(d, s, n); }
}  // namespace xe

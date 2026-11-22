// Inclus de force dans le C++ généré (build du runtime) : appels indirects vérifiés.
#pragma once
#include <cstdint>
struct PPCContext;
void WkCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target);
#define PPC_CALL_INDIRECT_FUNC(x) WkCallIndirect(ctx, base, (x))
// mftb (base de temps) : 50 MHz comme KeQueryPerformanceFrequency, au lieu de __rdtsc() brut (cf. core.cpp).
uint64_t WkQueryTimeBase();
#define PPC_QUERY_TIMEBASE() WkQueryTimeBase()

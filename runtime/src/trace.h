// Trace horodatée des événements d'une image (WAKFU_TRACE=<image de début>) : 65 536 événements à partir de cette
// image (compteur de VdSwap), écrits dans WAKFU_TRACE_FILE (défaut trace.txt) quand le tampon est plein.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace wk::trace {
struct Ev { uint64_t us; uint32_t a; char c; uint32_t tid; };
constexpr uint32_t kMax = 1u << 16;
inline Ev g_ev[kMax];
inline std::atomic<uint32_t> g_n{0};
inline std::atomic<uint32_t> g_frame{0};
inline uint32_t Start() { static const uint32_t s = getenv("WAKFU_TRACE") ? uint32_t(atoi(getenv("WAKFU_TRACE"))) : 0; return s; }
inline uint64_t Now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}
inline uint32_t Tid() { static std::atomic<uint32_t> next{0}; thread_local uint32_t id = next++; return id; }
inline void Dump() {
    const char* p = getenv("WAKFU_TRACE_FILE");
    if (FILE* f = fopen(p ? p : "trace.txt", "w")) {
        for (uint32_t i = 0; i < kMax; i++) fprintf(f, "%llu %u %c %08X\n", (unsigned long long)g_ev[i].us, g_ev[i].tid, g_ev[i].c, g_ev[i].a);
        fclose(f);
        fprintf(stderr, "[trace] %u événements écrits\n", kMax);
    }
}
// c : B/b jeu entre/sort de BlockOnFence, K anneau non vide, Q anneau vidé, W/w attente du CP, S swap (CP),
// P VdSwap (jeu), R lot retiré (fin GPU), U lot soumis, N présentation native (numéro du swap)
inline void Mark(char c, uint32_t a = 0) {
    if (!Start() || g_frame.load() < Start()) return;
    const uint32_t i = g_n.fetch_add(1);
    if (i < kMax) g_ev[i] = {Now(), a, c, Tid()};
    if (i == kMax - 1) Dump();
}
}  // namespace wk::trace

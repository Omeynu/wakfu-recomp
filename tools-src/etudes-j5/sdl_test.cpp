// Test hors jeu de audio_out avec SDL (pilote « disk ») : 1 s de trames 440 Hz cadencées comme le runtime.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include "kernel/audio_out.h"
using namespace wk::audio;
int main() {
    if (!Open()) return 1;
    static uint8_t frame[kFrameChannels * kFrameSamples * 4];
    int sent = 0, skipped = 0; double ph = 0;
    auto next = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 188; tick++) {
        next += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(next);
        if (QueuedFrames() > 8) { skipped++; continue; }
        for (int i = 0; i < kFrameSamples; i++, ph += 2 * M_PI * 440 / kRate) {
            float v = 0.5f * sinf(ph); uint32_t b; memcpy(&b, &v, 4); b = __builtin_bswap32(b);
            memcpy(frame + 4 * i, &b, 4);   // avant gauche seul
        }
        Submit(frame); sent++;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    printf("envoyées %d, sautées %d, en file %u\n", sent, skipped, QueuedFrames());
    return 0;
}

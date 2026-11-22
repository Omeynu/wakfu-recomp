// Sortie son (voir audio_out.h).
#include "../config.h"
#include "audio_out.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#ifdef WAKFU_HAS_SDL2
#include <SDL.h>
#endif

namespace wk::audio {

void DownmixToStereo(const uint8_t* in, float* out) {
    auto sample = [in](int channel, int i) {
        uint32_t b;
        memcpy(&b, in + 4 * (channel * kFrameSamples + i), 4);
        b = __builtin_bswap32(b);
        float f;
        memcpy(&f, &b, 4);
        return f;
    };
    constexpr float k = 0.70710678f;
    auto clamp = [](float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; };
    for (int i = 0; i < kFrameSamples; i++) {
        const float c = sample(2, i);
        out[2 * i] = clamp(sample(0, i) + k * (c + sample(4, i)));
        out[2 * i + 1] = clamp(sample(1, i) + k * (c + sample(5, i)));
    }
}

#ifdef WAKFU_HAS_SDL2
namespace {
SDL_AudioDeviceID g_device = 0;
constexpr uint32_t kFrameBytes = 2 * kFrameSamples * sizeof(float);
}  // namespace

bool Open() {
    // Statique locale (initialisation unique, thread-safe) plutôt que std::call_once, introuvable sous MinGW + Clang.
    static const bool opened = [] {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "[audio] SDL : %s (pas de son)\n", SDL_GetError());
            return false;
        }
        SDL_AudioSpec want{}, have{};
        want.freq = kRate;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = kFrameSamples;
        // Conversion éventuelle (fréquence, format) assurée par SDL : la file reste en 48 kHz stéréo flottant.
        g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!g_device) {
            fprintf(stderr, "[audio] périphérique : %s (pas de son)\n", SDL_GetError());
            return false;
        }
        fprintf(stderr, "[audio] sortie %s, %d Hz\n", SDL_GetCurrentAudioDriver(), have.freq);
        SDL_PauseAudioDevice(g_device, 0);
        return true;
    }();
    (void)opened;
    return g_device != 0;
}

void Submit(const uint8_t* guestFrame) {
    if (!g_device) return;
    float stereo[2 * kFrameSamples];
    DownmixToStereo(guestFrame, stereo);
    if (config::g.volume != 1.0f)   // option [audio] volume
        for (float& v : stereo) v *= config::g.volume;
    SDL_QueueAudio(g_device, stereo, kFrameBytes);
}

uint32_t QueuedFrames() { return g_device ? SDL_GetQueuedAudioSize(g_device) / kFrameBytes : 0; }
#else
bool Open() { return false; }
void Submit(const uint8_t*) {}
uint32_t QueuedFrames() { return 0; }
#endif

}  // namespace wk::audio

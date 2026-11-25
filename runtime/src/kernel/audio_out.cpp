// Sortie son (voir audio_out.h).
#include "../config.h"
#include "audio_out.h"
#include "window.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#ifdef WAKFU_HAS_SDL3
#include <SDL3/SDL.h>
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

#ifdef WAKFU_HAS_SDL3
namespace {
SDL_AudioStream* g_device = nullptr;
// File stéréo 48 kHz servie par le rappel du périphérique ; bilan pour le journal (Releve)
std::mutex g_mutex;
std::deque<float> g_file;
bool g_demarre = false;   // première trame reçue : avant, une file vide n'est pas une sous-alimentation
Bilan g_bilan;
// Appelé quand le périphérique demande des données (comme le rappel SDL2) : octets demandés, complétés de silence.
void Rappel(void*, SDL_AudioStream* flux, int octets, int) {
    if (octets <= 0) return;
    float out[16384];   // bloc de 1024 trames stéréo (plus si conversion de fréquence) ; au-delà, reste au prochain appel
    const size_t nb = std::min(size_t(octets) / sizeof(float), sizeof out / sizeof(float));
    std::lock_guard<std::mutex> l(g_mutex);
    const size_t k = std::min(nb, g_file.size());
    std::copy_n(g_file.begin(), k, out);
    g_file.erase(g_file.begin(), g_file.begin() + k);
    std::fill(out + k, out + nb, 0.0f);
    g_bilan.appels++;
    g_bilan.echantillonsParAppel = uint32_t(nb / 2);
    if (k < nb && g_demarre) {
        g_bilan.sousAlimentations++;
        g_bilan.manquants += (nb - k) / 2;
    }
    SDL_PutAudioStreamData(flux, out, int(nb * sizeof(float)));
}
}  // namespace

bool Open() {
    // Statique locale (initialisation unique, thread-safe) plutôt que std::call_once, introuvable sous MinGW + Clang.
    static const bool opened = [] {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[audio] SDL : %s (pas de son)\n", SDL_GetError());
            return false;
        }
        const SDL_AudioSpec want{SDL_AUDIO_F32, 2, kRate};
        // 1024 échantillons (21 ms) par appel du périphérique : le quantum par défaut de PipeWire ; 256 forçait tout
        // le graphe PipeWire à 5 ms (réveils fréquents, décrochages sur un portable chargé)
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
        // Conversion éventuelle (fréquence, format) assurée par SDL : la file reste en 48 kHz stéréo flottant.
        g_device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, Rappel, nullptr);
        if (!g_device) {
            fprintf(stderr, "[audio] périphérique : %s (pas de son)\n", SDL_GetError());
            return false;
        }
        SDL_AudioSpec have{};
        int trames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_device), &have, &trames);
        fprintf(stderr, "[audio] sortie %s, %d Hz, %d échantillons par appel\n", SDL_GetCurrentAudioDriver(), have.freq, trames);
        SDL_ResumeAudioStreamDevice(g_device);
        return true;
    }();
    (void)opened;
    return g_device != nullptr;
}

void Submit(const float* mix) {
    if (!g_device) return;
    float stereo[2 * kFrameSamples];
    // options [audio] volume et couper_hors_focus (le mixage continue : pas de dérive des vidéos) ; somme bornée à [-1, 1]
    const float volume = config::g.muteUnfocused && !window::Focused() ? 0.0f : config::g.volume;
    for (int i = 0; i < 2 * kFrameSamples; i++) stereo[i] = std::clamp(mix[i] * volume, -1.0f, 1.0f);
    std::lock_guard<std::mutex> l(g_mutex);
    g_file.insert(g_file.end(), stereo, stereo + 2 * kFrameSamples);
    g_demarre = true;
}

uint32_t QueuedFrames() {
    if (!g_device) return 0;
    std::lock_guard<std::mutex> l(g_mutex);
    return uint32_t(g_file.size() / (2 * kFrameSamples));
}

Bilan Releve() {
    std::lock_guard<std::mutex> l(g_mutex);
    const Bilan b = g_bilan;
    g_bilan = Bilan{};
    return b;
}
#else
bool Open() { return false; }
void Submit(const float*) {}
uint32_t QueuedFrames() { return 0; }
Bilan Releve() { return {}; }
#endif

}  // namespace wk::audio

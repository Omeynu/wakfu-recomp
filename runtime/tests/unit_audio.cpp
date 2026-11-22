// Test de la réduction 5.1 -> stéréo des trames du pilote XAudio (flottants gros-boutistes, canal par canal).
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../src/kernel/audio_out.h"
using namespace wk::audio;
static int g_fail = 0;
static void Put(uint8_t* frame, int channel, int i, float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    b = __builtin_bswap32(b);
    memcpy(frame + 4 * (channel * kFrameSamples + i), &b, 4);
}
static void Expect(const char* what, float got, float want) {
    if (std::fabs(got - want) > 1e-5f) { printf("ÉCHEC %s : %f au lieu de %f\n", what, got, want); g_fail++; }
}
int main() {
    static uint8_t frame[kFrameChannels * kFrameSamples * 4];
    float out[2 * kFrameSamples];
    // échantillon 0 : avant seuls ; 1 : centre ; 2 : LFE (ignoré) ; 3 : arrière ; 255 : saturation
    Put(frame, 0, 0, 0.5f);  Put(frame, 1, 0, -0.25f);
    Put(frame, 2, 1, 0.4f);
    Put(frame, 3, 2, 1.0f);
    Put(frame, 4, 3, 0.2f);  Put(frame, 5, 3, -0.6f);
    Put(frame, 0, 255, 0.9f); Put(frame, 2, 255, 0.9f);     // G : 0,9 + 0,707 x 0,9
    Put(frame, 1, 254, -0.9f); Put(frame, 5, 254, -0.9f);    // D : -0,9 - 0,707 x 0,9
    DownmixToStereo(frame, out);
    const float k = 0.70710678f;
    Expect("avant G", out[0], 0.5f);        Expect("avant D", out[1], -0.25f);
    Expect("centre G", out[2], 0.4f * k);   Expect("centre D", out[3], 0.4f * k);
    Expect("LFE G", out[4], 0.0f);          Expect("LFE D", out[5], 0.0f);
    Expect("arrière G", out[6], 0.2f * k);  Expect("arrière D", out[7], -0.6f * k);
    Expect("saturation G", out[510], 1.0f); Expect("saturation D", out[509], -1.0f);
    Expect("silence", out[100], 0.0f);
    printf(g_fail ? "unit_audio : %d échec(s)\n" : "unit_audio : OK\n", g_fail);
    return g_fail ? 1 : 0;
}

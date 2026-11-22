// Sortie son : trames du pilote XAudio vers le périphérique de l'hôte (SDL2 si présent à la compilation).
#pragma once
#include <cstdint>

namespace wk::audio {

// Trame du pilote (XAudioSubmitRenderDriverFrame) : 6 canaux (avant G, avant D, centre, LFE, arrière G,
// arrière D) de 256 échantillons à 48 kHz, flottants gros-boutistes, rangés canal par canal.
constexpr int kFrameSamples = 256, kFrameChannels = 6, kRate = 48000;

// 5.1 -> stéréo entrelacée (flottants hôte) : G = AvG + 0,707 (C + ArG), D = AvD + 0,707 (C + ArD),
// LFE ignoré, résultat borné à [-1, 1]. `in` : 6 x 256 flottants gros-boutistes ; `out` : 2 x 256.
void DownmixToStereo(const uint8_t* in, float* out);

// Ouvre le périphérique (une fois) ; false : pas de son (périphérique absent ou build sans SDL2).
bool Open();
// File une trame invitée (sans effet si le périphérique n'est pas ouvert).
void Submit(const uint8_t* guestFrame);
// Trames en attente de lecture côté hôte (0 si pas de périphérique).
uint32_t QueuedFrames();

}  // namespace wk::audio

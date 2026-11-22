// Décodeur XMA (bloc audio du Xenon). Le jeu écrit ses contextes (XMA_CONTEXT_DATA, disposition reprise de
// Xenia, BSD) dans un tableau en mémoire physique dont la base est lue au registre 0x7FEA1800, puis écrit un
// bit par contexte dans les registres kick/lock/clear. Le décodage est synchrone, au moment du kick : la sortie
// (anneau de blocs de 256 octets, PCM 16 bits gros-boutiste entrelacé) est remplie tant qu'elle a de la place.
//
// Champs (mots de 32 bits gros-boutistes, bit 0 = poids faible) :
//   mot 0 : paquets entrée 0 (0-11), boucles (12-19), entrée 0 valide (20), entrée 1 valide (21),
//           blocs de sortie (22-26), position d'écriture de la sortie en blocs (27-31)
//   mot 1 : paquets entrée 1 (0-11), fréquence (27-28), stéréo (29), sortie valide (31)
//   mot 2 : position de lecture de l'entrée en bits (0-25) ; mots 3/4 : début/fin de boucle en bits (0-25),
//   mot 4 bit 31 : tampon d'entrée courant ; mots 5-8 : adresses physiques entrée 0, entrée 1, sortie, travail ;
//   mot 9 : position de lecture de la sortie en blocs (0-4).
// Sortie pleine (écriture rattrape la lecture) : le bit « sortie valide » est effacé, comme le fait Xenia.
// Boucles au paquet près : après le paquet qui contient la fin de boucle, reprise au paquet du début.
#include "kernel/xma.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include "guest.h"
#ifdef WAKFU_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

namespace wk::xma {
namespace {
constexpr uint32_t kOffsetMask = 0x03FFFFFF, kPhysSize = 0x20000000;
constexpr uint32_t kRegWords = kContextCount / 32;
#ifdef WAKFU_HAS_FFMPEG
constexpr uint32_t kPacketBytes = 2048, kPacketBits = kPacketBytes * 8, kBlockBytes = 256;
constexpr int kRates[4] = {24000, 32000, 44100, 48000};
#endif

struct Decoder {
    bool used = false;
#ifdef WAKFU_HAS_FFMPEG
    AVCodecContext* av = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
#endif
    int channels = 0, rate = 0;
    uint32_t lastReadOffset = ~0u;   // position d'entrée laissée au dernier passage (autre valeur : le jeu a repositionné)
    std::vector<int16_t> pcm;        // échantillons décodés pas encore écrits (entrelacés)
    size_t pcmPos = 0;
};
std::mutex g_mutex;
uint32_t g_array = 0;   // adresse invitée du tableau (vue physique 0xA0000000)
Decoder g_ctx[kContextCount];

uint8_t* Phys(uint32_t phys) { return Host(0xA0000000u | (phys & (kPhysSize - 1))); }
bool PhysRange(uint32_t phys, uint32_t size) { return (phys & (kPhysSize - 1)) + uint64_t(size) <= kPhysSize; }

#ifdef WAKFU_HAS_FFMPEG
const AVCodec* Codec() {
    static const AVCodec* c = [] {
        av_log_set_level(AV_LOG_ERROR);
        const AVCodec* found = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!found) fprintf(stderr, "[xma] libavcodec sans décodeur xma2 : entrées consommées sans son\n");
        return found;
    }();
    return c;
}
void CloseDecoder(Decoder& d) {
    avcodec_free_context(&d.av);
    av_packet_free(&d.pkt);
    av_frame_free(&d.frame);
    d.channels = d.rate = 0;
}
// Ouvre (ou rouvre si le format change) le décodeur xma2 d'un flux de 1 ou 2 canaux.
bool OpenDecoder(Decoder& d, uint32_t id, int channels, int rate) {
    if (d.av && d.channels == channels && d.rate == rate) return true;
    CloseDecoder(d);
    AVCodecContext* c = avcodec_alloc_context3(Codec());
    if (!c) return false;
    c->sample_rate = rate;
    av_channel_layout_default(&c->ch_layout, channels);
    // XMA2WAVEFORMATEX (34 octets, petit-boutiste) : NumStreams (u16), ChannelMask (u32) ; le reste est ignoré
    c->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!c->extradata) { avcodec_free_context(&c); return false; }
    c->extradata_size = 34;
    c->extradata[0] = 1;
    c->extradata[2] = channels == 1 ? 0x04 : 0x03;   // centre, ou avant gauche + droite
    if (avcodec_open2(c, Codec(), nullptr) < 0) { avcodec_free_context(&c); return false; }
    d.av = c;
    d.pkt = av_packet_alloc();
    d.frame = av_frame_alloc();
    if (!d.pkt || !d.frame) { CloseDecoder(d); return false; }
    d.channels = channels;
    d.rate = rate;
    fprintf(stderr, "[xma] contexte %u : %d canal(aux), %d Hz\n", id, channels, rate);
    return true;
}
void ResetStream(Decoder& d) {
    if (d.av) avcodec_flush_buffers(d.av);
    d.pcm.clear();
    d.pcmPos = 0;
}
// Décode un paquet de 2048 octets et ajoute les échantillons (16 bits, entrelacés) à la file.
void DecodePacket(Decoder& d, const uint8_t* src) {
    if (av_new_packet(d.pkt, kPacketBytes) < 0) return;
    memcpy(d.pkt->data, src, kPacketBytes);
    const int err = avcodec_send_packet(d.av, d.pkt);
    av_packet_unref(d.pkt);
    if (err < 0) {
        avcodec_flush_buffers(d.av);   // paquet invalide : resynchronisation au paquet suivant
        return;
    }
    if (d.pcmPos && d.pcmPos == d.pcm.size()) { d.pcm.clear(); d.pcmPos = 0; }
    while (avcodec_receive_frame(d.av, d.frame) == 0) {
        const int n = d.frame->nb_samples, ch = d.channels;
        const size_t at = d.pcm.size();
        d.pcm.resize(at + size_t(n) * ch);
        for (int k = 0; k < ch; k++) {
            const float* s = reinterpret_cast<const float*>(d.frame->extended_data[k]);   // AV_SAMPLE_FMT_FLTP
            for (int i = 0; i < n; i++) {
                const float v = s[i] > 1.f ? 1.f : s[i] < -1.f ? -1.f : s[i];
                d.pcm[at + size_t(i) * ch + k] = int16_t(lrintf(v * 32767.f));
            }
        }
        av_frame_unref(d.frame);
    }
}
#endif

void SetOffset(uint32_t& w2, uint32_t bits) { w2 = (w2 & ~kOffsetMask) | (bits & kOffsetMask); }
// Tampon d'entrée courant épuisé : invalidé, passage à l'autre tampon, lecture après l'en-tête du 1er paquet.
void SwapInput(uint32_t* w) {
    const uint32_t cur = w[4] >> 31;
    w[0] &= ~(1u << (20 + cur));
    w[4] ^= 0x80000000u;
    SetOffset(w[2], 32);
}

// Décode le contexte id ; appelé sous g_mutex. Les mots modifiés sont réécrits à la fin.
void Work(uint32_t id) {
    const uint32_t c = g_array + id * 64;
    uint32_t w[10];
    for (int i = 0; i < 10; i++) w[i] = Ld32(c + 4 * i);
    const uint32_t before0 = w[0], before1 = w[1], before2 = w[2], before4 = w[4];
#ifdef WAKFU_HAS_FFMPEG
    Decoder& d = g_ctx[id];
    const int channels = (w[1] >> 29) & 1 ? 2 : 1;
    const uint32_t blocks = (w[0] >> 22) & 31, rd = w[9] & 31;
    const uint32_t outPhys = w[7], outBytes = blocks * kBlockBytes;
    bool ok = Codec() && (w[1] >> 31) && blocks && rd < blocks && PhysRange(outPhys, outBytes) &&
              OpenDecoder(d, id, channels, kRates[(w[1] >> 27) & 3]);
    if (!ok && Codec()) return;   // sortie non prête : rien à faire
    if (ok) {
        if ((w[2] & kOffsetMask) != d.lastReadOffset) ResetStream(d);
        uint32_t wr = (w[0] >> 27) % blocks;
        bool full = false;
        // Borne : une boucle infinie sur des paquets sans échantillons ne doit pas bloquer le thread du jeu
        for (uint32_t budget = 4096; budget; budget--) {
            // 1. File -> anneau de sortie, bloc par bloc, tant que l'écriture ne rattrape pas la lecture
            constexpr size_t kBlockSamples = kBlockBytes / 2;
            while (!full && d.pcm.size() - d.pcmPos >= kBlockSamples) {
                uint8_t* dst = Phys(outPhys) + wr * kBlockBytes;
                for (size_t i = 0; i < kBlockSamples; i++) {
                    const uint16_t v = uint16_t(d.pcm[d.pcmPos + i]);
                    dst[2 * i] = uint8_t(v >> 8);
                    dst[2 * i + 1] = uint8_t(v);
                }
                d.pcmPos += kBlockSamples;
                wr = (wr + 1) % blocks;
                full = wr == rd;
            }
            if (full) break;
            // 2. Paquet suivant du tampon d'entrée courant (ou de l'autre s'il est seul valide)
            uint32_t cur = w[4] >> 31;
            if (!(w[0] & (1u << (20 + cur)))) {
                if (!(w[0] & (1u << (21 - cur)))) break;   // plus d'entrée : on attend le jeu
                w[4] ^= 0x80000000u;
                cur ^= 1;
                SetOffset(w[2], 32);
            }
            const uint32_t count = cur ? (w[1] & 0xFFF) : (w[0] & 0xFFF);
            const uint32_t inPhys = w[5 + cur];
            const uint32_t pkt = (w[2] & kOffsetMask) / kPacketBits;
            if (pkt >= count || !PhysRange(inPhys, count * kPacketBytes)) { SwapInput(w); continue; }
            DecodePacket(d, Phys(inPhys) + pkt * kPacketBytes);
            uint32_t next = pkt + 1;
            const uint32_t loops = (w[0] >> 12) & 0xFF;
            if (loops && pkt == (w[4] & kOffsetMask) / kPacketBits) {
                next = (w[3] & kOffsetMask) / kPacketBits;
                if (loops != 255) w[0] = (w[0] & ~(0xFFu << 12)) | ((loops - 1) << 12);   // 255 : boucle infinie
                avcodec_flush_buffers(d.av);   // la trame à cheval sur la fin de boucle est abandonnée
            }
            SetOffset(w[2], next * kPacketBits + 32);
            if (next >= count) SwapInput(w);   // tampon libéré tout de suite : le jeu peut le remplir
        }
        w[0] = (w[0] & ~(31u << 27)) | (wr << 27);
        if (full) w[1] &= ~0x80000000u;
        d.lastReadOffset = w[2] & kOffsetMask;
    }
#endif
    if (!HasDecoder()) {
        // Sans décodeur : le tampon d'entrée courant est consommé, sans sortie (comportement d'avant J5).
        const uint32_t cur = w[4] >> 31, valid = 1u << (20 + cur);
        if (!(w[0] & valid)) return;
        w[0] &= ~valid;
        SetOffset(w[2], 0);
        w[4] ^= 0x80000000u;
    }
    if (w[0] != before0) St32(c, w[0]);
    if (w[1] != before1) St32(c + 4, w[1]);
    if (w[2] != before2) St32(c + 8, w[2]);
    if (w[4] != before4) St32(c + 16, w[4]);
}

// Lit et remet à zéro un groupe de registres (valeurs écrites par stwbrx : petit-boutiste, l'ordre de l'hôte).
void TakeRegisters(uint32_t reg, uint32_t* out) {
    for (uint32_t i = 0; i < kRegWords; i++)
        out[i] = __atomic_exchange_n(reinterpret_cast<uint32_t*>(Host(reg + 4 * i)), 0u, __ATOMIC_SEQ_CST);
}
}  // namespace

void Init() {
    g_array = Alloc(Heap::Physical, kContextCount * 64, 0x1000);
    const uint32_t phys = g_array & (kPhysSize - 1);
    memcpy(Host(kRegArrayBase), &phys, 4);   // lu par lwbrx
}

bool HasDecoder() {
#ifdef WAKFU_HAS_FFMPEG
    return Codec() != nullptr;
#else
    return false;
#endif
}

uint32_t CreateContext() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t id = 0; id < kContextCount; id++) {
        Decoder& d = g_ctx[id];
        if (d.used) continue;
        d.used = true;
        d.lastReadOffset = ~0u;
        d.pcm.clear();
        d.pcmPos = 0;
        memset(Host(g_array + id * 64), 0, 64);
        return g_array + id * 64;
    }
    return 0;
}

void ReleaseContext(uint32_t guestAddr) {
    const uint32_t off = (guestAddr & (kPhysSize - 1)) - (g_array & (kPhysSize - 1));
    if (off % 64 || off / 64 >= kContextCount) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Decoder& d = g_ctx[off / 64];
#ifdef WAKFU_HAS_FFMPEG
    CloseDecoder(d);
#endif
    d.pcm = {};
    d.pcmPos = 0;
    d.used = false;
}

void ServiceRegisters() {
    uint32_t clear[kRegWords], lockBits[kRegWords], kick[kRegWords];
    TakeRegisters(kRegClear, clear);
    TakeRegisters(kRegLock, lockBits);
    TakeRegisters(kRegKick, kick);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t r = 0; r < kRegWords; r++)
        for (uint32_t b = 0; b < 32; b++) {
            const uint32_t id = r * 32 + b, bit = 1u << b;
            Decoder& d = g_ctx[id];
            if (!d.used) continue;
            const uint32_t c = g_array + id * 64;
            if (clear[r] & bit) {
                // Comme Xenia : entrées et sortie invalidées, positions de sortie à 0
                St32(c, Ld32(c) & ~((1u << 20) | (1u << 21) | (31u << 27)));
                St32(c + 4, Ld32(c + 4) & ~0x80000000u);
                St32(c + 36, Ld32(c + 36) & ~31u);
                d.lastReadOffset = ~0u;
            }
            // lock (désactivation) : rien à arrêter, le décodage ne se fait qu'au kick
            if (kick[r] & bit) Work(id);
        }
}

}  // namespace wk::xma

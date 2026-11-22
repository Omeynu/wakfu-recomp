// Test du décodeur XMA par l'interface des contextes : une entrée XMA d'une wavebank XACT (musique) est
// envoyée par tampons d'entrée alternés de 37 paquets, la sortie (anneau de 16 blocs) est consommée comme
// le ferait le jeu. Comparaison avec un décodage FFmpeg direct : nombre d'échantillons et énergie.
// Puis une boucle (1 répétition) au paquet près. Usage : wakfu_unit_xma <WaveBank.xwb> [index, 0 par défaut]
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../src/guest.h"
#include "../src/kernel/xma.h"
#include "../src/platform.h"

using namespace wk;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "ÉCHEC ligne %d : %s\n", __LINE__, #c); g_fail++; } } while (0)

static uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
struct Stats { uint64_t frames = 0; double sq = 0; };

// Référence : décodeur xma2 alimenté directement ; flushAfter >= 0 : vidage après ce paquet (boucle)
static Stats Direct(const uint8_t* data, const std::vector<uint32_t>& packets, int ch, int rate) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->sample_rate = rate;
    av_channel_layout_default(&c->ch_layout, ch);
    c->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    c->extradata_size = 34;
    c->extradata[0] = 1;
    c->extradata[2] = ch == 1 ? 0x04 : 0x03;
    Stats s;
    if (avcodec_open2(c, codec, nullptr) < 0) return s;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    for (uint32_t p : packets) {
        if (p == ~0u) { avcodec_flush_buffers(c); continue; }
        av_new_packet(pkt, 2048);
        memcpy(pkt->data, data + size_t(p) * 2048, 2048);
        if (avcodec_send_packet(c, pkt) < 0) avcodec_flush_buffers(c);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(c, fr) == 0) {
            s.frames += fr->nb_samples;
            for (int k = 0; k < ch; k++)
                for (int i = 0; i < fr->nb_samples; i++) {
                    float v = ((float*)fr->extended_data[k])[i];
                    v = v > 1 ? 1 : v < -1 ? -1 : v;
                    const double q = double(int16_t(lrintf(v * 32767.f)));
                    s.sq += q * q;
                }
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    return s;
}

static void Kick(uint32_t ctxAddr) {
    // id = (adresse physique - base) >> 6, un bit par contexte, écrit en petit-boutiste (stwbrx)
    uint32_t base;
    memcpy(&base, Host(xma::kRegArrayBase), 4);
    const uint32_t id = ((ctxAddr & 0x1FFFFFFF) - base) >> 6;
    uint32_t* reg = reinterpret_cast<uint32_t*>(Host(xma::kRegKick + 4 * (id / 32)));
    *reg |= 1u << (id % 32);
    xma::ServiceRegisters();
}

// Consomme les blocs disponibles de l'anneau de sortie (comme le jeu), rend le nombre de blocs lus.
static uint32_t Consume(uint32_t c, uint32_t out, uint32_t blocks, Stats& s, int ch) {
    const uint32_t w0 = Ld32(c), w1 = Ld32(c + 4), rd = Ld32(c + 36) & 31, wr = w0 >> 27;
    const uint32_t n = (w1 >> 31) ? (wr + blocks - rd) % blocks : blocks;   // sortie invalide : anneau plein
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t b = out + ((rd + i) % blocks) * 256;
        for (uint32_t k = 0; k < 128; k++) {
            const double q = double(int16_t(Ld16(b + 2 * k)));
            s.sq += q * q;
        }
        s.frames += 128 / ch;
    }
    St32(c + 36, (Ld32(c + 36) & ~31u) | wr);
    St32(c + 4, w1 | 0x80000000u);
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage : %s <WaveBank.xwb> [index]\n", argv[0]); return 2; }
    g_base = (uint8_t*)platform::ReserveGuestMemory(0x100000000ull);
    if (!g_base) { fprintf(stderr, "mémoire invitée indisponible\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "wavebank introuvable : %s\n", argv[1]); return 2; }
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    const bool readOk = fread(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    if (!readOk || d.size() < 64) { fprintf(stderr, "lecture impossible\n"); return 2; }
    const uint32_t bdOff = BE32(&d[12]), mdOff = BE32(&d[20]), wdOff = BE32(&d[44]);
    const uint32_t entSize = BE32(&d[bdOff + 72]);
    const int idx = argc > 2 ? atoi(argv[2]) : 0;
    const uint8_t* e = &d[mdOff + idx * entSize];
    const uint32_t fmt = BE32(e + 4), off = BE32(e + 8), len = BE32(e + 12), expected = BE32(e) >> 4;
    const int ch = (fmt >> 2) & 7, rate = (fmt >> 5) & 0x3FFFF;
    if ((fmt & 3) != 1 || ch < 1 || ch > 2 || uint64_t(wdOff) + off + len > d.size()) {
        fprintf(stderr, "entrée %d : pas un flux XMA mono/stéréo\n", idx);
        return 2;
    }
    const uint8_t* data = &d[wdOff + off];
    const uint32_t total = len / 2048;
    const uint32_t rateId = rate == 24000 ? 0 : rate == 32000 ? 1 : rate == 44100 ? 2 : 3;

    xma::Init();
    CHECK(xma::HasDecoder());

    // 1. Flux complet, tampons d'entrée alternés de 37 paquets, anneau de sortie de 16 blocs
    constexpr uint32_t kChunk = 37, kBlocks = 16;
    const uint32_t c = xma::CreateContext();
    CHECK(c != 0);
    const uint32_t in[2] = {Alloc(Heap::Physical, kChunk * 2048, 2048), Alloc(Heap::Physical, kChunk * 2048, 2048)};
    const uint32_t out = Alloc(Heap::Physical, kBlocks * 256, 256);
    St32(c, (kBlocks << 22));
    St32(c + 4, (rateId << 27) | (uint32_t(ch == 2) << 29) | 0x80000000u);
    St32(c + 8, 32);
    St32(c + 20, in[0] & 0x1FFFFFFF);
    St32(c + 24, in[1] & 0x1FFFFFFF);
    St32(c + 28, out & 0x1FFFFFFF);
    Stats got;
    uint32_t next = 0, kicks = 0, fullSeen = 0;
    for (; kicks < 1000000; kicks++) {
        for (int b = 0; b < 2; b++) {   // remplissage des tampons d'entrée libérés
            const uint32_t w0 = Ld32(c);
            if ((w0 & (1u << (20 + b))) || next >= total) continue;
            const uint32_t n = total - next < kChunk ? total - next : kChunk;
            memcpy(Host(in[b]), data + size_t(next) * 2048, size_t(n) * 2048);
            next += n;
            if (b == 0) St32(c, (w0 & ~0xFFFu) | n | (1u << 20));
            else { St32(c, w0 | (1u << 21)); St32(c + 4, (Ld32(c + 4) & ~0xFFFu) | n); }
        }
        Kick(c);
        if (!(Ld32(c + 4) >> 31)) fullSeen++;
        const uint32_t n = Consume(c, out, kBlocks, got, ch);
        if (n == 0 && next >= total && !(Ld32(c) & (3u << 20))) break;
    }
    const Stats ref = Direct(data, [&] { std::vector<uint32_t> v; for (uint32_t i = 0; i < total; i++) v.push_back(i); return v; }(), ch, rate);
    const double rmsGot = sqrt(got.sq / double(got.frames * ch)) / 32768, rmsRef = sqrt(ref.sq / double(ref.frames * ch)) / 32768;
    printf("flux %d : %u paquets, %u kicks (sortie pleine %u fois)\n", idx, total, kicks, fullSeen);
    printf("  contexte : %llu échantillons, RMS %.5f ; direct : %llu, RMS %.5f ; wavebank : %u\n",
           (unsigned long long)got.frames, rmsGot, (unsigned long long)ref.frames, rmsRef, expected);
    CHECK(ref.frames > 0);
    CHECK(got.frames == ref.frames);
    CHECK(fabs(rmsGot - rmsRef) <= 1e-4 * rmsRef);
    CHECK(fabs(double(got.frames) - expected) <= 0.001 * expected);
    CHECK(fullSeen > 0);
    xma::ReleaseContext(c);

    // 2. Boucle : 8 paquets (à partir du 100e), une répétition de la boucle paquets 0..3
    const uint32_t c2 = xma::CreateContext();
    CHECK(c2 == c);   // contexte libéré réutilisé
    memcpy(Host(in[0]), data + 100 * 2048, 8 * 2048);
    St32(c2, 8 | (1u << 12) | (1u << 20) | (kBlocks << 22));
    St32(c2 + 4, (rateId << 27) | (uint32_t(ch == 2) << 29) | 0x80000000u);
    St32(c2 + 8, 32);
    St32(c2 + 12, 32);                 // début de boucle : paquet 0
    St32(c2 + 16, 3 * 16384 + 500);    // fin de boucle : dans le paquet 3
    St32(c2 + 20, in[0] & 0x1FFFFFFF);
    St32(c2 + 28, out & 0x1FFFFFFF);
    Stats loop;
    for (int i = 0; i < 1000; i++) {
        Kick(c2);
        if (Consume(c2, out, kBlocks, loop, ch) == 0) break;
    }
    const Stats loopRef = Direct(data + 100 * 2048, {0, 1, 2, 3, ~0u, 0, 1, 2, 3, 4, 5, 6, 7}, ch, rate);
    printf("boucle : contexte %llu échantillons, direct %llu\n", (unsigned long long)loop.frames,
           (unsigned long long)loopRef.frames);
    CHECK(loop.frames == loopRef.frames && loop.frames > 0);
    CHECK(((Ld32(c2) >> 12) & 0xFF) == 0);   // compteur de boucles décrémenté
    CHECK(!(Ld32(c2) & (1u << 20)));          // tampon consommé

    if (g_fail) { fprintf(stderr, "%d échec(s)\n", g_fail); return 1; }
    printf("OK\n");
    return 0;
}

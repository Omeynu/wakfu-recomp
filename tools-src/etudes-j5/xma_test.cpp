// Prototype : décodage XMA2 d'une entrée de wavebank XACT (Xbox 360) par paquets de 2048 octets,
// avec le décodeur xma2 de FFmpeg (LGPL). Usage : xma_test <fichier.xwb> <index> <sortie.wav>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
static uint32_t BE32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    std::vector<uint8_t> d; { fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(d.data(), 1, d.size(), f); fclose(f); }
    const uint32_t bdOff = BE32(&d[12]), mdOff = BE32(&d[20]), wdOff = BE32(&d[44]);
    const uint32_t entSize = BE32(&d[bdOff + 72]);
    const int idx = atoi(argv[2]);
    const uint8_t* e = &d[mdOff + idx * entSize];
    const uint32_t fmt = BE32(e + 4), off = BE32(e + 8), len = BE32(e + 12), dur = BE32(e) >> 4;
    const int tag = fmt & 3, ch = (fmt >> 2) & 7, rate = (fmt >> 5) & 0x3FFFF;
    printf("entrée %d : tag %d, %d canaux, %d Hz, %u octets, %u échantillons attendus\n", idx, tag, ch, rate, len, dur);
    if (tag != 1) { printf("pas du XMA\n"); return 1; }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->sample_rate = rate;
    av_channel_layout_default(&c->ch_layout, ch);
    // XMA2WAVEFORMATEX (34 octets, petit-boutiste) : NumStreams, ChannelMask ; le reste n'est pas lu par FFmpeg
    c->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    c->extradata_size = 34;
    c->extradata[0] = 1;                       // un flux (jusqu'à 2 canaux)
    c->extradata[2] = ch == 1 ? 0x04 : 0x03;   // masque : centre ou avant gauche/droite
    if (avcodec_open2(c, codec, nullptr) < 0) { printf("ouverture du décodeur impossible\n"); return 1; }
    AVPacket* pkt = av_packet_alloc(); AVFrame* fr = av_frame_alloc();
    std::vector<int16_t> pcm; double sq = 0;
    for (uint32_t p = 0; p + 2048 <= len; p += 2048) {
        av_new_packet(pkt, 2048);
        memcpy(pkt->data, &d[wdOff + off + p], 2048);
        if (avcodec_send_packet(c, pkt) < 0) { printf("paquet %u rejeté\n", p / 2048); }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(c, fr) == 0) {
            for (int i = 0; i < fr->nb_samples; i++)
                for (int k = 0; k < ch; k++) {
                    float s = ((float*)fr->extended_data[k])[i];   // planaire flottant (AV_SAMPLE_FMT_FLTP)
                    sq += s * s;
                    s = s > 1 ? 1 : s < -1 ? -1 : s;
                    pcm.push_back(int16_t(lrintf(s * 32767)));
                }
        }
    }
    const uint32_t n = pcm.size() / ch;
    printf("décodé : %u échantillons (%.1f s), format %s, RMS %.4f\n", n, double(n) / rate,
           av_get_sample_fmt_name(c->sample_fmt), sqrt(sq / (pcm.size() ? pcm.size() : 1)));
    FILE* o = fopen(argv[3], "wb");
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, o); }; auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, o); };
    fwrite("RIFF", 1, 4, o); w32(36 + pcm.size() * 2); fwrite("WAVEfmt ", 1, 8, o); w32(16); w16(1); w16(ch);
    w32(rate); w32(rate * ch * 2); w16(ch * 2); w16(16); fwrite("data", 1, 4, o); w32(pcm.size() * 2);
    fwrite(pcm.data(), 2, pcm.size(), o); fclose(o);
    return 0;
}

// Prototype : décodage hôte d'un .wmv du jeu (ASF, WMV3 + WMA2) avec FFmpeg (LGPL).
// Usage : wmv_test <fichier.wmv> <image.pgm>  — compte images et échantillons, mesure le temps, écrit le plan Y de l'image 60.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}
#include <chrono>
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, argv[1], nullptr, nullptr) < 0 || avformat_find_stream_info(fmt, nullptr) < 0) return 1;
    AVCodecContext* dec[16] = {};
    for (unsigned i = 0; i < fmt->nb_streams && i < 16; i++) {
        const AVCodec* c = avcodec_find_decoder(fmt->streams[i]->codecpar->codec_id);
        if (!c) { printf("flux %u : pas de décodeur\n", i); continue; }
        dec[i] = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec[i], fmt->streams[i]->codecpar);
        dec[i]->thread_count = 1;
        avcodec_open2(dec[i], c, nullptr);
        printf("flux %u : %s\n", i, c->name);
    }
    AVPacket* pkt = av_packet_alloc(); AVFrame* fr = av_frame_alloc();
    long frames = 0, samples = 0; double vsec = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        AVCodecContext* c = pkt->stream_index < 16 ? dec[pkt->stream_index] : nullptr;
        if (c) {
            auto t0 = std::chrono::steady_clock::now();
            avcodec_send_packet(c, pkt);
            while (avcodec_receive_frame(c, fr) == 0) {
                if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
                    if (++frames == 400) {
                        FILE* o = fopen(argv[2], "wb");
                        fprintf(o, "P5\n%d %d\n255\n", fr->width, fr->height);
                        for (int y = 0; y < fr->height; y++) fwrite(fr->data[0] + y * fr->linesize[0], 1, fr->width, o);
                        fclose(o);
                        printf("image 60 : %dx%d %s\n", fr->width, fr->height, av_get_pix_fmt_name((AVPixelFormat)fr->format));
                    }
                } else samples += fr->nb_samples;
            }
            if (c->codec_type == AVMEDIA_TYPE_VIDEO)
                vsec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        av_packet_unref(pkt);
    }
    printf("%ld images (%.2f ms/image, 1 thread), %ld échantillons audio, durée %.1f s\n", frames, 1000 * vsec / (frames ? frames : 1),
           samples, fmt->duration / 1e6);
    return 0;
}

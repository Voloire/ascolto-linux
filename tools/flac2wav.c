/* flac2wav: decode a mono FLAC file into a 16-bit PCM WAV, used by scripts/recap.py. */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int write_wav_header(FILE *fp, uint32_t rate, uint32_t data_bytes) {
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, 1);
    put_le32(h + 24, rate);
    put_le32(h + 28, rate * 2);
    put_le16(h + 32, 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
    return fwrite(h, 1, sizeof(h), fp) == sizeof(h) ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <in.flac> <out.wav>\n", argv[0]);
        return 2;
    }
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "flac2wav: cannot open %s\n", argv[1]);
        return 1;
    }
    int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (idx < 0) return 1;
    AVCodecParameters *par = fmt->streams[idx]->codecpar;
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *ctx = dec ? avcodec_alloc_context3(dec) : NULL;
    if (!ctx || avcodec_parameters_to_context(ctx, par) < 0 || avcodec_open2(ctx, dec, NULL) < 0) return 1;

    FILE *fp = fopen(argv[2], "wb");
    if (!fp) return 1;
    write_wav_header(fp, (uint32_t)ctx->sample_rate, 0);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint32_t data_bytes = 0;
    int ok = 1;

    while (ok && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                /* FLAC is encoded as S16 by ascolto, but accept S32 files and reduce them. */
                for (int i = 0; i < frame->nb_samples; i++) {
                    int16_t s;
                    if (frame->format == AV_SAMPLE_FMT_S16 || frame->format == AV_SAMPLE_FMT_S16P) {
                        s = ((const int16_t *)frame->data[0])[i];
                    } else {
                        s = (int16_t)(((const int32_t *)frame->data[0])[i] >> 16);
                    }
                    uint8_t b[2];
                    put_le16(b, (uint16_t)s);
                    if (fwrite(b, 1, 2, fp) != 2) { ok = 0; break; }
                    data_bytes += 2;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    fseek(fp, 0, SEEK_SET);
    write_wav_header(fp, (uint32_t)ctx->sample_rate, data_bytes);
    fclose(fp);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok ? 0 : 1;
}

#define _POSIX_C_SOURCE 200809L
#include "ascolto/audio_recorder.h"
#include "ascolto/types.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct ascolto_audio_recorder {
    AVFormatContext *fmt;
    AVCodecContext *enc;
    AVStream *stream;
    AVFrame *frame;
    AVPacket *pkt;
    int16_t *pending;
    int frame_size;
    int pending_count;
    int64_t next_pts;
    pthread_mutex_t mutex;
};

static int send_frame_locked(ascolto_audio_recorder_t *rec, AVFrame *frame) {
    if (avcodec_send_frame(rec->enc, frame) < 0) return -1;
    for (;;) {
        int rc = avcodec_receive_packet(rec->enc, rec->pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
        if (rc < 0) return -1;
        av_packet_rescale_ts(rec->pkt, rec->enc->time_base, rec->stream->time_base);
        rec->pkt->stream_index = rec->stream->index;
        rc = av_interleaved_write_frame(rec->fmt, rec->pkt);
        av_packet_unref(rec->pkt);
        if (rc < 0) return -1;
    }
}

static int encode_pending_locked(ascolto_audio_recorder_t *rec, int count) {
    if (av_frame_make_writable(rec->frame) < 0) return -1;
    rec->frame->nb_samples = count;
    memcpy(rec->frame->data[0], rec->pending, (size_t)count * sizeof(int16_t));
    rec->frame->pts = rec->next_pts;
    rec->next_pts += count;
    return send_frame_locked(rec, rec->frame);
}

static void recorder_free(ascolto_audio_recorder_t *rec) {
    if (!rec) return;
    if (rec->fmt && rec->fmt->pb) avio_closep(&rec->fmt->pb);
    avcodec_free_context(&rec->enc);
    avformat_free_context(rec->fmt);
    av_frame_free(&rec->frame);
    av_packet_free(&rec->pkt);
    free(rec->pending);
    pthread_mutex_destroy(&rec->mutex);
    free(rec);
}

ascolto_audio_recorder_t *ascolto_audio_recorder_open(const char *path) {
    if (!path || !*path) return NULL;
    ascolto_audio_recorder_t *rec = (ascolto_audio_recorder_t *)calloc(1, sizeof(*rec));
    if (!rec) return NULL;
    pthread_mutex_init(&rec->mutex, NULL);

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec) goto fail;
    if (avformat_alloc_output_context2(&rec->fmt, NULL, "flac", path) < 0) goto fail;
    rec->enc = avcodec_alloc_context3(codec);
    rec->pkt = av_packet_alloc();
    rec->frame = av_frame_alloc();
    if (!rec->enc || !rec->pkt || !rec->frame) goto fail;

    rec->enc->sample_fmt = AV_SAMPLE_FMT_S16;
    rec->enc->sample_rate = ASCOLTO_SAMPLE_RATE;
    av_channel_layout_default(&rec->enc->ch_layout, 1);
    rec->enc->time_base = (AVRational){1, ASCOLTO_SAMPLE_RATE};
    if (rec->fmt->oformat->flags & AVFMT_GLOBALHEADER) rec->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(rec->enc, codec, NULL) < 0) goto fail;

    rec->frame_size = rec->enc->frame_size > 0 ? rec->enc->frame_size : 4096;
    rec->pending = (int16_t *)malloc((size_t)rec->frame_size * sizeof(int16_t));
    if (!rec->pending) goto fail;
    rec->frame->format = AV_SAMPLE_FMT_S16;
    rec->frame->sample_rate = ASCOLTO_SAMPLE_RATE;
    av_channel_layout_default(&rec->frame->ch_layout, 1);
    rec->frame->nb_samples = rec->frame_size;
    if (av_frame_get_buffer(rec->frame, 0) < 0) goto fail;

    rec->stream = avformat_new_stream(rec->fmt, NULL);
    if (!rec->stream) goto fail;
    rec->stream->time_base = rec->enc->time_base;
    if (avcodec_parameters_from_context(rec->stream->codecpar, rec->enc) < 0) goto fail;
    if (avio_open(&rec->fmt->pb, path, AVIO_FLAG_WRITE) < 0) goto fail;
    chmod(path, 0600);
    if (avformat_write_header(rec->fmt, NULL) < 0) goto fail;
    return rec;

fail:
    recorder_free(rec);
    return NULL;
}

int ascolto_audio_recorder_write(ascolto_audio_recorder_t *rec, const float *pcm, size_t num_samples) {
    if (!rec || !pcm) return -1;
    int rc = 0;
    pthread_mutex_lock(&rec->mutex);
    for (size_t i = 0; i < num_samples && rc == 0; i++) {
        float v = pcm[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        rec->pending[rec->pending_count++] = (int16_t)(v * 32767.0f);
        if (rec->pending_count == rec->frame_size) {
            rc = encode_pending_locked(rec, rec->pending_count);
            rec->pending_count = 0;
        }
    }
    pthread_mutex_unlock(&rec->mutex);
    return rc;
}

void ascolto_audio_recorder_close(ascolto_audio_recorder_t *rec) {
    if (!rec) return;
    pthread_mutex_lock(&rec->mutex);
    if (rec->pending_count > 0) {
        encode_pending_locked(rec, rec->pending_count);
        rec->pending_count = 0;
    }
    send_frame_locked(rec, NULL);
    av_write_trailer(rec->fmt);
    pthread_mutex_unlock(&rec->mutex);
    recorder_free(rec);
}

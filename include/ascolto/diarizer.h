#ifndef ASCOLTO_DIARIZER_H
#define ASCOLTO_DIARIZER_H

#include "ascolto/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int speaker_id;
    char label[32];
    float centroid[ASCOLTO_EMBED_DIM];
    uint32_t utterance_count;
    float mean_pitch_hz;
    double last_active_sec;
} ascolto_speaker_profile_t;

typedef struct {
    ascolto_sensitivity_t sensitivity;
    float custom_threshold;
    float ema_alpha;
    int speaker_count;
    ascolto_speaker_profile_t speakers[ASCOLTO_MAX_SPEAKERS];
} ascolto_diarizer_t;

void ascolto_diarizer_init(ascolto_diarizer_t *d, ascolto_sensitivity_t sensitivity);
void ascolto_diarizer_reset(ascolto_diarizer_t *d);
void ascolto_diarizer_set_sensitivity(ascolto_diarizer_t *d, ascolto_sensitivity_t sensitivity);

/* Compute L2-normalized hybrid speaker embedding from 16 kHz PCM + optional Whistle frame embeddings. */
void ascolto_diarizer_compute_embedding(
    const float *pcm,
    size_t num_samples,
    const float *whistle_frames,
    size_t whistle_floats,
    size_t whistle_frame_dim,
    float out_embedding[ASCOLTO_EMBED_DIM],
    float *out_pitch_hz,
    float *out_rms_db
);

float ascolto_cosine_similarity(const float *a, const float *b, size_t dim);

/* Assigns speaker label ("ME" for MIC channel, "SPEAKER_1".."SPEAKER_N" for SYS channel). */
int ascolto_diarizer_assign(
    ascolto_diarizer_t *d,
    ascolto_channel_t channel,
    const float embedding[ASCOLTO_EMBED_DIM],
    float pitch_hz,
    double timestamp_sec,
    char out_label[32],
    float *out_similarity
);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_DIARIZER_H */

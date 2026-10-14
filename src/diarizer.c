#include "ascolto/diarizer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void l2_normalize(float *vec, size_t dim) {
    double sum_sq = 0.0;
    for (size_t i = 0; i < dim; i++) {
        sum_sq += (double)vec[i] * (double)vec[i];
    }
    if (sum_sq > 1e-12) {
        float inv = (float)(1.0 / sqrt(sum_sq));
        for (size_t i = 0; i < dim; i++) {
            vec[i] *= inv;
        }
    }
}

float ascolto_cosine_similarity(const float *a, const float *b, size_t dim) {
    if (!a || !b || dim == 0) return 0.0f;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    if (na < 1e-12 || nb < 1e-12) return 0.0f;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

void ascolto_diarizer_init(ascolto_diarizer_t *d, ascolto_sensitivity_t sensitivity) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->sensitivity = sensitivity;
    d->custom_threshold = 0.0f;
    d->ema_alpha = 0.25f;
    d->speaker_count = 0;
}

void ascolto_diarizer_reset(ascolto_diarizer_t *d) {
    if (!d) return;
    d->speaker_count = 0;
    memset(d->speakers, 0, sizeof(d->speakers));
}

void ascolto_diarizer_set_sensitivity(ascolto_diarizer_t *d, ascolto_sensitivity_t sensitivity) {
    if (!d) return;
    d->sensitivity = sensitivity;
}

/* Estimate fundamental frequency F0 (Hz) and voicing harmonicity over 32 ms frames */
static void estimate_pitch_and_rms(
    const float *pcm,
    size_t num_samples,
    float *out_mean_f0,
    float *out_std_f0,
    float *out_voicing,
    float *out_rms_db
) {
    double energy = 0.0;
    for (size_t i = 0; i < num_samples; i++) {
        energy += (double)pcm[i] * (double)pcm[i];
    }
    double rms = (num_samples > 0) ? sqrt(energy / (double)num_samples) : 0.0;
    *out_rms_db = (rms > 1e-6) ? (float)(20.0 * log10(rms)) : -100.0f;

    size_t frame_len = 512; /* 32 ms at 16 kHz */
    size_t hop = 256;
    size_t min_lag = (size_t)(ASCOLTO_SAMPLE_RATE / 360); /* ~360 Hz max F0 */
    size_t max_lag = (size_t)(ASCOLTO_SAMPLE_RATE / 70);  /* ~70 Hz min F0 */

    float f0_vals[256];
    size_t f0_count = 0;
    double voicing_sum = 0.0;

    for (size_t pos = 0; pos + frame_len <= num_samples && f0_count < 256; pos += hop) {
        const float *f = pcm + pos;
        double r0 = 0.0;
        for (size_t i = 0; i < frame_len; i++) {
            r0 += (double)f[i] * (double)f[i];
        }
        if (r0 < 1e-5) continue;

        double best_r = -1.0;
        size_t best_lag = min_lag;
        for (size_t lag = min_lag; lag <= max_lag && lag < frame_len; lag++) {
            double r = 0.0;
            for (size_t i = 0; i + lag < frame_len; i++) {
                r += (double)f[i] * (double)f[i + lag];
            }
            double norm_r = r / r0;
            if (norm_r > best_r) {
                best_r = norm_r;
                best_lag = lag;
            }
        }
        if (best_r >= 0.30) {
            f0_vals[f0_count++] = (float)ASCOLTO_SAMPLE_RATE / (float)best_lag;
            voicing_sum += best_r;
        }
    }

    if (f0_count == 0) {
        *out_mean_f0 = 150.0f;
        *out_std_f0 = 0.0f;
        *out_voicing = 0.0f;
        return;
    }

    double sum = 0.0;
    for (size_t i = 0; i < f0_count; i++) sum += f0_vals[i];
    double mean = sum / (double)f0_count;
    double var = 0.0;
    for (size_t i = 0; i < f0_count; i++) {
        double d = f0_vals[i] - mean;
        var += d * d;
    }
    *out_mean_f0 = (float)mean;
    *out_std_f0 = (float)sqrt(var / (double)f0_count);
    *out_voicing = (float)(voicing_sum / (double)f0_count);
}

/* Goertzel resonator band energies across 28 log-spaced vocal tract frequencies (90 Hz .. 4200 Hz) */
static void compute_vocal_tract_spectrum(const float *pcm, size_t num_samples, float out_bands[28]) {
    memset(out_bands, 0, 28 * sizeof(float));
    if (!pcm || num_samples < 256) return;

    size_t win_len = 512;
    size_t hop = 256;
    size_t frames = 0;
    double band_accum[28] = {0};

    double f_min = 90.0;
    double f_max = 4200.0;
    double coeffs[28];
    for (int b = 0; b < 28; b++) {
        double frac = (double)b / 27.0;
        double freq = f_min * pow(f_max / f_min, frac);
        double k = (freq * (double)win_len) / (double)ASCOLTO_SAMPLE_RATE;
        double w = (2.0 * M_PI * k) / (double)win_len;
        coeffs[b] = 2.0 * cos(w);
    }

    for (size_t pos = 0; pos + win_len <= num_samples && frames < 120; pos += hop) {
        const float *f = pcm + pos;
        double frame_e = 0.0;
        for (size_t i = 0; i < win_len; i++) frame_e += (double)f[i] * (double)f[i];
        if (frame_e < 1e-5) continue;

        for (int b = 0; b < 28; b++) {
            double c = coeffs[b];
            double s1 = 0.0, s2 = 0.0;
            for (size_t i = 0; i < win_len; i++) {
                double s0 = (double)f[i] + c * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            double power = s1 * s1 + s2 * s2 - c * s1 * s2;
            if (power < 1e-9) power = 1e-9;
            band_accum[b] += log(power / (double)win_len + 1e-6);
        }
        frames++;
    }

    if (frames > 0) {
        double mean_val = 0.0;
        for (int b = 0; b < 28; b++) {
            out_bands[b] = (float)(band_accum[b] / (double)frames);
            mean_val += out_bands[b];
        }
        mean_val /= 28.0;
        /* Cepstral mean subtraction to remove channel/gain bias */
        for (int b = 0; b < 28; b++) {
            out_bands[b] -= (float)mean_val;
        }
    }
}

void ascolto_diarizer_compute_embedding(
    const float *pcm,
    size_t num_samples,
    const float *whistle_frames,
    size_t whistle_floats,
    size_t whistle_frame_dim,
    float out_embedding[ASCOLTO_EMBED_DIM],
    float *out_pitch_hz,
    float *out_rms_db
) {
    memset(out_embedding, 0, ASCOLTO_EMBED_DIM * sizeof(float));
    float mean_f0 = 150.0f, std_f0 = 0.0f, voicing = 0.0f, rms_db = -100.0f;
    estimate_pitch_and_rms(pcm, num_samples, &mean_f0, &std_f0, &voicing, &rms_db);
    if (out_pitch_hz) *out_pitch_hz = mean_f0;
    if (out_rms_db) *out_rms_db = rms_db;

    /* Part 1 (dims 0..63): Vocal-tract formant filterbank + DCT cepstral coefficients + F0 prosody */
    float bands[28];
    compute_vocal_tract_spectrum(pcm, num_samples, bands);
    for (int i = 0; i < 28; i++) {
        out_embedding[i] = bands[i];
    }
    /* 20 DCT-II MFCC-like cepstral coefficients from the 28 log-bands */
    for (int k = 1; k <= 20; k++) {
        double sum = 0.0;
        for (int n = 0; n < 28; n++) {
            sum += (double)bands[n] * cos((M_PI / 28.0) * ((double)n + 0.5) * (double)k);
        }
        out_embedding[27 + k] = (float)(sum * 0.25);
    }
    /* Encode pitch (log F0) across harmonic anchor dimensions 48..63 */
    double log_f0 = (log((double)mean_f0 / 65.0) / log(360.0 / 65.0)) * 2.0 - 1.0;
    for (int i = 48; i < 60; i++) {
        double angle = (double)(i - 48 + 1) * log_f0 * M_PI * 0.5;
        out_embedding[i] = (float)((i % 2 == 0) ? sin(angle) : cos(angle)) * 1.8f;
    }
    out_embedding[60] = (float)log_f0 * 2.5f;
    out_embedding[61] = (std_f0 / 50.0f);
    out_embedding[62] = voicing;
    out_embedding[63] = (float)cos(log_f0 * M_PI) * 1.5f;
    l2_normalize(out_embedding, 64);

    /* Part 2 (dims 64..127): Whistle encoder speech embedding pooling (mean + std across 80 ms frames) */
    if (whistle_frames && whistle_floats > 0 && whistle_frame_dim > 0) {
        size_t n_frames = whistle_floats / whistle_frame_dim;
        if (n_frames > 0) {
            for (int d = 0; d < 32; d++) {
                double mean = 0.0;
                double sq = 0.0;
                size_t count = 0;
                for (size_t f = 0; f < n_frames; f++) {
                    const float *row = whistle_frames + f * whistle_frame_dim;
                    /* Strided projection over encoder hidden dimension */
                    for (size_t k = (size_t)d; k < whistle_frame_dim; k += 32) {
                        double v = (double)row[k];
                        mean += v;
                        sq += v * v;
                        count++;
                    }
                }
                if (count > 0) {
                    mean /= (double)count;
                    double var = (sq / (double)count) - (mean * mean);
                    if (var < 0.0) var = 0.0;
                    out_embedding[64 + d] = (float)mean;
                    out_embedding[96 + d] = (float)sqrt(var);
                }
            }
            /* Center and normalize Whistle embedding half */
            double w_mean = 0.0;
            for (int i = 64; i < 128; i++) w_mean += out_embedding[i];
            w_mean /= 64.0;
            for (int i = 64; i < 128; i++) out_embedding[i] -= (float)w_mean;
            l2_normalize(out_embedding + 64, 64);
        }
    } else {
        /* Fallback: mirror vocal features into second half if Whistle model wasn't loaded */
        memcpy(out_embedding + 64, out_embedding, 64 * sizeof(float));
    }

    /* Weight vocal-tract/pitch features (0.75) and Whistle encoder features (0.65) and L2-normalize whole 128-D vector */
    for (int i = 0; i < 64; i++) out_embedding[i] *= 0.78f;
    for (int i = 64; i < 128; i++) out_embedding[i] *= 0.62f;
    l2_normalize(out_embedding, ASCOLTO_EMBED_DIM);
}

int ascolto_diarizer_assign(
    ascolto_diarizer_t *d,
    ascolto_channel_t channel,
    const float embedding[ASCOLTO_EMBED_DIM],
    float pitch_hz,
    double timestamp_sec,
    char out_label[32],
    float *out_similarity
) {
    if (!d || !embedding || !out_label) return 0;

    /* MIC channel is always the local user ("ME") */
    if (channel == ASCOLTO_CHANNEL_MIC) {
        snprintf(out_label, 32, "ME");
        if (out_similarity) *out_similarity = 1.0f;
        return 0;
    }

    float threshold = ascolto_sensitivity_threshold(d->sensitivity, d->custom_threshold);
    int best_idx = -1;
    float best_sim = -1.0f;

    for (int i = 0; i < d->speaker_count; i++) {
        float cos_sim = ascolto_cosine_similarity(embedding, d->speakers[i].centroid, ASCOLTO_EMBED_DIM);
        /* Incorporate pitch proximity bonus/penalty so distinct voices are cleanly separated */
        float pitch_ratio = (pitch_hz > d->speakers[i].mean_pitch_hz)
                                ? (pitch_hz / d->speakers[i].mean_pitch_hz)
                                : (d->speakers[i].mean_pitch_hz / pitch_hz);
        float pitch_penalty = (pitch_ratio > 1.18f) ? (pitch_ratio - 1.18f) * 0.35f : 0.0f;
        float score = cos_sim - pitch_penalty;
        if (score > best_sim) {
            best_sim = score;
            best_idx = i;
        }
    }

    if (best_idx >= 0 && (best_sim >= threshold || d->speaker_count >= ASCOLTO_MAX_SPEAKERS)) {
        ascolto_speaker_profile_t *sp = &d->speakers[best_idx];
        float alpha = d->ema_alpha;
        for (size_t k = 0; k < ASCOLTO_EMBED_DIM; k++) {
            sp->centroid[k] = (1.0f - alpha) * sp->centroid[k] + alpha * embedding[k];
        }
        l2_normalize(sp->centroid, ASCOLTO_EMBED_DIM);
        sp->mean_pitch_hz = (1.0f - alpha) * sp->mean_pitch_hz + alpha * pitch_hz;
        sp->utterance_count++;
        sp->last_active_sec = timestamp_sec;
        snprintf(out_label, 32, "%s", sp->label);
        if (out_similarity) *out_similarity = best_sim;
        return sp->speaker_id;
    }

    /* Register new remote speaker */
    int new_idx = d->speaker_count++;
    ascolto_speaker_profile_t *sp = &d->speakers[new_idx];
    sp->speaker_id = new_idx + 1;
    snprintf(sp->label, sizeof(sp->label), "SPEAKER_%d", sp->speaker_id);
    memcpy(sp->centroid, embedding, ASCOLTO_EMBED_DIM * sizeof(float));
    l2_normalize(sp->centroid, ASCOLTO_EMBED_DIM);
    sp->utterance_count = 1;
    sp->mean_pitch_hz = pitch_hz;
    sp->last_active_sec = timestamp_sec;

    snprintf(out_label, 32, "%s", sp->label);
    if (out_similarity) *out_similarity = 1.0f;
    return sp->speaker_id;
}

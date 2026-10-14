#ifndef ASCOLTO_AUDIO_RECORDER_H
#define ASCOLTO_AUDIO_RECORDER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Streams 16 kHz mono float PCM into a FLAC file (16-bit), one recorder per channel. */
typedef struct ascolto_audio_recorder ascolto_audio_recorder_t;

/* Creates the file (mode 0600). Returns NULL on failure. */
ascolto_audio_recorder_t *ascolto_audio_recorder_open(const char *path);
int ascolto_audio_recorder_write(ascolto_audio_recorder_t *rec, const float *pcm, size_t num_samples);
/* Flushes the encoder, writes the trailer and frees the recorder. */
void ascolto_audio_recorder_close(ascolto_audio_recorder_t *rec);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_AUDIO_RECORDER_H */

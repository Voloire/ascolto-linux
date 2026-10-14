#ifndef ASCOLTO_OBSERVABILITY_H
#define ASCOLTO_OBSERVABILITY_H

#include "ascolto/types.h"
#include <pthread.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ascolto_core ascolto_core_t;

#define ASCOLTO_OBS_RING_SIZE 256

typedef struct {
    ascolto_core_t *core;
    char socket_path[512];
    char log_path[512];
    int listen_fd;
    FILE *log_fp;
    bool running;
    pthread_t server_thread;
    pthread_mutex_t mutex;
    uint64_t event_seq;
    char ring[ASCOLTO_OBS_RING_SIZE][1024];
    size_t ring_head;
    size_t ring_count;
} ascolto_obs_t;

int ascolto_obs_init(ascolto_obs_t *obs, ascolto_core_t *core, const char *socket_path, const char *log_path);
void ascolto_obs_emit(ascolto_obs_t *obs, const char *event_type, const char *json_payload_fields);
int ascolto_obs_handle_command(ascolto_obs_t *obs, const char *cmd_line, char *reply_buf, size_t reply_cap);
void ascolto_obs_destroy(ascolto_obs_t *obs);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_OBSERVABILITY_H */

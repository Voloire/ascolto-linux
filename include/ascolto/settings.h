#ifndef ASCOLTO_SETTINGS_H
#define ASCOLTO_SETTINGS_H
#include "ascolto/types.h"
#include <json-glib/json-glib.h>
typedef struct {
    const char *key, *label, *page;
    size_t offset, size;
    int kind;
} ascolto_field_t;
extern const ascolto_field_t ascolto_fields[];
extern const size_t ascolto_field_count;
char *ascolto_settings_json(const ascolto_config_t *cfg);
int ascolto_settings_patch(ascolto_config_t *cfg, const char *json, char **error);
int ascolto_settings_save(const ascolto_config_t *cfg);
int ascolto_store_init(ascolto_config_t *cfg, bool explicit_config);
char *ascolto_store_list(const ascolto_config_t *cfg, bool jobs);
int ascolto_store_index(const ascolto_config_t *cfg);
long ascolto_job_create(const ascolto_config_t *cfg, const char *dir, bool mp3);
void ascolto_job_finish(const ascolto_config_t *cfg, long id, bool success);
/* Resolve only failed/interrupted jobs; returns a newly allocated directory. */
char *ascolto_job_retry_dir(const ascolto_config_t *cfg, long id, bool *mp3);

#endif

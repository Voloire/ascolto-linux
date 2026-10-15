#ifndef ASCOLTO_SETTINGS_UI_H
#define ASCOLTO_SETTINGS_UI_H
#include "ascolto/core.h"
void ascolto_settings_open(ascolto_core_t *core);
void ascolto_settings_close(void);
void ascolto_settings_tick(ascolto_core_t *core);
int ascolto_settings_apply(ascolto_core_t *core, const char *json, char **error);
#endif

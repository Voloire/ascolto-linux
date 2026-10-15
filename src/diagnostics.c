#include "ascolto/diagnostics.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
static JsonObject *component(JsonObject *root, const char *name) {
    JsonObject *o = json_object_new();
    json_object_set_object_member(root, name, o);
    return o;
}
char *ascolto_diagnostics(ascolto_core_t *core) {
    JsonObject *root = json_object_new();
    json_object_set_boolean_member(root, "ok", TRUE);
    json_object_set_int_member(root, "schema_version", 1);
    json_object_set_int_member(root, "monotonic_ms", ascolto_monotonic_ms());
    pthread_mutex_lock(&core->mutex);
    JsonObject *o = component(root, "core");
    json_object_set_string_member(o, "state", ascolto_state_to_string(core->state));
    json_object_set_int_member(o, "session_id", core->session_id);
    json_object_set_int_member(o, "elapsed_sec", ascolto_core_elapsed_sec(core));
    json_object_set_boolean_member(o, "settings_pending", core->has_pending_config);
    o = component(root, "audio");
    json_object_set_string_member(o, "backend", core->audio.use_mock ? "mock" : "pipewire");
    json_object_set_boolean_member(o, "external_mic_active", core->external_mic_active);
    json_object_set_string_member(o, "mic_device", core->config.mic_device);
    json_object_set_string_member(o, "sys_device", core->config.sys_device);
    for (int i = 0; i < 2; i++) {
        JsonObject *ch = component(o, i ? "SYS" : "MIC");
        json_object_set_int_member(ch, "accepted_samples", core->audio_samples[i]);
        json_object_set_int_member(ch, "dropped_samples", core->dropped_samples[i]);
        json_object_set_double_member(ch, "rms", core->audio_rms[i]);
        json_object_set_double_member(ch, "session_audio_sec", core->session_audio_sec[i]);
        json_object_set_int_member(ch, "vad_buffer_samples", core->vad[i].count);
        json_object_set_boolean_member(ch, "vad_speech", core->vad[i].in_speech);
        json_object_set_boolean_member(ch, "recorder_open", core->recorder[i] != NULL);
    }
    o = component(root, "transcription");
    json_object_set_string_member(o, "backend", core->config.mock_engine ? "fixture" : "whistle");
    json_object_set_boolean_member(o, "model_loaded", core->whistle.model_loaded);
    json_object_set_int_member(o, "embedding_dimension", core->whistle.embed_dim);
    json_object_set_int_member(o, "segments_total", core->total_segments);
    json_object_set_int_member(o, "segments_retained", core->segments_count);
    o = component(root, "diarization");
    json_object_set_int_member(o, "remote_speakers", core->diarizer.speaker_count);
    json_object_set_double_member(
        o, "threshold",
        ascolto_sensitivity_threshold(core->config.diarization_sensitivity, core->config.custom_threshold));
    o = component(root, "artifacts");
    json_object_set_string_member(o, "session_dir", core->current_session_dir);
    json_object_set_boolean_member(o, "markdown_open", core->md_fp != NULL);
    json_object_set_boolean_member(o, "jsonl_open", core->jsonl_fp != NULL);
    o = component(root, "providers");
    json_object_set_string_member(o, "recap_provider", core->config.recap_provider);
    json_object_set_string_member(o, "recap_model", core->config.recap_model);
    int active = 0;
    for (int i = 0; i < 8; i++)
        active += core->worker_pid[i] > 0;
    json_object_set_int_member(o, "active_jobs", active);
    JsonArray *workers = json_array_new();
    for (int i = 0; i < 8; i++)
        if (core->worker_pid[i]) {
            JsonObject *worker = json_object_new();
            json_object_set_int_member(worker, "pid", core->worker_pid[i]);
            json_object_set_int_member(worker, "job_id", core->worker_job[i]);
            json_array_add_object_element(workers, worker);
        }
    json_object_set_array_member(o, "workers", workers);
    char database[512];
    g_strlcpy(database, core->config.database_path, sizeof(database));
    pthread_mutex_unlock(&core->mutex);
    o = component(root, "storage");
    sqlite3 *db = NULL;
    int rc = database[0] ? sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY, NULL) : SQLITE_CANTOPEN;
    json_object_set_boolean_member(o, "available", rc == SQLITE_OK);
    if (rc == SQLITE_OK) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK &&
            sqlite3_step(s) == SQLITE_ROW)
            json_object_set_int_member(o, "schema_version", sqlite3_column_int(s, 0));
        sqlite3_finalize(s);
    }
    if (db)
        sqlite3_close(db);
    o = component(root, "ui");
    ascolto_tray_snapshot_t snap;
    ascolto_tray_get_snapshot(&core->tray, &snap);
    json_object_set_boolean_member(o, "headless", core->tray.headless);
    json_object_set_boolean_member(o, "indicator_active", snap.using_gui_indicator);
    json_object_set_string_member(o, "label", snap.label);
    o = component(root, "observability");
    pthread_mutex_lock(&core->obs.mutex);
    json_object_set_int_member(o, "event_seq", core->obs.event_seq);
    json_object_set_int_member(o, "events_retained", core->obs.ring_count);
    json_object_set_boolean_member(o, "telemetry_file_open", core->obs.log_fp != NULL);
    pthread_mutex_unlock(&core->obs.mutex);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, root);
    char *out = json_to_string(n, FALSE);
    json_node_free(n);
    return out;
}

#define _POSIX_C_SOURCE 200809L
#include "ascolto/settings.h"
#include <glib.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define S(k, l, p) {#k, l, p, offsetof(ascolto_config_t, k), sizeof(((ascolto_config_t *)0)->k), 0}
#define B(k, l, p) {#k, l, p, offsetof(ascolto_config_t, k), sizeof(bool), 1}
const ascolto_field_t ascolto_fields[] = {B(auto_mic_trigger, "Automatic microphone activation", "General"),
                                          B(launch_at_login, "Launch at login", "General"),
                                          S(appearance, "Appearance", "General"),
                                          S(language, "Live transcription language", "Transcription"),
                                          S(keywords, "Custom vocabulary", "Transcription"),
                                          S(weights_path, "Whistle model path", "Transcription"),
                                          B(cloud_transcription, "Cloud retranscription", "Transcription"),
                                          B(cloud_recap, "Automatic recap after recording", "Transcription"),
                                          S(recap_provider, "Recap provider", "Transcription"),
                                          S(recap_model, "Recap model", "Transcription"),
                                          S(recap_lang, "Recap language", "Transcription"),
                                          B(recap_tts, "Generate spoken recap", "Transcription"),
                                          S(tts_lang, "Spoken recap language", "Transcription"),
                                          S(mic_device, "Microphone", "Recording"),
                                          S(sys_device, "System audio", "Recording"),
                                          B(save_audio, "Keep FLAC audio", "Recording"),
                                          S(output_dir, "Session folder", "Storage"),
                                          S(opencode_path, "OpenCode executable", "OpenCode"),
                                          S(openrouter_credential, "Credential reference", "OpenRouter"),
                                          S(openai_credential, "Credential reference", "OpenAI"),
                                          S(stt_model, "Transcription model", "OpenAI"),
                                          S(tts_model, "Speech model", "OpenAI"),
                                          S(tts_voice, "Voice", "OpenAI")};
const size_t ascolto_field_count = sizeof(ascolto_fields) / sizeof(*ascolto_fields);
char *ascolto_settings_json(const ascolto_config_t *cfg) {
    JsonObject *o = json_object_new();
    for (size_t i = 0; i < ascolto_field_count; i++) {
        const ascolto_field_t *f = &ascolto_fields[i];
        const char *v = (const char *)cfg + f->offset;
        if (f->kind)
            json_object_set_boolean_member(o, f->key, *(const bool *)v);
        else
            json_object_set_string_member(o, f->key, v);
    }
    json_object_set_string_member(o, "diarization_sensitivity",
                                  ascolto_sensitivity_to_string(cfg->diarization_sensitivity));
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, o);
    char *s = json_to_string(n, FALSE);
    json_node_free(n);
    return s;
}
static int fail(char **error, const char *s) {
    if (error)
        *error = g_strdup(s);
    return -1;
}
int ascolto_settings_patch(ascolto_config_t *cfg, const char *json, char **error) {
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, json, -1, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        g_object_unref(p);
        return fail(error, "Expected a JSON object");
    }
    ascolto_config_t next = *cfg;
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    GList *keys = json_object_get_members(o);
    for (GList *k = keys; k; k = k->next) {
        const char *key = k->data;
        JsonNode *n = json_object_get_member(o, key);
        bool found = false;
        if (!strcmp(key, "diarization_sensitivity") && json_node_get_value_type(n) == G_TYPE_STRING) {
            const char *v = json_node_get_string(n);
            found = !strcmp(v, "low") || !strcmp(v, "medium") || !strcmp(v, "high");
            if (found)
                next.diarization_sensitivity = ascolto_sensitivity_from_string(v);
        }
        for (size_t i = 0; i < ascolto_field_count; i++) {
            const ascolto_field_t *f = &ascolto_fields[i];
            if (strcmp(f->key, key))
                continue;
            char *v = (char *)&next + f->offset;
            if (f->kind && json_node_get_value_type(n) == G_TYPE_BOOLEAN) {
                *(bool *)v = json_node_get_boolean(n);
                found = true;
            } else if (!f->kind && json_node_get_value_type(n) == G_TYPE_STRING) {
                const char *s = json_node_get_string(n);
                if (strlen(s) < f->size && !strchr(s, '\n') && !strchr(s, '\r')) {
                    strcpy(v, s);
                    found = true;
                }
            }
        }
        if (!found) {
            g_list_free(keys);
            g_object_unref(p);
            return fail(error, "Unknown setting, wrong type, or value too long");
        }
    }
    g_list_free(keys);
    g_object_unref(p);
    if (strcmp(next.appearance, "system") && strcmp(next.appearance, "light") &&
        strcmp(next.appearance, "dark"))
        return fail(error, "Choose system, light, or dark appearance");
    if (strcmp(next.recap_provider, "claude") && strcmp(next.recap_provider, "opencode") &&
        strcmp(next.recap_provider, "openrouter"))
        return fail(error, "Unknown recap provider");
    if (!g_path_is_absolute(next.output_dir))
        return fail(error, "Session folder must be an absolute path");
    const char *refs[] = {next.openai_credential, next.openrouter_credential};
    for (int i = 0; i < 2; i++)
        if (!g_str_has_prefix(refs[i], "env:") && !g_str_has_prefix(refs[i], "op://"))
            return fail(error, "Use an env:NAME or op:// credential reference, never a key");
    if (!next.recap_model[0] || !next.stt_model[0] || !next.tts_model[0] || !next.opencode_path[0])
        return fail(error, "Model and executable fields cannot be empty");
    const char *languages[] = {next.language, next.recap_lang, next.tts_lang};
    for (int i = 0; i < 3; i++) {
        const char *allowed = i ? "en it es fr de nl pl" : "auto en it es fr de nl pl";
        char **parts = g_strsplit(allowed, " ", -1);
        bool found = false;
        for (int j = 0; parts[j]; j++)
            found |= !strcmp(parts[j], languages[i]);
        g_strfreev(parts);
        if (!found)
            return fail(error, "Unsupported language code");
    }
    for (int i = 0; i < 2; i++) {
        if (g_str_has_prefix(refs[i], "env:") &&
            !g_regex_match_simple("^env:[A-Za-z_][A-Za-z_0-9]*$", refs[i], 0, 0))
            return fail(error, "Invalid environment credential reference");
        if (g_str_has_prefix(refs[i], "op://") && (strlen(refs[i]) < 8 || strchr(refs[i], ' ')))
            return fail(error, "Invalid 1Password reference");
    }
    *cfg = next;
    return 0;
}
static sqlite3 *open_db(const ascolto_config_t *cfg) {
    sqlite3 *db = NULL;
    if (!cfg->database_path[0] ||
        sqlite3_open_v2(cfg->database_path, &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 3000);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    return db;
}
int ascolto_settings_save(const ascolto_config_t *cfg) {
    sqlite3 *db = open_db(cfg);
    if (!db)
        return -1;
    char *json = ascolto_settings_json(cfg);
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(
        db, "INSERT INTO preferences VALUES(1,?) ON CONFLICT(id) DO UPDATE SET value=excluded.value", -1, &s,
        NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(s, 1, json, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(s);
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    g_free(json);
    return rc == SQLITE_DONE ? 0 : -1;
}
int ascolto_store_init(ascolto_config_t *cfg, bool explicit_config) {
    char *parent = g_path_get_dirname(cfg->database_path);
    if (g_mkdir_with_parents(parent, 0700)) {
        g_free(parent);
        return -1;
    }
    g_free(parent);
    sqlite3 *db = open_db(cfg);
    if (!db)
        return -1;
    chmod(cfg->database_path, 0600);
    sqlite3_stmt *v = NULL;
    sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &v, NULL);
    int version = sqlite3_step(v) == SQLITE_ROW ? sqlite3_column_int(v, 0) : -1;
    sqlite3_finalize(v);
    if (version < 0 || version > 1) {
        sqlite3_close(db);
        return -1;
    }
    const char *schema =
        "PRAGMA journal_mode=WAL; BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS preferences(id INTEGER PRIMARY KEY CHECK(id=1),value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sessions(id INTEGER PRIMARY KEY,dir TEXT NOT NULL UNIQUE,created TEXT "
        "DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS artifacts(session_id INTEGER REFERENCES sessions(id),name TEXT,path "
        "TEXT,PRIMARY KEY(session_id,name));"
        "CREATE TABLE IF NOT EXISTS speakers(session_id INTEGER REFERENCES sessions(id),label TEXT,PRIMARY "
        "KEY(session_id,label));"
        "CREATE TABLE IF NOT EXISTS segments(session_id INTEGER REFERENCES sessions(id),seq INTEGER,payload "
        "TEXT,PRIMARY KEY(session_id,seq));"
        "CREATE TABLE IF NOT EXISTS jobs(id INTEGER PRIMARY KEY,dir TEXT NOT NULL,kind TEXT NOT NULL,state "
        "TEXT NOT NULL,snapshot TEXT NOT NULL,created TEXT DEFAULT CURRENT_TIMESTAMP,finished TEXT);"
        "CREATE UNIQUE INDEX IF NOT EXISTS active_job ON jobs(dir) WHERE state IN ('queued','running');"
        "PRAGMA user_version=1; COMMIT;";
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db, "SELECT value FROM preferences WHERE id=1", -1, &s, NULL);
    bool exists = sqlite3_step(s) == SQLITE_ROW;
    int rc = 0;
    if (exists && !explicit_config)
        rc = ascolto_settings_patch(cfg, (const char *)sqlite3_column_text(s, 0), NULL);
    sqlite3_finalize(s);
    sqlite3_exec(
        db,
        "UPDATE jobs SET state='interrupted',finished=CURRENT_TIMESTAMP WHERE state IN ('queued','running')",
        NULL, NULL, NULL);
    sqlite3_close(db);
    if (rc)
        return rc;
    return (!exists || explicit_config) ? ascolto_settings_save(cfg) : 0;
}
static int index_segments(sqlite3 *db, const char *dir) {
    sqlite3_stmt *find = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM sessions WHERE dir=?", -1, &find, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(find, 1, dir, -1, SQLITE_TRANSIENT);
    long id = sqlite3_step(find) == SQLITE_ROW ? sqlite3_column_int64(find, 0) : 0;
    sqlite3_finalize(find);
    if (!id)
        return -1;
    char *path = g_build_filename(dir, "transcript.jsonl", NULL);
    FILE *fp = fopen(path, "r");
    g_free(path);
    if (!fp)
        return 0;
    sqlite3_stmt *segment = NULL, *speaker = NULL;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO segments VALUES(?,?,?)", -1, &segment, NULL);
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO speakers VALUES(?,?)", -1, &speaker, NULL);
    JsonParser *parser = json_parser_new();
    char *line = NULL;
    size_t capacity = 0;
    ssize_t bytes;
    int rc = 0;
    while ((bytes = getline(&line, &capacity, fp)) > 0) {
        if (bytes > 65536 || !json_parser_load_from_data(parser, line, bytes, NULL) ||
            !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
            continue;
        JsonObject *o = json_node_get_object(json_parser_get_root(parser));
        if (!json_object_has_member(o, "seq_id") || !json_object_has_member(o, "speaker"))
            continue;
        if (json_node_get_value_type(json_object_get_member(o, "seq_id")) != G_TYPE_INT64 ||
            json_node_get_value_type(json_object_get_member(o, "speaker")) != G_TYPE_STRING)
            continue;
        sqlite3_bind_int64(segment, 1, id);
        sqlite3_bind_int64(segment, 2, json_object_get_int_member(o, "seq_id"));
        sqlite3_bind_text(segment, 3, line, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(segment) != SQLITE_DONE) {
            rc = -1;
            break;
        }
        sqlite3_reset(segment);
        sqlite3_bind_int64(speaker, 1, id);
        sqlite3_bind_text(speaker, 2, json_object_get_string_member(o, "speaker"), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(speaker) != SQLITE_DONE) {
            rc = -1;
            break;
        }
        sqlite3_reset(speaker);
    }
    free(line);
    fclose(fp);
    g_object_unref(parser);
    sqlite3_finalize(segment);
    sqlite3_finalize(speaker);
    return rc;
}
int ascolto_store_index(const ascolto_config_t *cfg) {
    sqlite3 *db = open_db(cfg);
    if (!db)
        return -1;
    GDir *d = g_dir_open(cfg->output_dir, 0, NULL);
    if (!d) {
        sqlite3_close(db);
        return 0;
    }
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_prefix(name, "call_"))
            continue;
        char *dir = g_build_filename(cfg->output_dir, name, NULL);
        char *md = g_build_filename(dir, "transcript.md", NULL);
        if (g_file_test(md, G_FILE_TEST_IS_REGULAR)) {
            sqlite3_stmt *s = NULL;
            sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO sessions(dir) VALUES(?)", -1, &s, NULL);
            sqlite3_bind_text(s, 1, dir, -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
            const char *names[] = {"transcript.md", "transcript.jsonl", "transcript.cloud.md", "mic.flac",
                                   "sys.flac",      "recap.md",         "recap.spoken.txt",    "recap.mp3"};
            for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
                char *path = g_build_filename(dir, names[i], NULL);
                if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                    sqlite3_prepare_v2(
                        db, "INSERT OR REPLACE INTO artifacts SELECT id,?,? FROM sessions WHERE dir=?", -1,
                        &s, NULL);
                    sqlite3_bind_text(s, 1, names[i], -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(s, 2, path, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(s, 3, dir, -1, SQLITE_TRANSIENT);
                    sqlite3_step(s);
                    sqlite3_finalize(s);
                }
                g_free(path);
            }
            if (index_segments(db, dir)) {
                g_free(md);
                g_free(dir);
                g_dir_close(d);
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                sqlite3_close(db);
                return -1;
            }
        }
        g_free(md);
        g_free(dir);
    }
    g_dir_close(d);
    int rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}
char *ascolto_store_list(const ascolto_config_t *cfg, bool jobs) {
    sqlite3 *db = open_db(cfg);
    JsonArray *a = json_array_new();
    sqlite3_stmt *s = NULL;
    if (db && sqlite3_prepare_v2(
                  db,
                  jobs ? "SELECT id,dir,kind,state,created FROM jobs ORDER BY id DESC LIMIT 40"
                       : "SELECT id,dir,created,(SELECT group_concat(name, ', ') FROM artifacts WHERE "
                         "session_id=sessions.id) AS artifacts FROM sessions ORDER BY id DESC LIMIT 40",
                  -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            JsonObject *o = json_object_new();
            for (int i = 0; i < sqlite3_column_count(s); i++)
                json_object_set_string_member(o, sqlite3_column_name(s, i),
                                              (const char *)(sqlite3_column_text(s, i)
                                                                 ? sqlite3_column_text(s, i)
                                                                 : (const unsigned char *)""));
            json_array_add_object_element(a, o);
        }
    }
    sqlite3_finalize(s);
    if (db)
        sqlite3_close(db);
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, a);
    char *out = json_to_string(n, FALSE);
    json_node_free(n);
    return out;
}
long ascolto_job_create(const ascolto_config_t *cfg, const char *dir, bool mp3) {
    sqlite3 *db = open_db(cfg);
    if (!db)
        return -1;
    sqlite3_stmt *s = NULL;
    char *json = ascolto_settings_json(cfg);
    long id = -1;
    if (sqlite3_prepare_v2(db, "INSERT INTO jobs(dir,kind,state,snapshot) VALUES(?,?,'running',?)", -1, &s,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, dir, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, mp3 ? "speech" : "recap", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, json, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_DONE)
            id = sqlite3_last_insert_rowid(db);
    }
    sqlite3_finalize(s);
    g_free(json);
    sqlite3_close(db);
    return id;
}
void ascolto_job_finish(const ascolto_config_t *cfg, long id, bool success) {
    sqlite3 *db = open_db(cfg);
    if (!db)
        return;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db, "UPDATE jobs SET state=?,finished=CURRENT_TIMESTAMP WHERE id=?", -1, &s, NULL);
    sqlite3_bind_text(s, 1, success ? "succeeded" : "failed", -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, id);
    sqlite3_step(s);
    sqlite3_finalize(s);
    sqlite3_close(db);
}

char *ascolto_job_retry_dir(const ascolto_config_t *cfg, long id, bool *mp3) {
    sqlite3 *db = open_db(cfg);
    if (!db)
        return NULL;
    sqlite3_stmt *s = NULL;
    char *dir = NULL;
    if (sqlite3_prepare_v2(db, "SELECT dir,kind FROM jobs WHERE id=? AND state IN ('failed','interrupted')",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            dir = g_strdup((const char *)sqlite3_column_text(s, 0));
            *mp3 = !strcmp((const char *)sqlite3_column_text(s, 1), "speech");
        }
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return dir;
}

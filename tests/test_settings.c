#define _POSIX_C_SOURCE 200809L
#include "ascolto/settings.h"
#include <glib.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char *dir = g_dir_make_tmp("ascolto-settings-XXXXXX", NULL);
    g_assert_nonnull(dir);
    ascolto_config_t cfg;
    ascolto_config_init_defaults(&cfg);
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s/calls", dir);
    snprintf(cfg.database_path, sizeof(cfg.database_path), "%s/state.db", dir);
    strcpy(cfg.language, "it");
    g_assert_cmpint(ascolto_store_init(&cfg, false), ==, 0);
    g_assert_cmpint(ascolto_settings_patch(&cfg, "{\"language\":\"en\",\"appearance\":\"dark\"}", NULL), ==,
                    0);
    g_assert_cmpint(ascolto_settings_save(&cfg), ==, 0);
    strcpy(cfg.language, "it");
    g_assert_cmpint(ascolto_store_init(&cfg, false), ==, 0);
    g_assert_cmpstr(cfg.language, ==, "en");
    char *before = ascolto_settings_json(&cfg);
    g_assert_cmpint(
        ascolto_settings_patch(&cfg, "{\"language\":\"de\",\"openai_credential\":\"sk-secret\"}", NULL), ==,
        -1);
    char *after = ascolto_settings_json(&cfg);
    g_assert_cmpstr(before, ==, after);
    g_free(before);
    g_free(after);
    g_assert_cmpint(ascolto_settings_patch(&cfg, "{\"save_audio\":\"false\"}", NULL), ==, -1);
    g_assert_cmpint(ascolto_settings_patch(&cfg, "{\"unknown\":true}", NULL), ==, -1);
    char *call = g_build_filename(cfg.output_dir, "call_test", NULL);
    g_mkdir_with_parents(call, 0700);
    char *md = g_build_filename(call, "transcript.md", NULL);
    g_file_set_contents(md, "original", -1, NULL);
    g_assert_cmpint(ascolto_store_index(&cfg), ==, 0);
    g_assert_cmpint(ascolto_store_index(&cfg), ==, 0);
    char *sessions = ascolto_store_list(&cfg, false);
    g_assert_nonnull(strstr(sessions, "transcript.md"));
    g_free(sessions);
    long id = ascolto_job_create(&cfg, call, false);
    g_assert_cmpint(id, >, 0);
    g_assert_cmpint(ascolto_job_create(&cfg, call, false), ==, -1);
    ascolto_job_finish(&cfg, id, false);
    g_assert_cmpint(ascolto_job_create(&cfg, call, false), >, id);
    g_assert_cmpint(ascolto_store_init(&cfg, false), ==, 0);
    char *jobs = ascolto_store_list(&cfg, true);
    g_assert_nonnull(strstr(jobs, "interrupted"));
    g_free(jobs);
    sqlite3 *db;
    sqlite3_open(cfg.database_path, &db);
    sqlite3_exec(db, "PRAGMA user_version=999", NULL, NULL, NULL);
    sqlite3_close(db);
    g_assert_cmpint(ascolto_store_init(&cfg, false), ==, -1);
    g_free(call);
    g_free(md);
    g_free(dir);
    puts("Settings: migration, atomic validation, restart, indexing, job exclusion and recovery passed.");
    return 0;
}

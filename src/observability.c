#define _POSIX_C_SOURCE 200809L
#include "ascolto/observability.h"
#include "ascolto/core.h"
#include "ascolto/diagnostics.h"
#include "ascolto/settings.h"
#include "ascolto/settings_ui.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void json_escape(const char *src, char *dst, size_t dst_cap) {
    if (!dst || dst_cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 6 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            dst[j++] = '\\';
            dst[j++] = 't';
        } else if (c >= 32) {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

void ascolto_obs_emit(ascolto_obs_t *obs, const char *event_type, const char *json_payload_fields) {
    if (!obs || !event_type)
        return;
    pthread_mutex_lock(&obs->mutex);
    obs->event_seq++;
    uint64_t ts_ms = ascolto_monotonic_ms();
    JsonObject *event = json_object_new();
    if (json_payload_fields && *json_payload_fields) {
        char *wrapped = g_strdup_printf("{%s}", json_payload_fields);
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, wrapped, -1, NULL)) {
            JsonObject *fields = json_node_get_object(json_parser_get_root(parser));
            GList *keys = json_object_get_members(fields);
            for (GList *k = keys; k; k = k->next)
                json_object_set_member(event, k->data,
                                       json_node_copy(json_object_get_member(fields, k->data)));
            g_list_free(keys);
        } else
            json_object_set_boolean_member(event, "payload_valid", FALSE);
        g_object_unref(parser);
        g_free(wrapped);
    }
    json_object_set_int_member(event, "seq", obs->event_seq);
    json_object_set_int_member(event, "ts_ms", ts_ms);
    json_object_set_string_member(event, "event", event_type);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, event);
    char *serialized = json_to_string(node, FALSE);
    char line[1024];
    if (strlen(serialized) >= sizeof(line)) {
        json_object_remove_member(event, "text");
        json_object_set_boolean_member(event, "truncated", TRUE);
        g_free(serialized);
        serialized = json_to_string(node, FALSE);
    }
    if (strlen(serialized) >= sizeof(line))
        snprintf(line, sizeof(line),
                 "{\"seq\":%llu,\"ts_ms\":%llu,\"event\":\"event_too_large\",\"truncated\":true}",
                 (unsigned long long)obs->event_seq, (unsigned long long)ts_ms);
    else {
        strcpy(line, serialized);
    }
    g_free(serialized);
    json_node_free(node);

    size_t idx = (obs->ring_head + obs->ring_count) % ASCOLTO_OBS_RING_SIZE;
    if (obs->ring_count == ASCOLTO_OBS_RING_SIZE) {
        obs->ring_head = (obs->ring_head + 1) % ASCOLTO_OBS_RING_SIZE;
    } else {
        obs->ring_count++;
    }
    snprintf(obs->ring[idx], sizeof(obs->ring[idx]), "%s", line);

    if (obs->log_fp) {
        fprintf(obs->log_fp, "%s\n", line);
        fflush(obs->log_fp);
    }
    pthread_mutex_unlock(&obs->mutex);
}

int ascolto_obs_handle_command(ascolto_obs_t *obs, const char *cmd_line, char *reply_buf, size_t reply_cap) {
    if (!obs || !obs->core || !cmd_line || !reply_buf || reply_cap == 0)
        return -1;
    ascolto_core_t *core = obs->core;

    char buf[16384];
    snprintf(buf, sizeof(buf), "%s", cmd_line);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }

    if (!strcasecmp(buf, "OPEN_SETTINGS")) {
        pthread_mutex_lock(&core->mutex);
        core->show_settings = true;
        pthread_mutex_unlock(&core->mutex);
        snprintf(reply_buf, reply_cap, "{\"ok\":true}\n");
        return 0;
    }
    if (!strcasecmp(buf, "GET_SETTINGS")) {
        pthread_mutex_lock(&core->mutex);
        char *json = ascolto_settings_json(core->has_pending_config ? &core->pending_config : &core->config);
        pthread_mutex_unlock(&core->mutex);
        snprintf(reply_buf, reply_cap, "%s\n", json);
        g_free(json);
        return 0;
    }
    if (!strncasecmp(buf, "UPDATE_SETTINGS ", 16)) {
        char *error = NULL;
        int rc = ascolto_settings_apply(core, buf + 16, &error);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s,\"error\":\"%s\"}\n", rc ? "false" : "true",
                 error ? error : "");
        g_free(error);
        return 0;
    }
    if (!strcasecmp(buf, "LIST_SESSIONS") || !strcasecmp(buf, "LIST_JOBS")) {
        char *json = ascolto_store_list(&core->config, !strcasecmp(buf, "LIST_JOBS"));
        snprintf(reply_buf, reply_cap, "%s\n", json);
        g_free(json);
        return 0;
    }
    if (!strncasecmp(buf, "RETRY_JOB ", 10)) {
        char *end = NULL;
        long id = strtol(buf + 10, &end, 10);
        bool mp3 = false;
        char *dir = (id > 0 && end && !*end) ? ascolto_job_retry_dir(&core->config, id, &mp3) : NULL;
        int rc = -1;
        if (dir) {
            char *md = g_build_filename(dir, "transcript.md", NULL);
            rc = mp3 ? ascolto_core_make_mp3(core, md) : ascolto_core_run_recap(core, md);
            g_free(md);
            g_free(dir);
        }
        snprintf(reply_buf, reply_cap, "{\"ok\":%s}\n", rc ? "false" : "true");
        return 0;
    }
    if (!strcasecmp(buf, "GET_PROVIDER_STATUS")) {
        JsonObject *o = json_object_new();
        json_object_set_boolean_member(o, "ok", TRUE);
        pthread_mutex_lock(&core->mutex);
        ascolto_config_t cfg = core->has_pending_config ? core->pending_config : core->config;
        pthread_mutex_unlock(&core->mutex);
        char *path = g_find_program_in_path(cfg.opencode_path);
        json_object_set_boolean_member(o, "opencode_executable_found", path != NULL);
        g_free(path);
        path = g_find_program_in_path("claude");
        json_object_set_boolean_member(o, "claude_executable_found", path != NULL);
        g_free(path);
        json_object_set_string_member(o, "selected_recap_provider", cfg.recap_provider);
        json_object_set_string_member(o, "openai_status",
                                      "credential reference configured; use Test connection to verify");
        json_object_set_string_member(o, "openrouter_status",
                                      "credential reference configured; use Test connection to verify");
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, o);
        char *json = json_to_string(n, FALSE);
        snprintf(reply_buf, reply_cap, "%s\n", json);
        g_free(json);
        json_node_free(n);
        return 0;
    }
    if (!strcasecmp(buf, "DIAGNOSTICS")) {
        char *json = ascolto_diagnostics(core);
        snprintf(reply_buf, reply_cap, "%s\n", json);
        g_free(json);
        return 0;
    }
    if (strcasecmp(buf, "STATUS") == 0) {
        JsonObject *o = json_object_new();
        json_object_set_boolean_member(o, "ok", TRUE);
        pthread_mutex_lock(&core->mutex);
        json_object_set_string_member(o, "state", ascolto_state_to_string(core->state));
        json_object_set_int_member(o, "session_id", core->session_id);
        json_object_set_int_member(o, "elapsed_sec", ascolto_core_elapsed_sec(core));
        json_object_set_boolean_member(o, "auto_mic_trigger", core->config.auto_mic_trigger);
        json_object_set_boolean_member(o, "external_mic_active", core->external_mic_active);
        json_object_set_string_member(o, "active_mic_app", core->active_mic_app);
        json_object_set_string_member(o, "language", core->config.language);
        json_object_set_string_member(o, "mic_device", core->config.mic_device);
        json_object_set_string_member(o, "sys_device", core->config.sys_device);
        json_object_set_string_member(o, "sensitivity",
                                      ascolto_sensitivity_to_string(core->config.diarization_sensitivity));
        json_object_set_double_member(o, "threshold",
                                      ascolto_sensitivity_threshold(core->config.diarization_sensitivity,
                                                                    core->config.custom_threshold));
        json_object_set_string_member(o, "keywords", core->config.keywords);
        json_object_set_boolean_member(o, "whistle_loaded", core->whistle.model_loaded);
        json_object_set_int_member(o, "speaker_count", core->diarizer.speaker_count);
        json_object_set_int_member(o, "total_segments", core->total_segments);
        json_object_set_string_member(o, "output_dir", core->config.output_dir);
        json_object_set_string_member(o, "current_md_path", core->current_md_path);
        json_object_set_string_member(o, "current_jsonl_path", core->current_jsonl_path);
        int active = 0;
        for (int i = 0; i < 8; i++)
            active += core->worker_pid[i] > 0;
        json_object_set_int_member(o, "active_jobs", active);
        json_object_set_boolean_member(o, "settings_pending", core->has_pending_config);
        pthread_mutex_unlock(&core->mutex);
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, o);
        char *json = json_to_string(n, FALSE);
        snprintf(reply_buf, reply_cap, "%s\n", json);
        g_free(json);
        json_node_free(n);
        return 0;
    }

    if (strcasecmp(buf, "START") == 0) {
        int rc = ascolto_core_start_manual(core);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s,\"state\":\"%s\",\"session_id\":%llu}\n",
                 rc == 0 ? "true" : "false", ascolto_state_to_string(core->state),
                 (unsigned long long)core->session_id);
        return 0;
    }

    if (strcasecmp(buf, "STOP") == 0) {
        int rc = ascolto_core_stop(core);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s,\"state\":\"%s\",\"session_id\":%llu}\n",
                 rc == 0 ? "true" : "false", ascolto_state_to_string(core->state),
                 (unsigned long long)core->session_id);
        return 0;
    }

    if (strcasecmp(buf, "TOGGLE") == 0) {
        int rc = 0;
        if (core->state == ASCOLTO_STATE_IDLE) {
            rc = ascolto_core_start_manual(core);
        } else {
            rc = ascolto_core_stop(core);
        }
        snprintf(reply_buf, reply_cap, "{\"ok\":%s,\"state\":\"%s\",\"session_id\":%llu}\n",
                 rc == 0 ? "true" : "false", ascolto_state_to_string(core->state),
                 (unsigned long long)core->session_id);
        return 0;
    }

    if (strcasecmp(buf, "PAUSE") == 0) {
        int rc = ascolto_core_pause(core);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s,\"state\":\"%s\"}\n", rc == 0 ? "true" : "false",
                 ascolto_state_to_string(core->state));
        return 0;
    }

    if (strcasecmp(buf, "RESUME") == 0) {
        int rc = ascolto_core_resume(core);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s,\"state\":\"%s\"}\n", rc == 0 ? "true" : "false",
                 ascolto_state_to_string(core->state));
        return 0;
    }

    if (strncasecmp(buf, "SET_AUTO_MIC ", 13) == 0) {
        const char *arg = buf + 13;
        bool en = (strcmp(arg, "1") == 0 || strcasecmp(arg, "true") == 0 || strcasecmp(arg, "on") == 0);
        ascolto_core_set_auto_mic(core, en);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"auto_mic_trigger\":%s}\n", en ? "true" : "false");
        return 0;
    }

    if (strncasecmp(buf, "SET_SAVE_AUDIO ", 15) == 0) {
        const char *arg = buf + 15;
        bool en = (strcmp(arg, "1") == 0 || strcasecmp(arg, "true") == 0 || strcasecmp(arg, "on") == 0);
        ascolto_core_set_save_audio(core, en);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"save_audio\":%s}\n", en ? "true" : "false");
        return 0;
    }

    if (strncasecmp(buf, "SET_CLOUD_RECAP ", 16) == 0) {
        const char *arg = buf + 16;
        bool en = (strcmp(arg, "1") == 0 || strcasecmp(arg, "true") == 0 || strcasecmp(arg, "on") == 0);
        ascolto_core_set_cloud_recap(core, en);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"cloud_recap\":%s}\n", en ? "true" : "false");
        return 0;
    }

    if (strncasecmp(buf, "SET_RECAP_TTS ", 14) == 0) {
        const char *arg = buf + 14;
        bool en = (strcmp(arg, "1") == 0 || strcasecmp(arg, "true") == 0 || strcasecmp(arg, "on") == 0);
        ascolto_core_set_recap_tts(core, en);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"recap_tts\":%s}\n", en ? "true" : "false");
        return 0;
    }

    if (strncasecmp(buf, "RECAP", 5) == 0 && (buf[5] == '\0' || buf[5] == ' ')) {
        int rc = ascolto_core_run_recap(core, buf[5] == ' ' ? buf + 6 : NULL);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s}\n", rc == 0 ? "true" : "false");
        return 0;
    }

    if (strncasecmp(buf, "SET_RECAP_LANG ", 15) == 0) {
        int rc = ascolto_core_set_recap_lang(core, buf + 15);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s}\n", rc == 0 ? "true" : "false");
        return 0;
    }

    if (strncasecmp(buf, "SET_TTS_LANG ", 13) == 0) {
        int rc = ascolto_core_set_tts_lang(core, buf + 13);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s}\n", rc == 0 ? "true" : "false");
        return 0;
    }

    if (strncasecmp(buf, "MAKE_MP3", 8) == 0 && (buf[8] == '\0' || buf[8] == ' ')) {
        int rc = ascolto_core_make_mp3(core, buf[8] == ' ' ? buf + 9 : NULL);
        snprintf(reply_buf, reply_cap, "{\"ok\":%s}\n", rc == 0 ? "true" : "false");
        return 0;
    }

    if (strcasecmp(buf, "OPEN_SESSION_FOLDER") == 0) {
        ascolto_tray_open_session_folder(&core->tray);
        char dir_e[512];
        json_escape(core->current_session_dir[0] ? core->current_session_dir : core->config.output_dir, dir_e,
                    sizeof(dir_e));
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"opened\":\"%s\"}\n", dir_e);
        return 0;
    }

    if (strncasecmp(buf, "SET_LANG ", 9) == 0) {
        ascolto_core_set_language(core, buf + 9);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"language\":\"%s\"}\n", core->config.language);
        return 0;
    }

    if (strncasecmp(buf, "SET_MIC ", 8) == 0) {
        ascolto_core_set_mic_device(core, buf + 8);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"mic_device\":\"%s\"}\n", core->config.mic_device);
        return 0;
    }

    if (strncasecmp(buf, "SET_SYS ", 8) == 0) {
        ascolto_core_set_sys_device(core, buf + 8);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"sys_device\":\"%s\"}\n", core->config.sys_device);
        return 0;
    }

    if (strncasecmp(buf, "SET_SENSITIVITY ", 16) == 0) {
        ascolto_sensitivity_t s = ascolto_sensitivity_from_string(buf + 16);
        ascolto_core_set_sensitivity(core, s);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"sensitivity\":\"%s\"}\n",
                 ascolto_sensitivity_to_string(core->config.diarization_sensitivity));
        return 0;
    }

    if (strncasecmp(buf, "SET_KEYWORDS ", 13) == 0) {
        ascolto_core_set_keywords(core, buf + 13);
        char kw_esc[512];
        json_escape(core->config.keywords, kw_esc, sizeof(kw_esc));
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"keywords\":\"%s\"}\n", kw_esc);
        return 0;
    }

    if ((!strncmp(buf, "INJECT_", 7) || !strncmp(buf, "SIM_", 4)) && !core->audio.use_mock) {
        snprintf(reply_buf, reply_cap, "{\"ok\":false,\"error\":\"Simulation requires --mock-audio\"}\n");
        return 0;
    }
    if (!strncmp(buf, "SIM_CAPTURE_FAILURE ", 20)) {
        pthread_mutex_lock(&core->mutex);
        core->mock_capture_failure = !strcmp(buf + 20, "on");
        pthread_mutex_unlock(&core->mutex);
        snprintf(reply_buf, reply_cap, "{\"ok\":true}\n");
        return 0;
    }
    if (strncasecmp(buf, "SIM_MIC_ACTIVE ", 15) == 0) {
        const char *rest = buf + 15;
        bool act = (rest[0] == '1' || strncasecmp(rest, "true", 4) == 0);
        const char *sp = strchr(rest, ' ');
        const char *app = (sp && *(sp + 1)) ? (sp + 1) : "SimulatedCallApp";
        ascolto_core_notify_mic_trigger(core, act, app);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"external_mic_active\":%s,\"state\":\"%s\"}\n",
                 act ? "true" : "false", ascolto_state_to_string(core->state));
        return 0;
    }

    if (strncasecmp(buf, "INJECT_WAV ", 11) == 0) {
        char chan_str[16] = {0};
        char path_str[512] = {0};
        char hint_str[512] = {0};
        int n = sscanf(buf + 11, "%15s %511s %511[^\n]", chan_str, path_str, hint_str);
        if (n < 2 || (strcmp(chan_str, "MIC") && strcmp(chan_str, "SYS"))) {
            snprintf(reply_buf, reply_cap,
                     "{\"ok\":false,\"error\":\"usage: INJECT_WAV MIC|SYS <path> [hint]\"}\n");
            return 0;
        }
        ascolto_channel_t ch = (strcasecmp(chan_str, "MIC") == 0) ? ASCOLTO_CHANNEL_MIC : ASCOLTO_CHANNEL_SYS;
        float *samples = NULL;
        size_t count = 0;
        if (ascolto_wav_load_16k_mono(path_str, &samples, &count) != 0) {
            snprintf(reply_buf, reply_cap, "{\"ok\":false,\"error\":\"failed to load wav\"}\n");
            return 0;
        }
        double t0 = core->session_audio_sec[ch];
        ascolto_core_ingest_audio(core, ch, samples, count, t0, (n >= 3 && *hint_str) ? hint_str : NULL);
        ascolto_core_flush_channel(core, ch);
        free(samples);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"injected_samples\":%zu,\"total_segments\":%llu}\n",
                 count, (unsigned long long)core->total_segments);
        return 0;
    }

    if (strncasecmp(buf, "INJECT_SYNTH_SPEECH ", 20) == 0) {
        char chan_str[16] = {0};
        float f0 = 120.0f, f1 = 500.0f, f2 = 1500.0f;
        int dur_ms = 1200;
        char text_str[512] = {0};
        int n = sscanf(buf + 20, "%15s %f %f %f %d %511[^\n]", chan_str, &f0, &f1, &f2, &dur_ms, text_str);
        if (n < 6 || (strcmp(chan_str, "MIC") && strcmp(chan_str, "SYS")) || !isfinite(f0) || !isfinite(f1) ||
            !isfinite(f2) || f0 <= 0 || f1 <= 0 || f2 <= 0) {
            snprintf(reply_buf, reply_cap,
                     "{\"ok\":false,\"error\":\"usage: INJECT_SYNTH_SPEECH MIC|SYS <f0> <f1> <f2> <dur_ms> "
                     "<text>\"}\n");
            return 0;
        }
        if (dur_ms < 200)
            dur_ms = 200;
        if (dur_ms > 15000)
            dur_ms = 15000;
        size_t count = (size_t)((dur_ms * ASCOLTO_SAMPLE_RATE) / 1000);
        float *pcm = (float *)calloc(count, sizeof(float));
        if (!pcm) {
            snprintf(reply_buf, reply_cap, "{\"ok\":false,\"error\":\"oom\"}\n");
            return 0;
        }
        ascolto_synth_voice_utterance(pcm, count, f0, f1, f2, f2 + 1000.0f);
        ascolto_channel_t ch = (strcasecmp(chan_str, "MIC") == 0) ? ASCOLTO_CHANNEL_MIC : ASCOLTO_CHANNEL_SYS;
        double t0 = core->session_audio_sec[ch];
        ascolto_core_ingest_audio(core, ch, pcm, count, t0, text_str);
        ascolto_core_flush_channel(core, ch);
        free(pcm);
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"total_segments\":%llu,\"speaker_count\":%d}\n",
                 (unsigned long long)core->total_segments, core->diarizer.speaker_count);
        return 0;
    }

    if (strcasecmp(buf, "LIST_DEVICES") == 0) {
        ascolto_device_info_t devs[ASCOLTO_MAX_DEVICES];
        size_t count = 0;
        core->audio.list_devices(&core->audio, devs, ASCOLTO_MAX_DEVICES, &count);
        size_t off = (size_t)snprintf(reply_buf, reply_cap, "{\"ok\":true,\"devices\":[");
        for (size_t i = 0; i < count && off + 256 < reply_cap; i++) {
            char name_e[256], desc_e[256];
            json_escape(devs[i].name, name_e, sizeof(name_e));
            json_escape(devs[i].description, desc_e, sizeof(desc_e));
            off += (size_t)snprintf(reply_buf + off, reply_cap - off,
                                    "%s{\"id\":\"%s\",\"name\":\"%s\",\"description\":\"%s\","
                                    "\"is_monitor\":%s,\"is_default\":%s}",
                                    (i > 0) ? "," : "", devs[i].id, name_e, desc_e,
                                    devs[i].is_monitor ? "true" : "false",
                                    devs[i].is_default ? "true" : "false");
        }
        snprintf(reply_buf + off, reply_cap - off, "]}\n");
        return 0;
    }

    if (strcasecmp(buf, "LIST_SPEAKERS") == 0) {
        pthread_mutex_lock(&core->mutex);
        size_t off = (size_t)snprintf(reply_buf, reply_cap, "{\"ok\":true,\"speakers\":[");
        for (int i = 0; i < core->diarizer.speaker_count && off + 200 < reply_cap; i++) {
            const ascolto_speaker_profile_t *sp = &core->diarizer.speakers[i];
            off += (size_t)snprintf(reply_buf + off, reply_cap - off,
                                    "%s{\"speaker_id\":%d,\"label\":\"%s\",\"utterances\":%u,"
                                    "\"mean_pitch_hz\":%.1f,\"last_active_sec\":%.2f}",
                                    (i > 0) ? "," : "", sp->speaker_id, sp->label, sp->utterance_count,
                                    sp->mean_pitch_hz, sp->last_active_sec);
        }
        snprintf(reply_buf + off, reply_cap - off, "]}\n");
        pthread_mutex_unlock(&core->mutex);
        return 0;
    }

    if (strcasecmp(buf, "LIST_SEGMENTS") == 0) {
        pthread_mutex_lock(&core->mutex);
        size_t off = (size_t)snprintf(reply_buf, reply_cap, "{\"ok\":true,\"segments\":[");
        for (size_t i = 0; i < core->segments_count && off + 512 < reply_cap; i++) {
            const ascolto_segment_t *sg = &core->segments[i];
            char txt_e[512];
            json_escape(sg->text, txt_e, sizeof(txt_e));
            off += (size_t)snprintf(reply_buf + off, reply_cap - off,
                                    "%s{\"seq_id\":%llu,\"channel\":\"%s\",\"speaker\":\"%s\","
                                    "\"speaker_id\":%d,\"similarity\":%.3f,\"start_sec\":%.2f,"
                                    "\"end_sec\":%.2f,\"pitch_hz\":%.1f,\"language\":\"%s\",\"text\":\"%s\"}",
                                    (i > 0) ? "," : "", (unsigned long long)sg->seq_id,
                                    ascolto_channel_to_string(sg->channel), sg->speaker_label, sg->speaker_id,
                                    sg->speaker_similarity, sg->start_sec, sg->end_sec, sg->pitch_hz,
                                    sg->language, txt_e);
        }
        snprintf(reply_buf + off, reply_cap - off, "]}\n");
        pthread_mutex_unlock(&core->mutex);
        return 0;
    }

    if (strcasecmp(buf, "GET_TRAY_STATE") == 0) {
        ascolto_tray_snapshot_t snap;
        ascolto_tray_get_snapshot(&core->tray, &snap);
        char lbl_e[128], st_e[256], kw_e[256];
        json_escape(snap.label, lbl_e, sizeof(lbl_e));
        json_escape(snap.status_text, st_e, sizeof(st_e));
        json_escape(snap.keywords_summary, kw_e, sizeof(kw_e));
        snprintf(
            reply_buf, reply_cap,
            "{\"ok\":true,\"active\":%s,\"gui_indicator\":%s,\"icon\":\"%s\","
            "\"label\":\"%s\",\"status_text\":\"%s\",\"recording\":%s,\"paused\":%s,"
            "\"auto_mic_checked\":%s,\"save_audio_checked\":%s,\"cloud_recap_checked\":%s,\"recap_tts_"
            "checked\":%s,\"language\":\"%s\",\"mic\":\"%s\",\"sys\":\"%s\","
            "\"sensitivity\":\"%s\",\"keywords_summary\":\"%s\",\"recent_files_count\":%zu}\n",
            snap.active ? "true" : "false", snap.using_gui_indicator ? "true" : "false", snap.icon_name,
            lbl_e, st_e, snap.menu_recording_enabled ? "true" : "false", snap.menu_paused ? "true" : "false",
            snap.menu_auto_mic_checked ? "true" : "false", snap.menu_save_audio_checked ? "true" : "false",
            snap.menu_cloud_recap_checked ? "true" : "false", snap.menu_recap_tts_checked ? "true" : "false",
            snap.selected_language, snap.selected_mic, snap.selected_sys,
            ascolto_sensitivity_to_string(snap.selected_sensitivity), kw_e, snap.recent_files_count);
        return 0;
    }

    if (!strcasecmp(buf, "GET_EVENTS") || !strncasecmp(buf, "GET_EVENTS ", 11)) {
        uint64_t after = buf[10] == ' ' ? g_ascii_strtoull(buf + 11, NULL, 10) : 0;
        pthread_mutex_lock(&obs->mutex);
        uint64_t oldest = obs->event_seq - obs->ring_count + 1, last = after;
        size_t off = (size_t)snprintf(
            reply_buf, reply_cap,
            "{\"ok\":true,\"oldest_seq\":%llu,\"latest_seq\":%llu,\"lost_events\":%llu,\"events\":[",
            (unsigned long long)oldest, (unsigned long long)obs->event_seq,
            (unsigned long long)(after + 1 < oldest ? oldest - after - 1 : 0));
        size_t emitted = 0;
        for (size_t i = 0; i < obs->ring_count && off + 1200 < reply_cap; i++) {
            uint64_t seq = oldest + i;
            if (seq <= after)
                continue;
            size_t idx = (obs->ring_head + i) % ASCOLTO_OBS_RING_SIZE;
            off += (size_t)snprintf(reply_buf + off, reply_cap - off, "%s%s", emitted++ ? "," : "",
                                    obs->ring[idx]);
            last = seq;
        }
        snprintf(reply_buf + off, reply_cap - off, "],\"next_after\":%llu,\"has_more\":%s}\n",
                 (unsigned long long)last, last < obs->event_seq ? "true" : "false");
        pthread_mutex_unlock(&obs->mutex);
        return 0;
    }

    if (strcasecmp(buf, "QUIT") == 0) {
        core->running = false;
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"quitting\":true}\n");
        return 0;
    }

    if (strcasecmp(buf, "OPEN_FOLDER") == 0) {
        ascolto_tray_open_transcripts_folder(&core->tray);
        char dir_e[512];
        json_escape(core->config.output_dir, dir_e, sizeof(dir_e));
        snprintf(reply_buf, reply_cap, "{\"ok\":true,\"opened\":\"%s\"}\n", dir_e);
        return 0;
    }

    snprintf(reply_buf, reply_cap, "{\"ok\":false,\"error\":\"unknown command\"}\n");
    return 0;
}

static void *obs_server_loop(void *arg) {
    ascolto_obs_t *obs = (ascolto_obs_t *)arg;
    char cmd[16384];
    char *reply = (char *)malloc(65536);
    if (!reply)
        return NULL;

    while (obs->running) {
        struct pollfd pfd;
        pfd.fd = obs->listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int prc = poll(&pfd, 1, 30);
        if (prc <= 0)
            continue;

        int cfd = accept(obs->listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            break;
        }
        fcntl(cfd, F_SETFD, FD_CLOEXEC);

        struct pollfd cpfd;
        cpfd.fd = cfd;
        cpfd.events = POLLIN;
        cpfd.revents = 0;
        if (poll(&cpfd, 1, 1000) > 0) {
            size_t used = 0;
            bool complete = false;
            while (used + 1 < sizeof(cmd)) {
                ssize_t n = read(cfd, cmd + used, sizeof(cmd) - 1 - used);
                if (n <= 0)
                    break;
                used += (size_t)n;
                cmd[used] = 0;
                if (strchr(cmd, '\n')) {
                    complete = true;
                    break;
                }
                if (poll(&cpfd, 1, 1000) <= 0)
                    break;
            }
            if (used > 0) {
                cmd[used] = '\0';
                if (!complete) {
                    const char *error = "{\"ok\":false,\"error\":\"Incomplete or oversized command\"}\n";
                    ssize_t ignored = write(cfd, error, strlen(error));
                    (void)ignored;
                    close(cfd);
                    continue;
                }
                reply[0] = '\0';
                ascolto_obs_handle_command(obs, cmd, reply, 65536);
                size_t rlen = strlen(reply);
                size_t sent = 0;
                while (sent < rlen) {
                    ssize_t w = write(cfd, reply + sent, rlen - sent);
                    if (w <= 0)
                        break;
                    sent += (size_t)w;
                }
            }
        }
        close(cfd);
    }

    free(reply);
    return NULL;
}

int ascolto_obs_init(ascolto_obs_t *obs, ascolto_core_t *core, const char *socket_path,
                     const char *log_path) {
    if (!obs)
        return -1;
    memset(obs, 0, sizeof(*obs));
    obs->core = core;
    obs->listen_fd = -1;
    pthread_mutex_init(&obs->mutex, NULL);

    if (log_path && *log_path) {
        snprintf(obs->log_path, sizeof(obs->log_path), "%s", log_path);
        obs->log_fp = fopen(log_path, "a");
        if (obs->log_fp) {
            fcntl(fileno(obs->log_fp), F_SETFD, FD_CLOEXEC);
        }
    }

    if (socket_path && *socket_path) {
        snprintf(obs->socket_path, sizeof(obs->socket_path), "%s", socket_path);
        unlink(obs->socket_path);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", obs->socket_path);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && chmod(obs->socket_path, 0600) == 0 &&
                listen(fd, 32) == 0) {
                obs->listen_fd = fd;
                obs->running = true;
                pthread_create(&obs->server_thread, NULL, obs_server_loop, obs);
            } else {
                close(fd);
            }
        }
    }
    return 0;
}

void ascolto_obs_destroy(ascolto_obs_t *obs) {
    if (!obs)
        return;
    if (obs->running) {
        obs->running = false;
        pthread_join(obs->server_thread, NULL);
    }
    if (obs->listen_fd >= 0) {
        close(obs->listen_fd);
        obs->listen_fd = -1;
    }
    if (obs->socket_path[0]) {
        unlink(obs->socket_path);
    }
    if (obs->log_fp) {
        fclose(obs->log_fp);
        obs->log_fp = NULL;
    }
    pthread_mutex_destroy(&obs->mutex);
}

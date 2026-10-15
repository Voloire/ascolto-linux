#define _POSIX_C_SOURCE 200809L
#include "ascolto/tray_ui.h"
#include "ascolto/core.h"
#include "ascolto/settings_ui.h"
#include <dlfcn.h>
#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    void *library, *indicator;
    void (*set_icon)(void *, const char *, const char *);
    void (*set_label)(void *, const char *, const char *);
    GtkWidget *menu, *status, *start, *pause, *automatic, *recap, *speech, *recent, *jobs;
    bool updating;
    char *assets;
} Tray;
static void open_path(const char *path) {
    if (!path || !*path)
        return;
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) {
        g_app_info_launch_default_for_uri(uri, NULL, NULL);
        g_free(uri);
    }
}
void ascolto_tray_open_transcripts_folder(ascolto_tray_ui_t *ui) {
    if (ui && ui->core)
        open_path(ui->core->config.output_dir);
}
void ascolto_tray_open_session_folder(ascolto_tray_ui_t *ui) {
    if (ui && ui->core)
        open_path(ui->core->current_session_dir[0] ? ui->core->current_session_dir
                                                   : ui->core->config.output_dir);
}
static void start(GtkWidget *w, gpointer data) {
    (void)w;
    ascolto_core_t *c = data;
    if (c->state == ASCOLTO_STATE_IDLE)
        ascolto_core_start_manual(c);
    else
        ascolto_core_stop(c);
}
static void pause_session(GtkWidget *w, gpointer data) {
    (void)w;
    ascolto_core_t *c = data;
    if (c->state == ASCOLTO_STATE_PAUSED)
        ascolto_core_resume(c);
    else
        ascolto_core_pause(c);
}
static void automatic(GtkCheckMenuItem *w, gpointer data) {
    ascolto_core_t *c = data;
    Tray *t = c->tray.gtk_impl;
    if (!t->updating)
        ascolto_core_set_auto_mic(c, gtk_check_menu_item_get_active(w));
}
static void settings(GtkWidget *w, gpointer data) {
    (void)w;
    ascolto_settings_open(data);
}
static void recap(GtkWidget *w, gpointer data) {
    (void)w;
    ascolto_core_run_recap(data, NULL);
}
static void speech(GtkWidget *w, gpointer data) {
    (void)w;
    ascolto_core_make_mp3(data, NULL);
}
static void folder(GtkWidget *w, gpointer data) {
    (void)w;
    ascolto_core_t *c = data;
    ascolto_tray_open_session_folder(&c->tray);
}
static void recent_folder(GtkWidget *w, gpointer data) {
    (void)w;
    open_path(data);
}
static void free_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}
static void quit(GtkWidget *w, gpointer data) {
    (void)w;
    ((ascolto_core_t *)data)->running = false;
}
static GtkWidget *item(Tray *t, const char *name, GCallback callback, ascolto_core_t *c) {
    GtkWidget *w = gtk_menu_item_new_with_label(name);
    if (callback)
        g_signal_connect(w, "activate", callback, c);
    gtk_menu_shell_append(GTK_MENU_SHELL(t->menu), w);
    return w;
}
static void separator(Tray *t) {
    gtk_menu_shell_append(GTK_MENU_SHELL(t->menu), gtk_separator_menu_item_new());
}
int ascolto_tray_init(ascolto_tray_ui_t *ui, ascolto_core_t *core, bool headless) {
    memset(ui, 0, sizeof(*ui));
    ui->core = core;
    ui->headless = headless;
    ui->running = true;
    pthread_mutex_init(&ui->mutex, NULL);
    if (!headless && gtk_init_check(NULL, NULL)) {
        Tray *t = g_new0(Tray, 1);
        t->library = dlopen("libayatana-appindicator3.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (t->library) {
            void *(*create)(const char *, const char *, int) = dlsym(t->library, "app_indicator_new");
            void (*set_menu)(void *, GtkMenu *) = dlsym(t->library, "app_indicator_set_menu");
            void (*set_status)(void *, int) = dlsym(t->library, "app_indicator_set_status");
            t->set_icon = dlsym(t->library, "app_indicator_set_icon_full");
            t->set_label = dlsym(t->library, "app_indicator_set_label");
            if (create && set_menu && set_status && t->set_icon && t->set_label) {
                t->indicator = create("ascolto-indicator", "audio-input-microphone-symbolic", 0);
                t->menu = gtk_menu_new();
                g_object_ref_sink(t->menu);
                t->status = item(t, "Ready to listen", NULL, core);
                gtk_widget_set_sensitive(t->status, FALSE);
                t->jobs = item(t, "No background jobs", NULL, core);
                gtk_widget_set_sensitive(t->jobs, FALSE);
                separator(t);
                t->start = item(t, "Start recording", G_CALLBACK(start), core);
                t->pause = item(t, "Pause", G_CALLBACK(pause_session), core);
                t->automatic = gtk_check_menu_item_new_with_label("Automatic microphone activation");
                g_signal_connect(t->automatic, "toggled", G_CALLBACK(automatic), core);
                gtk_menu_shell_append(GTK_MENU_SHELL(t->menu), t->automatic);
                separator(t);
                item(t, "Open session folder", G_CALLBACK(folder), core);
                t->recent = item(t, "Recent sessions", NULL, core);
                t->recap = item(t, "Generate recap", G_CALLBACK(recap), core);
                t->speech = item(t, "Generate MP3", G_CALLBACK(speech), core);
                separator(t);
                item(t, "Settings…", G_CALLBACK(settings), core);
                item(t, "Quit", G_CALLBACK(quit), core);
                char exe[1024];
                ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
                if (n > 0) {
                    exe[n] = 0;
                    char *dir = g_path_get_dirname(exe);
                    t->assets = g_build_filename(dir, "..", "assets", NULL);
                    g_free(dir);
                } else
                    t->assets = g_strdup("assets");
                gtk_widget_show_all(t->menu);
                set_menu(t->indicator, GTK_MENU(t->menu));
                set_status(t->indicator, 1);
                ui->gtk_impl = t;
                ui->snapshot.using_gui_indicator = true;
            }
        }
        if (!ui->gtk_impl) {
            if (t->library)
                dlclose(t->library);
            g_free(t);
        }
    }
    ascolto_tray_update(ui);
    return 0;
}
void ascolto_tray_update(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->core)
        return;
    ascolto_core_t *core = ui->core;

    uint64_t elapsed = ascolto_core_elapsed_sec(core);
    unsigned int mins = (unsigned int)(elapsed / 60);
    unsigned int secs = (unsigned int)(elapsed % 60);

    pthread_mutex_lock(&ui->mutex);
    ascolto_tray_snapshot_t *s = &ui->snapshot;
    s->active = true;
    s->menu_auto_mic_checked = core->config.auto_mic_trigger;
    s->menu_save_audio_checked = core->config.save_audio;
    s->menu_cloud_recap_checked = core->config.cloud_recap;
    s->menu_recap_tts_checked = core->config.recap_tts;
    snprintf(s->selected_language, sizeof(s->selected_language), "%s", core->config.language);
    snprintf(s->selected_mic, sizeof(s->selected_mic), "%s", core->config.mic_device);
    snprintf(s->selected_sys, sizeof(s->selected_sys), "%s", core->config.sys_device);
    s->selected_sensitivity = core->config.diarization_sensitivity;
    snprintf(s->keywords_summary, sizeof(s->keywords_summary), "%.255s",
             core->config.keywords[0] ? core->config.keywords : "(none)");

    s->recent_files_count = core->recent_files_count;
    for (size_t i = 0; i < core->recent_files_count && i < ASCOLTO_MAX_RECENT_FILES; i++) {
        snprintf(s->recent_files[i], sizeof(s->recent_files[i]), "%s", core->recent_files[i]);
    }

    if (core->state == ASCOLTO_STATE_IDLE) {
        snprintf(s->icon_name, sizeof(s->icon_name), "ascolto-idle-symbolic");
        snprintf(s->label, sizeof(s->label), "Ascolto");
        snprintf(s->status_text, sizeof(s->status_text), "Ready to listen");
        s->menu_recording_enabled = false;
        s->menu_paused = false;
    } else if (core->state == ASCOLTO_STATE_PAUSED) {
        snprintf(s->icon_name, sizeof(s->icon_name), "ascolto-paused-symbolic");
        snprintf(s->label, sizeof(s->label), "PAUSED %02u:%02u", mins, secs);
        snprintf(s->status_text, sizeof(s->status_text), "Paused (%02u:%02u)", mins, secs);
        s->menu_recording_enabled = true;
        s->menu_paused = true;
    } else {
        snprintf(s->icon_name, sizeof(s->icon_name), "ascolto-recording-symbolic");
        snprintf(s->label, sizeof(s->label), "REC %02u:%02u", mins, secs);
        snprintf(s->status_text, sizeof(s->status_text), "Recording (%02u:%02u) [%s]", mins, secs,
                 core->state == ASCOLTO_STATE_RECORDING_AUTO ? "Auto-Mic" : "Manual");
        s->menu_recording_enabled = true;
        s->menu_paused = false;
    }

    ui->dirty = true;
    pthread_mutex_unlock(&ui->mutex);
}

void ascolto_tray_get_snapshot(ascolto_tray_ui_t *ui, ascolto_tray_snapshot_t *out_snap) {
    if (!ui || !out_snap)
        return;
    pthread_mutex_lock(&ui->mutex);
    *out_snap = ui->snapshot;
    pthread_mutex_unlock(&ui->mutex);
}

void ascolto_tray_step_iteration(ascolto_tray_ui_t *ui) {
    Tray *t = ui->gtk_impl;
    if (!t)
        return;
    ascolto_tray_snapshot_t s;
    ascolto_tray_get_snapshot(ui, &s);
    if (!ui->dirty)
        return;
    ui->dirty = false;
    t->updating = true;
    char *file = g_strconcat(s.icon_name, ".svg", NULL);
    char *path = g_build_filename(t->assets, file, NULL);
    t->set_icon(t->indicator, path, s.status_text);
    g_free(file);
    g_free(path);
    t->set_label(t->indicator, s.label, "REC 99:99");
    gtk_menu_item_set_label(GTK_MENU_ITEM(t->status), s.status_text);
    gtk_menu_item_set_label(GTK_MENU_ITEM(t->start),
                            s.menu_recording_enabled ? "Stop recording" : "Start recording");
    gtk_menu_item_set_label(GTK_MENU_ITEM(t->pause), s.menu_paused ? "Resume" : "Pause");
    gtk_widget_set_sensitive(t->pause, s.menu_recording_enabled);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(t->automatic), s.menu_auto_mic_checked);
    bool busy = false;
    for (int i = 0; i < 8; i++)
        busy |= ui->core->worker_pid[i] > 0;
    gtk_menu_item_set_label(GTK_MENU_ITEM(t->jobs),
                            busy ? "AI processing in background" : "No background jobs");
    bool has = ui->core->current_md_path[0] && g_file_test(ui->core->current_md_path, G_FILE_TEST_IS_REGULAR);
    gtk_widget_set_sensitive(t->recap, has && !s.menu_recording_enabled && !busy);
    char *spoken = g_build_filename(ui->core->current_session_dir, "recap.spoken.txt", NULL);
    gtk_widget_set_sensitive(t->speech, !s.menu_recording_enabled && !busy &&
                                            g_file_test(spoken, G_FILE_TEST_IS_REGULAR));
    g_free(spoken);
    /* Rebuild only when session count changes; avoid replacing an open menu every tick. */
    const char *latest = s.recent_files_count ? s.recent_files[s.recent_files_count - 1] : "";
    const char *old = g_object_get_data(G_OBJECT(t->recent), "latest");
    if (g_strcmp0(old, latest)) {
        GtkWidget *menu = gtk_menu_new();
        for (size_t i = 0; i < s.recent_files_count; i++) {
            char *dir = g_path_get_dirname(s.recent_files[i]);
            char *base = g_path_get_basename(dir);
            GtkWidget *w = gtk_menu_item_new_with_label(base);
            g_signal_connect_data(w, "activate", G_CALLBACK(recent_folder), dir, free_data, 0);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), w);
            g_free(base);
        }
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(t->recent), menu);
        gtk_widget_show_all(menu);
        g_object_set_data_full(G_OBJECT(t->recent), "latest", g_strdup(latest), g_free);
    }
    gtk_widget_set_sensitive(t->recent, s.recent_files_count > 0);
    t->updating = false;
}
void ascolto_tray_destroy(ascolto_tray_ui_t *ui) {
    Tray *t = ui->gtk_impl;
    if (t) {
        g_object_unref(t->indicator);
        gtk_widget_destroy(t->menu);
        g_object_unref(t->menu);
        g_free(t->assets); /* The indicator library owns registered GTypes and must remain loaded. */
        g_free(t);
    }
    pthread_mutex_destroy(&ui->mutex);
    ui->gtk_impl = NULL;
}

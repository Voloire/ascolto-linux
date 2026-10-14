#define _POSIX_C_SOURCE 200809L
#include "ascolto/tray_ui.h"
#include "ascolto/core.h"
#include <dlfcn.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib-object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef gboolean (*fn_gtk_init_check_t)(int *argc, char ***argv);
typedef gboolean (*fn_gtk_events_pending_t)(void);
typedef gboolean (*fn_gtk_main_iteration_do_t)(gboolean blocking);
typedef void *(*fn_gtk_menu_new_t)(void);
typedef void *(*fn_gtk_menu_item_new_with_label_t)(const char *label);
typedef void (*fn_gtk_menu_item_set_label_t)(void *item, const char *label);
typedef void (*fn_gtk_menu_item_set_submenu_t)(void *item, void *submenu);
typedef void *(*fn_gtk_check_menu_item_new_with_label_t)(const char *label);
typedef void (*fn_gtk_check_menu_item_set_active_t)(void *item, gboolean is_active);
typedef gboolean (*fn_gtk_check_menu_item_get_active_t)(void *item);
typedef void *(*fn_gtk_separator_menu_item_new_t)(void);
typedef void (*fn_gtk_menu_shell_append_t)(void *menu_shell, void *child);
typedef void (*fn_gtk_widget_show_all_t)(void *widget);
typedef void (*fn_gtk_widget_set_sensitive_t)(void *widget, gboolean sensitive);
typedef void (*fn_gtk_widget_destroy_t)(void *widget);

typedef void *(*fn_app_indicator_new_t)(const char *id, const char *icon_name, int category);
typedef void (*fn_app_indicator_set_status_t)(void *self, int status);
typedef void (*fn_app_indicator_set_icon_full_t)(void *self, const char *icon_name, const char *icon_desc);
typedef void (*fn_app_indicator_set_label_t)(void *self, const char *label, const char *guide);
typedef void (*fn_app_indicator_set_title_t)(void *self, const char *title);
typedef void (*fn_app_indicator_set_menu_t)(void *self, void *menu);

typedef struct {
    void *gtk_lib;
    void *ind_lib;
    fn_gtk_init_check_t gtk_init_check;
    fn_gtk_events_pending_t gtk_events_pending;
    fn_gtk_main_iteration_do_t gtk_main_iteration_do;
    fn_gtk_menu_new_t gtk_menu_new;
    fn_gtk_menu_item_new_with_label_t gtk_menu_item_new_with_label;
    fn_gtk_menu_item_set_label_t gtk_menu_item_set_label;
    fn_gtk_menu_item_set_submenu_t gtk_menu_item_set_submenu;
    fn_gtk_check_menu_item_new_with_label_t gtk_check_menu_item_new_with_label;
    fn_gtk_check_menu_item_set_active_t gtk_check_menu_item_set_active;
    fn_gtk_check_menu_item_get_active_t gtk_check_menu_item_get_active;
    fn_gtk_separator_menu_item_new_t gtk_separator_menu_item_new;
    fn_gtk_menu_shell_append_t gtk_menu_shell_append;
    fn_gtk_widget_show_all_t gtk_widget_show_all;
    fn_gtk_widget_set_sensitive_t gtk_widget_set_sensitive;
    fn_gtk_widget_destroy_t gtk_widget_destroy;

    fn_app_indicator_new_t app_indicator_new;
    fn_app_indicator_set_status_t app_indicator_set_status;
    fn_app_indicator_set_icon_full_t app_indicator_set_icon_full;
    fn_app_indicator_set_label_t app_indicator_set_label;
    fn_app_indicator_set_title_t app_indicator_set_title;
    fn_app_indicator_set_menu_t app_indicator_set_menu;

    void *indicator;
    void *menu;
    void *item_status;
    void *item_start_stop;
    void *item_pause_resume;
    void *item_auto_mic;
    void *item_keywords;
    void *item_recent;
    void *recent_items[5];
    char recent_paths[5][512];

    void *lang_items[8];
    const char *lang_codes[8];

    void *sens_items[3];
    const char *sens_codes[3];

    size_t mic_items_count;
    void *mic_items[ASCOLTO_MAX_DEVICES];
    char mic_names[ASCOLTO_MAX_DEVICES][128];

    size_t sys_items_count;
    void *sys_items[ASCOLTO_MAX_DEVICES];
    char sys_names[ASCOLTO_MAX_DEVICES][128];

    size_t built_recent_count;
    bool has_applied;
    ascolto_tray_snapshot_t applied;
    bool updating_widgets;
    volatile bool keywords_dialog_open;
} gtk_tray_priv_t;

static void ensure_dir_exists(const char *dir_path) {
    if (!dir_path || !*dir_path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void launch_path_nonblocking(const char *path, bool is_dir) {
    if (!path || !*path) return;
    if (is_dir) {
        ensure_dir_exists(path);
    }
    gchar *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) return;

    if (is_dir) {
        /* Use standard D-Bus org.freedesktop.FileManager1.ShowFolders so GNOME raises Nautilus on Wayland */
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (bus) {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
            g_variant_builder_add(&builder, "s", uri);
            g_dbus_connection_call(
                bus,
                "org.freedesktop.FileManager1",
                "/org/freedesktop/FileManager1",
                "org.freedesktop.FileManager1",
                "ShowFolders",
                g_variant_new("(ass)", &builder, ""),
                NULL,
                G_DBUS_CALL_FLAGS_NONE,
                3000,
                NULL,
                NULL,
                NULL
            );
            g_object_unref(bus);
            g_free(uri);
            return;
        }
    }

    g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
}

void ascolto_tray_open_transcripts_folder(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->core) return;
    launch_path_nonblocking(ui->core->config.output_dir, true);
}

static void apply_snapshot_to_gtk_main_thread(ascolto_tray_ui_t *ui);

static void on_menu_start_stop(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    if (ui->core->state == ASCOLTO_STATE_IDLE) {
        ascolto_core_start_manual(ui->core);
    } else {
        ascolto_core_stop(ui->core);
    }
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_pause_resume(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    if (ui->core->state == ASCOLTO_STATE_PAUSED) {
        ascolto_core_resume(ui->core);
    } else if (ui->core->state == ASCOLTO_STATE_RECORDING_AUTO ||
               ui->core->state == ASCOLTO_STATE_RECORDING_MANUAL) {
        ascolto_core_pause(ui->core);
    }
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_auto_mic_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    gboolean active = p->gtk_check_menu_item_get_active(widget);
    ascolto_core_set_auto_mic(ui->core, active ? true : false);
    apply_snapshot_to_gtk_main_thread(ui);
}

static void on_menu_set_lang(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    const char *lang = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-lang");
    if (lang) {
        ascolto_core_set_language(ui->core, lang);
        apply_snapshot_to_gtk_main_thread(ui);
    }
}

static void on_menu_set_mic(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    const char *dev = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-dev");
    if (dev) {
        ascolto_core_set_mic_device(ui->core, dev);
        apply_snapshot_to_gtk_main_thread(ui);
    }
}

static void on_menu_set_sys(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    const char *dev = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-dev");
    if (dev) {
        ascolto_core_set_sys_device(ui->core, dev);
        apply_snapshot_to_gtk_main_thread(ui);
    }
}

static void on_menu_set_sens(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    const char *sens_str = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-sens");
    if (sens_str) {
        ascolto_core_set_sensitivity(ui->core, ascolto_sensitivity_from_string(sens_str));
        apply_snapshot_to_gtk_main_thread(ui);
    }
}

static void *keywords_dialog_worker(void *arg) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)arg;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;

    char current_kw[1024] = {0};
    pthread_mutex_lock(&ui->core->mutex);
    snprintf(current_kw, sizeof(current_kw), "%s", ui->core->config.keywords);
    pthread_mutex_unlock(&ui->core->mutex);

    gchar *argv[] = {
        (gchar *)"zenity",
        (gchar *)"--entry",
        (gchar *)"--title=Ascolto - Keywords personalizzate",
        (gchar *)"--text=Inserisci parole chiave, nomi propri o acronimi (separati da virgola):",
        (gchar *)"--entry-text",
        (gchar *)current_kw,
        (gchar *)"--width=460",
        NULL
    };
    gchar *std_out = NULL;
    gint exit_status = 0;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &std_out, NULL, &exit_status, NULL) && exit_status == 0 && std_out) {
        size_t len = strlen(std_out);
        while (len > 0 && (std_out[len - 1] == '\n' || std_out[len - 1] == '\r')) {
            std_out[--len] = '\0';
        }
        ascolto_core_set_keywords(ui->core, std_out);
    }
    if (std_out) g_free(std_out);
    p->keywords_dialog_open = false;
    return NULL;
}

static void on_menu_keywords_dialog(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->keywords_dialog_open) return;
    p->keywords_dialog_open = true;
    pthread_t th;
    if (pthread_create(&th, NULL, keywords_dialog_worker, ui) == 0) {
        pthread_detach(th);
    } else {
        p->keywords_dialog_open = false;
    }
}

static void on_menu_open_recent_file(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    for (size_t i = 0; i < 5; i++) {
        if (p->recent_items[i] == widget && p->recent_paths[i][0] != '\0') {
            launch_path_nonblocking(p->recent_paths[i], false);
            break;
        }
    }
}

static void on_menu_open_folder(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    launch_path_nonblocking(ui->core->config.output_dir, true);
}

static void on_menu_quit(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core) {
        ui->core->running = false;
    }
}

static void rebuild_recent_submenu(ascolto_tray_ui_t *ui, gtk_tray_priv_t *p, const ascolto_tray_snapshot_t *s) {
    (void)ui;
    for (size_t slot = 0; slot < 5; slot++) {
        if (!p->recent_items[slot]) continue;
        if (slot < s->recent_files_count) {
            size_t src_idx = s->recent_files_count - 1 - slot;
            const char *full = s->recent_files[src_idx];
            const char *base = strrchr(full, '/');
            base = base ? (base + 1) : full;
            snprintf(p->recent_paths[slot], sizeof(p->recent_paths[slot]), "%s", full);
            p->gtk_menu_item_set_label(p->recent_items[slot], base);
            p->gtk_widget_set_sensitive(p->recent_items[slot], TRUE);
        } else {
            p->recent_paths[slot][0] = '\0';
            p->gtk_menu_item_set_label(p->recent_items[slot],
                                       (slot == 0 && s->recent_files_count == 0)
                                           ? "(Nessuna trascrizione recente)"
                                           : "-");
            p->gtk_widget_set_sensitive(p->recent_items[slot], FALSE);
        }
    }
    p->built_recent_count = s->recent_files_count;
}

static bool init_gtk_indicator(ascolto_tray_ui_t *ui) {
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)calloc(1, sizeof(gtk_tray_priv_t));
    if (!p) return false;

    p->gtk_lib = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
    p->ind_lib = dlopen("libayatana-appindicator3.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!p->gtk_lib || !p->ind_lib) {
        if (p->gtk_lib) dlclose(p->gtk_lib);
        if (p->ind_lib) dlclose(p->ind_lib);
        free(p);
        return false;
    }

    p->gtk_init_check = (fn_gtk_init_check_t)dlsym(p->gtk_lib, "gtk_init_check");
    p->gtk_events_pending = (fn_gtk_events_pending_t)dlsym(p->gtk_lib, "gtk_events_pending");
    p->gtk_main_iteration_do = (fn_gtk_main_iteration_do_t)dlsym(p->gtk_lib, "gtk_main_iteration_do");
    p->gtk_menu_new = (fn_gtk_menu_new_t)dlsym(p->gtk_lib, "gtk_menu_new");
    p->gtk_menu_item_new_with_label = (fn_gtk_menu_item_new_with_label_t)dlsym(p->gtk_lib, "gtk_menu_item_new_with_label");
    p->gtk_menu_item_set_label = (fn_gtk_menu_item_set_label_t)dlsym(p->gtk_lib, "gtk_menu_item_set_label");
    p->gtk_menu_item_set_submenu = (fn_gtk_menu_item_set_submenu_t)dlsym(p->gtk_lib, "gtk_menu_item_set_submenu");
    p->gtk_check_menu_item_new_with_label = (fn_gtk_check_menu_item_new_with_label_t)dlsym(p->gtk_lib, "gtk_check_menu_item_new_with_label");
    p->gtk_check_menu_item_set_active = (fn_gtk_check_menu_item_set_active_t)dlsym(p->gtk_lib, "gtk_check_menu_item_set_active");
    p->gtk_check_menu_item_get_active = (fn_gtk_check_menu_item_get_active_t)dlsym(p->gtk_lib, "gtk_check_menu_item_get_active");
    p->gtk_separator_menu_item_new = (fn_gtk_separator_menu_item_new_t)dlsym(p->gtk_lib, "gtk_separator_menu_item_new");
    p->gtk_menu_shell_append = (fn_gtk_menu_shell_append_t)dlsym(p->gtk_lib, "gtk_menu_shell_append");
    p->gtk_widget_show_all = (fn_gtk_widget_show_all_t)dlsym(p->gtk_lib, "gtk_widget_show_all");
    p->gtk_widget_set_sensitive = (fn_gtk_widget_set_sensitive_t)dlsym(p->gtk_lib, "gtk_widget_set_sensitive");
    p->gtk_widget_destroy = (fn_gtk_widget_destroy_t)dlsym(p->gtk_lib, "gtk_widget_destroy");

    p->app_indicator_new = (fn_app_indicator_new_t)dlsym(p->ind_lib, "app_indicator_new");
    p->app_indicator_set_status = (fn_app_indicator_set_status_t)dlsym(p->ind_lib, "app_indicator_set_status");
    p->app_indicator_set_icon_full = (fn_app_indicator_set_icon_full_t)dlsym(p->ind_lib, "app_indicator_set_icon_full");
    p->app_indicator_set_label = (fn_app_indicator_set_label_t)dlsym(p->ind_lib, "app_indicator_set_label");
    p->app_indicator_set_title = (fn_app_indicator_set_title_t)dlsym(p->ind_lib, "app_indicator_set_title");
    p->app_indicator_set_menu = (fn_app_indicator_set_menu_t)dlsym(p->ind_lib, "app_indicator_set_menu");

    if (!p->gtk_init_check || !p->app_indicator_new || !p->gtk_init_check(NULL, NULL)) {
        dlclose(p->ind_lib);
        dlclose(p->gtk_lib);
        free(p);
        return false;
    }

    p->indicator = p->app_indicator_new("ascolto-indicator", "audio-input-microphone-symbolic", 0);
    if (!p->indicator) {
        dlclose(p->ind_lib);
        dlclose(p->gtk_lib);
        free(p);
        return false;
    }
    p->app_indicator_set_status(p->indicator, 1);
    if (p->app_indicator_set_title) {
        p->app_indicator_set_title(p->indicator, "Ascolto Call Transcriber");
    }

    p->menu = p->gtk_menu_new();

    p->item_status = p->gtk_menu_item_new_with_label("Stato: In attesa del microfono");
    p->gtk_widget_set_sensitive(p->item_status, FALSE);
    p->gtk_menu_shell_append(p->menu, p->item_status);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    p->item_start_stop = p->gtk_menu_item_new_with_label("Avvia trascrizione manuale");
    g_signal_connect_data(p->item_start_stop, "activate", G_CALLBACK(on_menu_start_stop), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_start_stop);

    p->item_pause_resume = p->gtk_menu_item_new_with_label("Metti in pausa");
    p->gtk_widget_set_sensitive(p->item_pause_resume, FALSE);
    g_signal_connect_data(p->item_pause_resume, "activate", G_CALLBACK(on_menu_pause_resume), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_pause_resume);

    p->item_auto_mic = p->gtk_check_menu_item_new_with_label("Attivazione automatica col microfono");
    p->gtk_check_menu_item_set_active(p->item_auto_mic, ui->core->config.auto_mic_trigger ? TRUE : FALSE);
    g_signal_connect_data(p->item_auto_mic, "toggled", G_CALLBACK(on_menu_auto_mic_toggled), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_auto_mic);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    /* Language submenu with checkmarks */
    void *item_lang = p->gtk_menu_item_new_with_label("Lingua di trascrizione");
    void *sub_lang = p->gtk_menu_new();
    static const char *langs[8][2] = {
        {"auto", "Rilevamento automatico (Auto)"},
        {"it", "Italiano (it)"},
        {"en", "English (en)"},
        {"es", "Espanol (es)"},
        {"fr", "Francais (fr)"},
        {"de", "Deutsch (de)"},
        {"nl", "Nederlands (nl)"},
        {"pl", "Polski (pl)"}
    };
    for (size_t i = 0; i < 8; i++) {
        void *li = p->gtk_check_menu_item_new_with_label(langs[i][1]);
        p->lang_items[i] = li;
        p->lang_codes[i] = langs[i][0];
        g_object_set_data(G_OBJECT(li), "ascolto-lang", (gpointer)langs[i][0]);
        g_signal_connect_data(li, "toggled", G_CALLBACK(on_menu_set_lang), ui, NULL, 0);
        p->gtk_menu_shell_append(sub_lang, li);
    }
    p->gtk_menu_item_set_submenu(item_lang, sub_lang);
    p->gtk_menu_shell_append(p->menu, item_lang);

    /* Devices submenus (MIC & SYS) with checkmarks */
    ascolto_device_info_t devs[ASCOLTO_MAX_DEVICES];
    size_t dev_count = 0;
    ui->core->audio.list_devices(&ui->core->audio, devs, ASCOLTO_MAX_DEVICES, &dev_count);

    void *item_mic = p->gtk_menu_item_new_with_label("Dispositivo Microfono (MIC)");
    void *sub_mic = p->gtk_menu_new();
    void *item_sys = p->gtk_menu_item_new_with_label("Audio di Sistema (SYS)");
    void *sub_sys = p->gtk_menu_new();

    for (size_t i = 0; i < dev_count; i++) {
        void *di = p->gtk_check_menu_item_new_with_label(devs[i].description);
        g_object_set_data_full(G_OBJECT(di), "ascolto-dev", g_strdup(devs[i].name), g_free);
        if (devs[i].is_monitor) {
            size_t idx = p->sys_items_count++;
            p->sys_items[idx] = di;
            snprintf(p->sys_names[idx], sizeof(p->sys_names[idx]), "%s", devs[i].name);
            g_signal_connect_data(di, "toggled", G_CALLBACK(on_menu_set_sys), ui, NULL, 0);
            p->gtk_menu_shell_append(sub_sys, di);
        } else {
            size_t idx = p->mic_items_count++;
            p->mic_items[idx] = di;
            snprintf(p->mic_names[idx], sizeof(p->mic_names[idx]), "%s", devs[i].name);
            g_signal_connect_data(di, "toggled", G_CALLBACK(on_menu_set_mic), ui, NULL, 0);
            p->gtk_menu_shell_append(sub_mic, di);
        }
    }
    p->gtk_menu_item_set_submenu(item_mic, sub_mic);
    p->gtk_menu_shell_append(p->menu, item_mic);
    p->gtk_menu_item_set_submenu(item_sys, sub_sys);
    p->gtk_menu_shell_append(p->menu, item_sys);

    /* Speaker diarization sensitivity submenu with checkmarks */
    void *item_sens = p->gtk_menu_item_new_with_label("Sensibilita riconoscimento interlocutori");
    void *sub_sens = p->gtk_menu_new();
    static const char *sens_opts[3][2] = {
        {"low", "Bassa (meno speaker)"},
        {"medium", "Media (bilanciata)"},
        {"high", "Alta (distingue voci simili)"}
    };
    for (size_t i = 0; i < 3; i++) {
        void *si = p->gtk_check_menu_item_new_with_label(sens_opts[i][1]);
        p->sens_items[i] = si;
        p->sens_codes[i] = sens_opts[i][0];
        g_object_set_data(G_OBJECT(si), "ascolto-sens", (gpointer)sens_opts[i][0]);
        g_signal_connect_data(si, "toggled", G_CALLBACK(on_menu_set_sens), ui, NULL, 0);
        p->gtk_menu_shell_append(sub_sens, si);
    }
    p->gtk_menu_item_set_submenu(item_sens, sub_sens);
    p->gtk_menu_shell_append(p->menu, item_sens);

    /* Keywords dialog item */
    p->item_keywords = p->gtk_menu_item_new_with_label("Keywords personalizzate...");
    g_signal_connect_data(p->item_keywords, "activate", G_CALLBACK(on_menu_keywords_dialog), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_keywords);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    /* Recent transcripts submenu (5 pre-allocated slots updated in place) */
    p->item_recent = p->gtk_menu_item_new_with_label("Trascrizioni recenti");
    void *sub_recent = p->gtk_menu_new();
    for (size_t i = 0; i < 5; i++) {
        void *ri = p->gtk_menu_item_new_with_label("-");
        p->gtk_widget_set_sensitive(ri, FALSE);
        p->recent_items[i] = ri;
        p->recent_paths[i][0] = '\0';
        g_signal_connect_data(ri, "activate", G_CALLBACK(on_menu_open_recent_file), ui, NULL, 0);
        p->gtk_menu_shell_append(sub_recent, ri);
    }
    p->gtk_menu_item_set_submenu(p->item_recent, sub_recent);
    p->built_recent_count = (size_t)-1;
    p->gtk_menu_shell_append(p->menu, p->item_recent);

    /* Open transcripts folder */
    void *item_open = p->gtk_menu_item_new_with_label("Apri cartella trascrizioni");
    g_signal_connect_data(item_open, "activate", G_CALLBACK(on_menu_open_folder), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, item_open);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

    void *item_quit = p->gtk_menu_item_new_with_label("Esci");
    g_signal_connect_data(item_quit, "activate", G_CALLBACK(on_menu_quit), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, item_quit);

    p->gtk_widget_show_all(p->menu);
    p->app_indicator_set_menu(p->indicator, p->menu);

    ui->gtk_impl = p;
    return true;
}

static void apply_snapshot_to_gtk_main_thread(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->gtk_impl) return;
    pthread_mutex_lock(&ui->mutex);
    if (!ui->dirty) {
        pthread_mutex_unlock(&ui->mutex);
        return;
    }
    ascolto_tray_snapshot_t snap = ui->snapshot;
    ui->dirty = false;
    pthread_mutex_unlock(&ui->mutex);

    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    bool first = !p->has_applied;
    p->updating_widgets = true;

    if (first || strcmp(snap.icon_name, p->applied.icon_name) != 0) {
        p->app_indicator_set_icon_full(p->indicator, snap.icon_name, snap.status_text);
    }
    if (first || strcmp(snap.label, p->applied.label) != 0) {
        p->app_indicator_set_label(p->indicator, snap.label, "REC 99:99");
    }
    if (first || strcmp(snap.status_text, p->applied.status_text) != 0) {
        p->gtk_menu_item_set_label(p->item_status, snap.status_text);
    }
    if (first || snap.menu_recording_enabled != p->applied.menu_recording_enabled) {
        p->gtk_menu_item_set_label(p->item_start_stop,
                                   (!snap.menu_recording_enabled)
                                       ? "Avvia trascrizione manuale"
                                       : "Ferma trascrizione");
        p->gtk_widget_set_sensitive(p->item_pause_resume,
                                    snap.menu_recording_enabled ? TRUE : FALSE);
    }
    if (first || snap.menu_paused != p->applied.menu_paused ||
        snap.menu_recording_enabled != p->applied.menu_recording_enabled) {
        p->gtk_menu_item_set_label(p->item_pause_resume,
                                   snap.menu_paused
                                       ? "Riprendi trascrizione"
                                       : "Metti in pausa");
    }
    if (first || snap.menu_auto_mic_checked != p->applied.menu_auto_mic_checked) {
        p->gtk_check_menu_item_set_active(p->item_auto_mic,
                                          snap.menu_auto_mic_checked ? TRUE : FALSE);
    }
    if (first || strcmp(snap.selected_language, p->applied.selected_language) != 0) {
        for (size_t i = 0; i < 8; i++) {
            if (p->lang_items[i]) {
                p->gtk_check_menu_item_set_active(p->lang_items[i],
                                                  strcmp(snap.selected_language, p->lang_codes[i]) == 0 ? TRUE : FALSE);
            }
        }
    }
    if (first || snap.selected_sensitivity != p->applied.selected_sensitivity) {
        const char *sens_code = ascolto_sensitivity_to_string(snap.selected_sensitivity);
        for (size_t i = 0; i < 3; i++) {
            if (p->sens_items[i]) {
                p->gtk_check_menu_item_set_active(p->sens_items[i],
                                                  strcmp(sens_code, p->sens_codes[i]) == 0 ? TRUE : FALSE);
            }
        }
    }
    if (first || strcmp(snap.selected_mic, p->applied.selected_mic) != 0) {
        for (size_t i = 0; i < p->mic_items_count; i++) {
            p->gtk_check_menu_item_set_active(p->mic_items[i],
                                              strcmp(snap.selected_mic, p->mic_names[i]) == 0 ? TRUE : FALSE);
        }
    }
    if (first || strcmp(snap.selected_sys, p->applied.selected_sys) != 0) {
        for (size_t i = 0; i < p->sys_items_count; i++) {
            p->gtk_check_menu_item_set_active(p->sys_items[i],
                                              strcmp(snap.selected_sys, p->sys_names[i]) == 0 ? TRUE : FALSE);
        }
    }
    if (first || p->built_recent_count != snap.recent_files_count) {
        rebuild_recent_submenu(ui, p, &snap);
    }
    if (first || strcmp(snap.keywords_summary, p->applied.keywords_summary) != 0) {
        char kw_lbl[320];
        snprintf(kw_lbl, sizeof(kw_lbl), "Keywords personalizzate (%s)...", snap.keywords_summary);
        p->gtk_menu_item_set_label(p->item_keywords, kw_lbl);
    }

    p->applied = snap;
    p->has_applied = true;
    p->updating_widgets = false;

    int guard = 0;
    while (p->gtk_events_pending && p->gtk_events_pending() && guard++ < 64) {
        p->gtk_main_iteration_do(FALSE);
    }
}

int ascolto_tray_init(ascolto_tray_ui_t *ui, ascolto_core_t *core, bool headless) {
    if (!ui || !core) return -1;
    memset(ui, 0, sizeof(*ui));
    ui->core = core;
    ui->headless = headless;
    ui->running = true;
    pthread_mutex_init(&ui->mutex, NULL);

    bool gui_ok = false;
    if (!headless && (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))) {
        gui_ok = init_gtk_indicator(ui);
    }
    ui->snapshot.active = true;
    ui->snapshot.using_gui_indicator = gui_ok;
    ascolto_tray_update(ui);
    apply_snapshot_to_gtk_main_thread(ui);
    return 0;
}

void ascolto_tray_update(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->core) return;
    ascolto_core_t *core = ui->core;

    uint64_t elapsed = ascolto_core_elapsed_sec(core);
    unsigned int mins = (unsigned int)(elapsed / 60);
    unsigned int secs = (unsigned int)(elapsed % 60);

    pthread_mutex_lock(&ui->mutex);
    ascolto_tray_snapshot_t *s = &ui->snapshot;
    s->active = true;
    s->menu_auto_mic_checked = core->config.auto_mic_trigger;
    snprintf(s->selected_language, sizeof(s->selected_language), "%s", core->config.language);
    snprintf(s->selected_mic, sizeof(s->selected_mic), "%s", core->config.mic_device);
    snprintf(s->selected_sys, sizeof(s->selected_sys), "%s", core->config.sys_device);
    s->selected_sensitivity = core->config.diarization_sensitivity;
    snprintf(s->keywords_summary, sizeof(s->keywords_summary), "%.255s",
             core->config.keywords[0] ? core->config.keywords : "(nessuna)");

    s->recent_files_count = core->recent_files_count;
    for (size_t i = 0; i < core->recent_files_count && i < ASCOLTO_MAX_RECENT_FILES; i++) {
        snprintf(s->recent_files[i], sizeof(s->recent_files[i]), "%s", core->recent_files[i]);
    }

    if (core->state == ASCOLTO_STATE_IDLE) {
        snprintf(s->icon_name, sizeof(s->icon_name), "microphone-sensitivity-muted-symbolic");
        snprintf(s->label, sizeof(s->label), "Ascolto");
        snprintf(s->status_text, sizeof(s->status_text), "Stato: In attesa del microfono");
        s->menu_recording_enabled = false;
        s->menu_paused = false;
    } else if (core->state == ASCOLTO_STATE_PAUSED) {
        snprintf(s->icon_name, sizeof(s->icon_name), "media-playback-pause-symbolic");
        snprintf(s->label, sizeof(s->label), "PAUSA %02u:%02u", mins, secs);
        snprintf(s->status_text, sizeof(s->status_text), "Stato: In pausa (%02u:%02u)", mins, secs);
        s->menu_recording_enabled = true;
        s->menu_paused = true;
    } else {
        snprintf(s->icon_name, sizeof(s->icon_name), "audio-input-microphone-symbolic");
        snprintf(s->label, sizeof(s->label), "REC %02u:%02u", mins, secs);
        snprintf(s->status_text, sizeof(s->status_text),
                 "Stato: Trascrizione attiva (%02u:%02u) [%s]",
                 mins, secs,
                 core->state == ASCOLTO_STATE_RECORDING_AUTO ? "Auto-Mic" : "Manuale");
        s->menu_recording_enabled = true;
        s->menu_paused = false;
    }

    ui->dirty = true;
    pthread_mutex_unlock(&ui->mutex);
}

void ascolto_tray_get_snapshot(ascolto_tray_ui_t *ui, ascolto_tray_snapshot_t *out_snap) {
    if (!ui || !out_snap) return;
    pthread_mutex_lock(&ui->mutex);
    *out_snap = ui->snapshot;
    pthread_mutex_unlock(&ui->mutex);
}

void ascolto_tray_step_iteration(ascolto_tray_ui_t *ui) {
    if (!ui || !ui->gtk_impl) return;
    apply_snapshot_to_gtk_main_thread(ui);
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    int guard = 0;
    while (p->gtk_events_pending && p->gtk_events_pending() && guard++ < 64) {
        p->gtk_main_iteration_do(FALSE);
    }
    if (ui->dirty) {
        apply_snapshot_to_gtk_main_thread(ui);
    }
}

void ascolto_tray_destroy(ascolto_tray_ui_t *ui) {
    if (!ui) return;
    pthread_mutex_lock(&ui->mutex);
    ui->running = false;
    if (ui->gtk_impl) {
        gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
        if (p->ind_lib) dlclose(p->ind_lib);
        if (p->gtk_lib) dlclose(p->gtk_lib);
        free(p);
        ui->gtk_impl = NULL;
    }
    pthread_mutex_unlock(&ui->mutex);
    pthread_mutex_destroy(&ui->mutex);
}

#define _POSIX_C_SOURCE 200809L
#include "ascolto/tray_ui.h"
#include "ascolto/core.h"
#include <dlfcn.h>
#include <glib-object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Dynamic function pointers for GTK3 and Ayatana AppIndicator3 */
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

/* Dialog & Entry for custom keywords */
typedef void *(*fn_gtk_dialog_new_with_buttons_t)(const char *title, void *parent, int flags, const char *first_btn, ...);
typedef void *(*fn_gtk_dialog_get_content_area_t)(void *dialog);
typedef int (*fn_gtk_dialog_run_t)(void *dialog);
typedef void *(*fn_gtk_entry_new_t)(void);
typedef void (*fn_gtk_entry_set_text_t)(void *entry, const char *text);
typedef const char *(*fn_gtk_entry_get_text_t)(void *entry);
typedef void *(*fn_gtk_label_new_t)(const char *str);
typedef void (*fn_gtk_container_add_t)(void *container, void *widget);

/* AppIndicator3 */
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
    fn_gtk_dialog_new_with_buttons_t gtk_dialog_new_with_buttons;
    fn_gtk_dialog_get_content_area_t gtk_dialog_get_content_area;
    fn_gtk_dialog_run_t gtk_dialog_run;
    fn_gtk_entry_new_t gtk_entry_new;
    fn_gtk_entry_set_text_t gtk_entry_set_text;
    fn_gtk_entry_get_text_t gtk_entry_get_text;
    fn_gtk_label_new_t gtk_label_new;
    fn_gtk_container_add_t gtk_container_add;

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
    bool updating_widgets;
} gtk_tray_priv_t;

static void on_menu_start_stop(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    if (ui->core->state == ASCOLTO_STATE_IDLE) {
        ascolto_core_start_manual(ui->core);
    } else {
        ascolto_core_stop(ui->core);
    }
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
}

static void on_menu_auto_mic_toggled(void *widget, gpointer user_data) {
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (p->updating_widgets) return;
    gboolean active = p->gtk_check_menu_item_get_active(widget);
    ascolto_core_set_auto_mic(ui->core, active ? true : false);
}

static void on_menu_set_lang(void *widget, gpointer user_data) {
    (void)widget;
    const char *lang = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-lang");
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core && lang) {
        ascolto_core_set_language(ui->core, lang);
    }
}

static void on_menu_set_mic(void *widget, gpointer user_data) {
    const char *dev = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-dev");
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core && dev) {
        ascolto_core_set_mic_device(ui->core, dev);
    }
}

static void on_menu_set_sys(void *widget, gpointer user_data) {
    const char *dev = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-dev");
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core && dev) {
        ascolto_core_set_sys_device(ui->core, dev);
    }
}

static void on_menu_set_sens(void *widget, gpointer user_data) {
    const char *sens_str = (const char *)g_object_get_data(G_OBJECT(widget), "ascolto-sens");
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core && sens_str) {
        ascolto_core_set_sensitivity(ui->core, ascolto_sensitivity_from_string(sens_str));
    }
}

static void on_menu_keywords_dialog(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core || !ui->gtk_impl) return;
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    if (!p->gtk_dialog_new_with_buttons || !p->gtk_entry_new) return;

    /* GTK_RESPONSE_ACCEPT = -3, GTK_RESPONSE_CANCEL = -6 */
    void *dlg = p->gtk_dialog_new_with_buttons(
        "Ascolto - Keywords personalizzate",
        NULL, 1,
        "Annulla", -6,
        "Salva", -3,
        NULL
    );
    void *area = p->gtk_dialog_get_content_area(dlg);
    void *lbl = p->gtk_label_new("Inserisci parole chiave, nomi o acronimi (separati da virgola):");
    void *entry = p->gtk_entry_new();
    p->gtk_entry_set_text(entry, ui->core->config.keywords);
    p->gtk_container_add(area, lbl);
    p->gtk_container_add(area, entry);
    p->gtk_widget_show_all(dlg);

    int resp = p->gtk_dialog_run(dlg);
    if (resp == -3) {
        const char *txt = p->gtk_entry_get_text(entry);
        if (txt) {
            ascolto_core_set_keywords(ui->core, txt);
        }
    }
    p->gtk_widget_destroy(dlg);
}

static void on_menu_open_folder(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (!ui || !ui->core) return;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", ui->core->config.output_dir);
    int rc = system(cmd);
    (void)rc;
}

static void on_menu_quit(void *widget, gpointer user_data) {
    (void)widget;
    ascolto_tray_ui_t *ui = (ascolto_tray_ui_t *)user_data;
    if (ui && ui->core) {
        ui->core->running = false;
    }
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

    p->gtk_dialog_new_with_buttons = (fn_gtk_dialog_new_with_buttons_t)dlsym(p->gtk_lib, "gtk_dialog_new_with_buttons");
    p->gtk_dialog_get_content_area = (fn_gtk_dialog_get_content_area_t)dlsym(p->gtk_lib, "gtk_dialog_get_content_area");
    p->gtk_dialog_run = (fn_gtk_dialog_run_t)dlsym(p->gtk_lib, "gtk_dialog_run");
    p->gtk_entry_new = (fn_gtk_entry_new_t)dlsym(p->gtk_lib, "gtk_entry_new");
    p->gtk_entry_set_text = (fn_gtk_entry_set_text_t)dlsym(p->gtk_lib, "gtk_entry_set_text");
    p->gtk_entry_get_text = (fn_gtk_entry_get_text_t)dlsym(p->gtk_lib, "gtk_entry_get_text");
    p->gtk_label_new = (fn_gtk_label_new_t)dlsym(p->gtk_lib, "gtk_label_new");
    p->gtk_container_add = (fn_gtk_container_add_t)dlsym(p->gtk_lib, "gtk_container_add");

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

    /* APP_INDICATOR_CATEGORY_APPLICATION_STATUS = 0, APP_INDICATOR_STATUS_ACTIVE = 1 */
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

    /* Language submenu */
    void *item_lang = p->gtk_menu_item_new_with_label("Lingua di trascrizione");
    void *sub_lang = p->gtk_menu_new();
    const char *langs[][2] = {
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
        void *li = p->gtk_menu_item_new_with_label(langs[i][1]);
        g_object_set_data(G_OBJECT(li), "ascolto-lang", (gpointer)langs[i][0]);
        g_signal_connect_data(li, "activate", G_CALLBACK(on_menu_set_lang), ui, NULL, 0);
        p->gtk_menu_shell_append(sub_lang, li);
    }
    p->gtk_menu_item_set_submenu(item_lang, sub_lang);
    p->gtk_menu_shell_append(p->menu, item_lang);

    /* Devices submenus (MIC & SYS) */
    ascolto_device_info_t devs[ASCOLTO_MAX_DEVICES];
    size_t dev_count = 0;
    ui->core->audio.list_devices(&ui->core->audio, devs, ASCOLTO_MAX_DEVICES, &dev_count);

    void *item_mic = p->gtk_menu_item_new_with_label("Dispositivo Microfono (MIC)");
    void *sub_mic = p->gtk_menu_new();
    void *item_sys = p->gtk_menu_item_new_with_label("Audio di Sistema (SYS)");
    void *sub_sys = p->gtk_menu_new();

    for (size_t i = 0; i < dev_count; i++) {
        void *di = p->gtk_menu_item_new_with_label(devs[i].description);
        g_object_set_data_full(G_OBJECT(di), "ascolto-dev", g_strdup(devs[i].name), g_free);
        if (devs[i].is_monitor) {
            g_signal_connect_data(di, "activate", G_CALLBACK(on_menu_set_sys), ui, NULL, 0);
            p->gtk_menu_shell_append(sub_sys, di);
        } else {
            g_signal_connect_data(di, "activate", G_CALLBACK(on_menu_set_mic), ui, NULL, 0);
            p->gtk_menu_shell_append(sub_mic, di);
        }
    }
    p->gtk_menu_item_set_submenu(item_mic, sub_mic);
    p->gtk_menu_shell_append(p->menu, item_mic);
    p->gtk_menu_item_set_submenu(item_sys, sub_sys);
    p->gtk_menu_shell_append(p->menu, item_sys);

    /* Speaker diarization sensitivity submenu */
    void *item_sens = p->gtk_menu_item_new_with_label("Sensibilita riconoscimento interlocutori");
    void *sub_sens = p->gtk_menu_new();
    const char *sens_opts[][2] = {
        {"low", "Bassa (meno speaker)"},
        {"medium", "Media (bilanciata)"},
        {"high", "Alta (distingue voci simili)"}
    };
    for (size_t i = 0; i < 3; i++) {
        void *si = p->gtk_menu_item_new_with_label(sens_opts[i][1]);
        g_object_set_data(G_OBJECT(si), "ascolto-sens", (gpointer)sens_opts[i][0]);
        g_signal_connect_data(si, "activate", G_CALLBACK(on_menu_set_sens), ui, NULL, 0);
        p->gtk_menu_shell_append(sub_sens, si);
    }
    p->gtk_menu_item_set_submenu(item_sens, sub_sens);
    p->gtk_menu_shell_append(p->menu, item_sens);

    /* Keywords dialog item */
    p->item_keywords = p->gtk_menu_item_new_with_label("Keywords personalizzate...");
    g_signal_connect_data(p->item_keywords, "activate", G_CALLBACK(on_menu_keywords_dialog), ui, NULL, 0);
    p->gtk_menu_shell_append(p->menu, p->item_keywords);

    p->gtk_menu_shell_append(p->menu, p->gtk_separator_menu_item_new());

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

    if (ui->gtk_impl) {
        gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
        p->updating_widgets = true;
        p->app_indicator_set_icon_full(p->indicator, s->icon_name, s->status_text);
        p->app_indicator_set_label(p->indicator, s->label, "REC 99:99");
        p->gtk_menu_item_set_label(p->item_status, s->status_text);
        p->gtk_menu_item_set_label(p->item_start_stop,
                                   (core->state == ASCOLTO_STATE_IDLE)
                                       ? "Avvia trascrizione manuale"
                                       : "Ferma trascrizione");
        p->gtk_menu_item_set_label(p->item_pause_resume,
                                   (core->state == ASCOLTO_STATE_PAUSED)
                                       ? "Riprendi trascrizione"
                                       : "Metti in pausa");
        p->gtk_widget_set_sensitive(p->item_pause_resume,
                                    (core->state == ASCOLTO_STATE_IDLE) ? FALSE : TRUE);
        p->gtk_check_menu_item_set_active(p->item_auto_mic,
                                          core->config.auto_mic_trigger ? TRUE : FALSE);
        char kw_lbl[320];
        snprintf(kw_lbl, sizeof(kw_lbl), "Keywords personalizzate (%s)...", s->keywords_summary);
        p->gtk_menu_item_set_label(p->item_keywords, kw_lbl);
        p->updating_widgets = false;
    }
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
    gtk_tray_priv_t *p = (gtk_tray_priv_t *)ui->gtk_impl;
    int guard = 0;
    while (p->gtk_events_pending && p->gtk_events_pending() && guard++ < 32) {
        p->gtk_main_iteration_do(FALSE);
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

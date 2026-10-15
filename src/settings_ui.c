#define _POSIX_C_SOURCE 200809L
#include "ascolto/settings_ui.h"
#include "ascolto/settings.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GtkWidget *window, *stack, *notice, *apply_button, *revert_button, *status_label, *session_box,
    *job_box;
static GtkWidget *controls[64], *sensitivity;
static ascolto_core_t *owner;
static bool loading, dirty;
static GtkCssProvider *css;
static uint64_t refreshed;
static const char *pages[] = {"General",  "Transcription", "Recording", "Storage",
                              "OpenCode", "OpenRouter",    "OpenAI"};
static const char *descriptions[] = {
    "A quiet place for every conversation.",        "From live words to a useful recap.",
    "Two channels. One complete conversation.",     "Your conversations stay in your folders.",
    "Use your locally authenticated OpenCode CLI.", "Choose a recap model through OpenRouter.",
    "Cloud transcription and spoken recaps."};
static char *asset_path(const char *relative) {
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0)
        return g_strdup(relative);
    exe[n] = 0;
    char *dir = g_path_get_dirname(exe);
    char *p = g_build_filename(dir, "..", relative, NULL);
    g_free(dir);
    return p;
}
static void style(bool dark) {
    if (!css) {
        css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    char *text = g_strdup_printf(
        "window.ascolto {background:%s;color:%s;font-family:Ubuntu;font-size:14px;}"
        ".ascolto .sidebar{background:%s;padding:20px 12px;} .ascolto .sidebar row{padding:8px "
        "12px;border-radius:5px;}"
        ".ascolto .sidebar row:selected{background:%s;color:%s;}"
        ".ascolto .heading{font-size:26px;font-weight:600;} .ascolto .brand{font-size:21px;font-weight:600;}"
        ".ascolto .muted{color:%s;font-size:12px;} .ascolto "
        ".section{font-size:11px;font-weight:600;letter-spacing:1px;color:%s;}"
        ".ascolto .card{background:%s;border:1px solid %s;border-radius:8px;}"
        ".ascolto .field{padding:12px 16px;border-bottom:1px solid %s;}"
        ".ascolto entry,.ascolto combobox button{background:%s;color:%s;border:1px solid "
        "%s;box-shadow:none;min-height:26px;}"
        ".ascolto button{border-radius:5px;box-shadow:none;} .ascolto "
        "button.suggested-action{background:%s;color:#fff;border-color:%s;}"
        ".ascolto switch:checked{background:#557d68;} .ascolto .footer{padding:12px 22px;border-top:1px "
        "solid %s;}"
        ".ascolto .notice{color:%s;font-size:12px;}",
        dark ? "#18211e" : "#f1f4ef", dark ? "#e8efe9" : "#203c32", dark ? "#202c26" : "#e5ece2",
        dark ? "#365343" : "#d2e2d0", dark ? "#f1f6ef" : "#1d3b2d", dark ? "#acbcb1" : "#63786a",
        dark ? "#a1baa8" : "#6a8071", dark ? "#29362f" : "#ffffff", dark ? "#3b4c40" : "#dce4da",
        dark ? "#36473c" : "#ecf0e9", dark ? "#223027" : "#f5f7f3", dark ? "#e8efe9" : "#203c32",
        dark ? "#455c4b" : "#dce4da", "#214f3c", "#214f3c", dark ? "#36473c" : "#dce4da",
        dark ? "#dbbd7b" : "#82602f");
    char *extras =
        g_strdup_printf(".ascolto .sidebar list,.ascolto .sidebar row,.ascolto .sidebar label{color:%s;}"
                        ".ascolto .sidebar list{background:transparent;}"
                        ".ascolto button:not(.suggested-action){background:%s;color:%s;border-color:%s;}"
                        ".ascolto button:disabled{opacity:0.45;} .ascolto .card > label{margin:12px 16px;}"
                        ".ascolto switch:checked{border-color:#557d68;} .ascolto "
                        ".brand-image{background:#e5ece2;padding:3px;border-radius:6px;}",
                        dark ? "#e8efe9" : "#203c32", dark ? "#29362f" : "#ffffff",
                        dark ? "#e8efe9" : "#203c32", dark ? "#455c4b" : "#dce4da");
    char *complete = g_strconcat(text, extras, NULL);
    gtk_css_provider_load_from_data(css, complete, -1, NULL);
    g_free(complete);
    g_free(extras);
    g_free(text);
}
static void class(GtkWidget *w, const char *name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), name);
}
static GtkWidget *label(const char *s, const char *cls) {
    GtkWidget *w = gtk_label_new(s);
    gtk_label_set_xalign(GTK_LABEL(w), 0);
    if (cls)
        class(w, cls);
    return w;
}
static const char *value(GtkWidget *w) {
    if (GTK_IS_COMBO_BOX(w)) {
        const char *v = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w));
        return v ? v : "";
    }
    return gtk_entry_get_text(GTK_ENTRY(w));
}
static void changed(void) {
    if (loading)
        return;
    dirty = true;
    gtk_widget_set_sensitive(apply_button, TRUE);
    gtk_widget_set_sensitive(revert_button, TRUE);
    gtk_label_set_text(GTK_LABEL(notice), "Unsaved changes");
}
static void entry_changed(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    changed();
}
static void switch_changed(GObject *w, GParamSpec *p, gpointer data) {
    (void)w;
    (void)p;
    (void)data;
    changed();
}
static GtkWidget *combo(const char *items) {
    GtkWidget *w = gtk_combo_box_text_new();
    char **parts = g_strsplit(items, "|", -1);
    for (int i = 0; parts[i]; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w), parts[i], parts[i]);
    g_strfreev(parts);
    return w;
}
static void set_value(GtkWidget *w, const char *s) {
    if (GTK_IS_COMBO_BOX(w)) {
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(w), s)) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w), s, s);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(w), s);
        }
    } else
        gtk_entry_set_text(GTK_ENTRY(w), s);
}
static void load(void) {
    loading = true;
    pthread_mutex_lock(&owner->mutex);
    ascolto_config_t cfg = owner->has_pending_config ? owner->pending_config : owner->config;
    pthread_mutex_unlock(&owner->mutex);
    for (size_t i = 0; i < ascolto_field_count; i++) {
        const ascolto_field_t *f = &ascolto_fields[i];
        const char *v = (const char *)&cfg + f->offset;
        if (f->kind)
            gtk_switch_set_active(GTK_SWITCH(controls[i]), *(const bool *)v);
        else
            set_value(controls[i], v);
    }
    set_value(sensitivity, ascolto_sensitivity_to_string(cfg.diarization_sensitivity));
    gboolean dark = FALSE;
    g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", &dark, NULL);
    style(!strcmp(cfg.appearance, "dark") || (!strcmp(cfg.appearance, "system") && dark));
    loading = false;
    dirty = false;
    gtk_widget_set_sensitive(apply_button, FALSE);
    gtk_widget_set_sensitive(revert_button, FALSE);
    gtk_label_set_text(GTK_LABEL(notice), owner->has_pending_config
                                              ? "Saved. Changes take effect after this session."
                                              : "All changes saved");
}
static int autostart(const ascolto_config_t *cfg) {
    char *dir = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "ascolto-daemon.desktop", NULL);
    int rc = 0;
    if (cfg->launch_at_login) {
        char *script = asset_path("scripts/launch_ascolto.sh");
        char *content = g_strdup_printf("[Desktop Entry]\nType=Application\nName=Ascolto\nExec=\"%s\" "
                                        "daemon\nX-GNOME-Autostart-enabled=true\n",
                                        script);
        rc = g_file_set_contents(path, content, -1, NULL) ? 0 : -1;
        g_free(content);
        g_free(script);
    } else if (g_file_test(path, G_FILE_TEST_EXISTS))
        rc = unlink(path);
    g_free(path);
    g_free(dir);
    return rc;
}
int ascolto_settings_apply(ascolto_core_t *core, const char *json, char **error) {
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t next = core->has_pending_config ? core->pending_config : core->config;
    int rc = ascolto_settings_patch(&next, json, error);
    if (!rc && strcmp(next.weights_path, core->config.weights_path) &&
        !g_file_test(next.weights_path, G_FILE_TEST_IS_REGULAR)) {
        rc = -1;
        if (error)
            *error = g_strdup("Whistle model file does not exist");
    }
    if (!rc && next.launch_at_login != core->config.launch_at_login && autostart(&next)) {
        rc = -1;
        if (error)
            *error = g_strdup("Cannot update login startup");
    }
    if (!rc && ascolto_config_save(&next, NULL)) {
        rc = -1;
        if (error)
            *error = g_strdup("Cannot save settings");
    }
    if (!rc) {
        core->pending_config = next;
        core->has_pending_config = true;
    }
    pthread_mutex_unlock(&core->mutex);
    return rc;
}
static void apply(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    JsonObject *o = json_object_new();
    for (size_t i = 0; i < ascolto_field_count; i++) {
        const ascolto_field_t *f = &ascolto_fields[i];
        if (f->kind)
            json_object_set_boolean_member(o, f->key, gtk_switch_get_active(GTK_SWITCH(controls[i])));
        else
            json_object_set_string_member(o, f->key, value(controls[i]));
    }
    json_object_set_string_member(o, "diarization_sensitivity", value(sensitivity));
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, o);
    char *json = json_to_string(n, FALSE), *error = NULL;
    if (ascolto_settings_apply(owner, json, &error))
        gtk_label_set_text(GTK_LABEL(notice), error ? error : "Unable to save");
    else
        load();
    g_free(error);
    g_free(json);
    json_node_free(n);
}
static void revert(GtkWidget *w, gpointer d) {
    (void)w;
    (void)d;
    load();
}
static gboolean closing(GtkWidget *w, GdkEvent *event, gpointer data) {
    (void)event;
    (void)data;
    if (dirty) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                              GTK_BUTTONS_NONE, "Discard unsaved changes?");
        gtk_dialog_add_buttons(GTK_DIALOG(d), "Keep editing", GTK_RESPONSE_CANCEL, "Discard",
                               GTK_RESPONSE_ACCEPT, NULL);
        int r = gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (r != GTK_RESPONSE_ACCEPT)
            return TRUE;
        load();
    }
    gtk_widget_hide(w);
    return TRUE;
}
static void navigate(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    (void)list;
    (void)data;
    if (row)
        gtk_stack_set_visible_child_name(GTK_STACK(stack), pages[gtk_list_box_row_get_index(row)]);
}
static void free_closure(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}
static void open_dir(GtkWidget *w, gpointer data) {
    (void)w;
    char *uri = g_filename_to_uri(data, NULL, NULL);
    if (uri) {
        g_app_info_launch_default_for_uri(uri, NULL, NULL);
        g_free(uri);
    }
}
static void retry(GtkWidget *w, gpointer data) {
    (void)w;
    char command[64], reply[256];
    snprintf(command, sizeof(command), "RETRY_JOB %s", (const char *)data);
    ascolto_obs_handle_command(&owner->obs, command, reply, sizeof(reply));
    gtk_label_set_text(GTK_LABEL(notice), strstr(reply, "true")
                                              ? "Job restarted. See background jobs below."
                                              : "Cannot retry this job while it is active or unavailable.");
}
static void clear_box(GtkWidget *box) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(children);
}
static void lists(void) {
    for (int jobs = 0; jobs < 2; jobs++) {
        GtkWidget *box = jobs ? job_box : session_box;
        clear_box(box);
        char *json = ascolto_store_list(&owner->config, jobs);
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, json, -1, NULL)) {
            JsonArray *a = json_node_get_array(json_parser_get_root(p));
            if (!json_array_get_length(a))
                gtk_box_pack_start(
                    GTK_BOX(box),
                    label(jobs ? "No background jobs yet." : "Your recorded conversations will appear here.",
                          "muted"),
                    FALSE, FALSE, 12);
            for (guint i = 0; i < json_array_get_length(a); i++) {
                JsonObject *o = json_array_get_object_element(a, i);
                const char *dir = json_object_get_string_member(o, "dir");
                char *base = g_path_get_basename(dir);
                GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
                class(row, "field");
                GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
                gtk_box_pack_start(GTK_BOX(text), label(base, NULL), FALSE, FALSE, 0);
                const char *detail = json_object_get_string_member(o, jobs ? "state" : "artifacts");
                GtkWidget *sub = label(detail, "muted");
                gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
                gtk_box_pack_start(GTK_BOX(text), sub, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
                GtkWidget *b = gtk_button_new_with_label(jobs ? "Retry job" : "Open folder");
                gtk_widget_set_sensitive(b, !jobs || !strcmp(detail, "failed") ||
                                                !strcmp(detail, "interrupted"));
                g_signal_connect_data(b, "clicked", jobs ? G_CALLBACK(retry) : G_CALLBACK(open_dir),
                                      g_strdup(jobs ? json_object_get_string_member(o, "id") : dir),
                                      free_closure, 0);
                gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
                g_free(base);
            }
        }
        g_object_unref(p);
        g_free(json);
        gtk_widget_show_all(box);
    }
}
static void refreshed_devices(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    ascolto_device_info_t devices[ASCOLTO_MAX_DEVICES];
    size_t count = 0;
    owner->audio.list_devices(&owner->audio, devices, ASCOLTO_MAX_DEVICES, &count);
    loading = true;
    for (size_t i = 0; i < ascolto_field_count; i++) {
        const char *key = ascolto_fields[i].key;
        if (strcmp(key, "mic_device") && strcmp(key, "sys_device"))
            continue;
        char *selected = g_strdup(value(controls[i]));
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(controls[i]));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(controls[i]), "default", "System default");
        for (size_t j = 0; j < count; j++)
            if (devices[j].is_monitor == (!strcmp(key, "sys_device")))
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(controls[i]), devices[j].id,
                                          devices[j].description[0] ? devices[j].description
                                                                    : devices[j].name);
        set_value(controls[i], selected);
        g_free(selected);
    }
    loading = false;
}
static void check_done(GObject *source, GAsyncResult *result, gpointer data) {
    GtkWidget *out = data;
    char *stdout_text = NULL, *stderr_text = NULL;
    GError *error = NULL;
    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &stdout_text, &stderr_text, &error);
    if (window)
        gtk_label_set_text(GTK_LABEL(out), error         ? "Check failed"
                                           : stdout_text ? stdout_text
                                                         : "Check failed");
    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
    g_object_unref(out);
    g_object_unref(source);
}
static void check_provider(GtkWidget *w, gpointer data) {
    const char *provider = data;
    char *script = asset_path("scripts/provider_check.py");
    const char *ref = "", *exe = "opencode";
    for (size_t i = 0; i < ascolto_field_count; i++) {
        const char *key = ascolto_fields[i].key;
        if ((!strcmp(provider, "OpenAI") && !strcmp(key, "openai_credential")) ||
            (!strcmp(provider, "OpenRouter") && !strcmp(key, "openrouter_credential")))
            ref = value(controls[i]);
        if (!strcmp(key, "opencode_path"))
            exe = value(controls[i]);
    }
    GtkWidget *out = g_object_get_data(G_OBJECT(w), "result");
    gtk_label_set_text(GTK_LABEL(out), "Checking…");
    GError *error = NULL;
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
                                      "python3", script, provider, ref, exe, NULL);
    if (p)
        g_subprocess_communicate_utf8_async(p, NULL, NULL, check_done, g_object_ref(out));
    else {
        gtk_label_set_text(GTK_LABEL(out), "Unable to start provider check");
        g_clear_error(&error);
    }
    g_free(script);
}
void ascolto_settings_open(ascolto_core_t *core) {
    if (core->config.headless)
        return;
    if (window) {
        gtk_window_present(GTK_WINDOW(window));
        return;
    }
    if (!gtk_init_check(NULL, NULL))
        return;
    owner = core;
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Ascolto · Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 980, 740);
    gtk_widget_set_size_request(window, 800, 560);
    class(window, "ascolto");
    char *icon = asset_path("assets/ascolto-icon.svg");
    gtk_window_set_icon_from_file(GTK_WINDOW(window), icon, NULL);
    g_signal_connect(window, "delete-event", G_CALLBACK(closing), NULL);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0),
              *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), root);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    class(side, "sidebar");
    gtk_widget_set_size_request(side, 180, -1);
    gtk_box_pack_start(GTK_BOX(body), side, FALSE, FALSE, 0);
    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *im = gtk_image_new_from_file(icon);
    class(im, "brand-image");
    gtk_image_set_pixel_size(GTK_IMAGE(im), 32);
    GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale(icon, 36, 36, TRUE, NULL);
    if (pix) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(im), pix);
        g_object_unref(pix);
    }
    g_free(icon);
    gtk_box_pack_start(GTK_BOX(brand), im, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand), label("Ascolto", "brand"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side), brand, FALSE, FALSE, 12);
    GtkWidget *nav = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(nav), GTK_SELECTION_SINGLE);
    for (size_t i = 0; i < G_N_ELEMENTS(pages); i++) {
        GtkWidget *l = label(pages[i], NULL);
        gtk_container_add(GTK_CONTAINER(nav), l);
    }
    gtk_box_pack_start(GTK_BOX(side), nav, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(side), label("ON YOUR DEVICE\nMade for listening.", "muted"), FALSE, FALSE, 16);
    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_box_pack_start(GTK_BOX(body), stack, TRUE, TRUE, 0);
    for (size_t page = 0; page < G_N_ELEMENTS(pages); page++) {
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
        gtk_container_set_border_width(GTK_CONTAINER(content), 28);
        gtk_container_add(GTK_CONTAINER(scroll), content);
        gtk_stack_add_named(GTK_STACK(stack), scroll, pages[page]);
        gtk_box_pack_start(GTK_BOX(content), label(page >= 4 ? "AI PROVIDERS" : "PREFERENCES", "section"),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), label(pages[page], "heading"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), label(descriptions[page], "muted"), FALSE, FALSE, 0);
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        class(card, "card");
        gtk_box_pack_start(GTK_BOX(content), card, FALSE, FALSE, 0);
        for (size_t i = 0; i < ascolto_field_count; i++) {
            const ascolto_field_t *f = &ascolto_fields[i];
            if (strcmp(f->page, pages[page]))
                continue;
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
            class(row, "field");
            GtkWidget *name = label(f->label, NULL);
            gtk_label_set_line_wrap(GTK_LABEL(name), TRUE);
            gtk_widget_set_size_request(name, 170, -1);
            gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
            GtkWidget *w;
            if (f->kind)
                w = gtk_switch_new();
            else if (!strcmp(f->key, "appearance"))
                w = combo("system|light|dark");
            else if (!strcmp(f->key, "recap_provider"))
                w = combo("claude|opencode|openrouter");
            else if (strstr(f->key, "lang") || !strcmp(f->key, "language"))
                w = combo(!strcmp(f->key, "language") ? "auto|en|it|es|fr|de|nl|pl" : "en|it|es|fr|de|nl|pl");
            else if (!strcmp(f->key, "mic_device") || !strcmp(f->key, "sys_device"))
                w = combo("default");
            else {
                w = gtk_entry_new();
                gtk_entry_set_width_chars(GTK_ENTRY(w), 24);
                gtk_entry_set_max_length(GTK_ENTRY(w), f->size - 1);
            }
            gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
            gtk_box_pack_end(GTK_BOX(row), w, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
            controls[i] = w;
            g_object_set_data(G_OBJECT(w), "setting-key", (gpointer)f->key);
            atk_object_set_name(gtk_widget_get_accessible(w), f->label);
            if (f->kind)
                g_signal_connect(w, "notify::active", G_CALLBACK(switch_changed), NULL);
            else
                g_signal_connect(w, "changed", G_CALLBACK(entry_changed), NULL);
        }
        if (page == 0) {
            status_label = label("", "muted");
            gtk_box_pack_start(GTK_BOX(content), status_label, FALSE, FALSE, 0);
        }
        if (page == 1) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
            class(row, "field");
            gtk_box_pack_start(GTK_BOX(row), label("Speaker separation", NULL), TRUE, TRUE, 0);
            sensitivity = combo("low|medium|high");
            g_signal_connect(sensitivity, "changed", G_CALLBACK(entry_changed), NULL);
            gtk_box_pack_end(GTK_BOX(row), sensitivity, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content),
                               label("Recording and transcription changes apply to the next session.\nUse "
                                     "provider/model for OpenCode; a model ID for OpenRouter.",
                                     "muted"),
                               FALSE, FALSE, 0);
        }
        if (page == 2) {
            GtkWidget *b = gtk_button_new_with_label("Refresh audio devices");
            g_signal_connect(b, "clicked", G_CALLBACK(refreshed_devices), NULL);
            gtk_widget_set_halign(b, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(content), b, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content),
                               label("Cloud retranscription requires FLAC audio.\nAudio and live transcripts "
                                     "remain available after processing.",
                                     "muted"),
                               FALSE, FALSE, 0);
        }
        if (page == 3) {
            GtkWidget *db = label(core->config.database_path, "muted");
            gtk_label_set_ellipsize(GTK_LABEL(db), PANGO_ELLIPSIZE_MIDDLE);
            gtk_box_pack_start(GTK_BOX(content), label("DATABASE · SCHEMA 1", "section"), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content), db, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content), label("RECENT SESSIONS", "section"), FALSE, FALSE, 0);
            session_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            class(session_box, "card");
            gtk_box_pack_start(GTK_BOX(content), session_box, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content), label("BACKGROUND JOBS", "section"), FALSE, FALSE, 0);
            job_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            class(job_box, "card");
            gtk_box_pack_start(GTK_BOX(content), job_box, FALSE, FALSE, 0);
        }
        if (page >= 4) {
            GtkWidget *b = gtk_button_new_with_label(page == 4 ? "Check local readiness" : "Test connection");
            gtk_widget_set_halign(b, GTK_ALIGN_START);
            GtkWidget *out =
                label(page == 4 ? "Uses existing local authentication. No automatic sign-in."
                                : "Use env:VARIABLE or op://vault/item/field. Keys are never stored.",
                      "muted");
            gtk_label_set_line_wrap(GTK_LABEL(out), TRUE);
            g_object_set_data(G_OBJECT(b), "result", out);
            g_signal_connect(b, "clicked", G_CALLBACK(check_provider), (gpointer)pages[page]);
            gtk_box_pack_start(GTK_BOX(content), b, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content), out, FALSE, FALSE, 0);
        }
    }
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    class(footer, "footer");
    notice = label("All changes saved", "notice");
    gtk_label_set_line_wrap(GTK_LABEL(notice), TRUE);
    gtk_box_pack_start(GTK_BOX(footer), notice, TRUE, TRUE, 0);
    revert_button = gtk_button_new_with_label("Revert");
    apply_button = gtk_button_new_with_label("Apply changes");
    class(apply_button, "suggested-action");
    gtk_box_pack_end(GTK_BOX(footer), apply_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(footer), revert_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(root), footer, FALSE, FALSE, 0);
    g_signal_connect(apply_button, "clicked", G_CALLBACK(apply), NULL);
    g_signal_connect(revert_button, "clicked", G_CALLBACK(revert), NULL);
    g_signal_connect(nav, "row-selected", G_CALLBACK(navigate), NULL);
    load();
    refreshed_devices(NULL, NULL);
    lists();
    gtk_list_box_select_row(GTK_LIST_BOX(nav), gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), 0));
    gtk_widget_show_all(window);
}
void ascolto_settings_tick(ascolto_core_t *core) {
    pthread_mutex_lock(&core->mutex);
    bool show = core->show_settings;
    core->show_settings = false;
    if (core->has_pending_config && core->state == ASCOLTO_STATE_IDLE && !core->session_closing) {
        bool reload = strcmp(core->config.weights_path, core->pending_config.weights_path) != 0;
        core->config = core->pending_config;
        core->has_pending_config = false;
        if (reload && !core->config.mock_engine) {
            ascolto_whistle_destroy(&core->whistle);
            ascolto_whistle_init(&core->whistle, core->config.weights_path);
        }
        ascolto_diarizer_set_sensitivity(&core->diarizer, core->config.diarization_sensitivity);
    }
    pthread_mutex_unlock(&core->mutex);
    if (show)
        ascolto_settings_open(core);
    if (window && gtk_widget_get_visible(window) && ascolto_monotonic_ms() - refreshed > 3000) {
        char text[256];
        snprintf(text, sizeof(text), "%s · %s\nWhistle: %s · Live audio stays on this device",
                 ascolto_state_to_string(core->state),
                 core->has_pending_config ? "Changes pending" : "Settings up to date",
                 core->whistle.model_loaded ? "ready" : "model unavailable");
        gtk_label_set_text(GTK_LABEL(status_label), text);
        lists();
        refreshed = ascolto_monotonic_ms();
    }
    if (!core->config.headless)
        while (gtk_events_pending())
            gtk_main_iteration_do(FALSE);
}
void ascolto_settings_close(void) {
    if (window) {
        gtk_widget_destroy(window);
        window = NULL;
    }
    if (css) {
        g_object_unref(css);
        css = NULL;
    }
}

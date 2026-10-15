#define _POSIX_C_SOURCE 200809L
#include "ascolto/settings.h"
#include "ascolto/settings_ui.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
static GtkWidget *find_stack(GtkWidget *w) {
    if (GTK_IS_STACK(w))
        return w;
    if (GTK_IS_CONTAINER(w)) {
        GList *items = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = items; l; l = l->next) {
            GtkWidget *s = find_stack(l->data);
            if (s) {
                g_list_free(items);
                return s;
            }
        }
        g_list_free(items);
    }
    return NULL;
}
static GtkWidget *find_control(GtkWidget *w, const char *key) {
    const char *setting = g_object_get_data(G_OBJECT(w), "setting-key");
    if (setting && !strcmp(setting, key))
        return w;
    if (GTK_IS_BUTTON(w) && gtk_button_get_label(GTK_BUTTON(w)) &&
        !strcmp(gtk_button_get_label(GTK_BUTTON(w)), key))
        return w;
    if (GTK_IS_LIST_BOX(w) && !strcmp(key, "navigation"))
        return w;
    if (GTK_IS_CONTAINER(w)) {
        GList *items = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = items; l; l = l->next) {
            GtkWidget *found = find_control(l->data, key);
            if (found) {
                g_list_free(items);
                return found;
            }
        }
        g_list_free(items);
    }
    return NULL;
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    ascolto_core_t *c = g_new0(ascolto_core_t, 1);
    ascolto_config_init_defaults(&c->config);
    char *dir = g_dir_make_tmp("ascolto-ui-XXXXXX", NULL);
    snprintf(c->config.database_path, sizeof(c->config.database_path), "%s/db", dir);
    snprintf(c->config.output_dir, sizeof(c->config.output_dir), "%s/calls", dir);
    pthread_mutex_init(&c->mutex, NULL);
    ascolto_audio_backend_create(&c->audio, true, NULL, NULL, NULL);
    g_assert_cmpint(ascolto_store_init(&c->config, false), ==, 0);
    ascolto_tray_init(&c->tray, c, false);
    ascolto_tray_snapshot_t tray;
    ascolto_tray_get_snapshot(&c->tray, &tray);
    g_assert_true(tray.using_gui_indicator);
    const char *pages[] = {"General",  "Transcription", "Recording", "Storage",
                           "OpenCode", "OpenRouter",    "OpenAI"};
    g_mkdir_with_parents("/tmp/ascolto-ui", 0700);
    for (int dark = 0; dark < 2; dark++) {
        strcpy(c->config.appearance, dark ? "dark" : "light");
        ascolto_settings_open(c);
        GList *windows = gtk_window_list_toplevels();
        GtkWidget *w = NULL, *stack = NULL;
        for (GList *l = windows; l; l = l->next) {
            stack = find_stack(l->data);
            if (stack) {
                w = l->data;
                break;
            }
        }
        g_assert_nonnull(stack);
        ascolto_settings_tick(c);
        gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
        for (size_t i = 0; i < G_N_ELEMENTS(pages); i++) {
            GtkWidget *nav = find_control(w, "navigation");
            g_assert_nonnull(nav);
            gtk_list_box_select_row(GTK_LIST_BOX(nav), gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), i));
            for (int j = 0; j < 12; j++) {
                while (gtk_events_pending())
                    gtk_main_iteration();
                g_usleep(20000);
            }
            GdkWindow *gw = gtk_widget_get_window(w);
            GdkPixbuf *p =
                gdk_pixbuf_get_from_window(gw, 0, 0, gdk_window_get_width(gw), gdk_window_get_height(gw));
            g_assert_nonnull(p);
            char path[256];
            snprintf(path, sizeof(path), "/tmp/ascolto-ui/%s-%s.png", dark ? "dark" : "light", pages[i]);
            g_assert_true(gdk_pixbuf_save(p, path, "png", NULL, NULL));
            g_object_unref(p);
        }
        GtkWidget *entry = find_control(w, "keywords"), *apply = find_control(w, "Apply changes"),
                  *revert = find_control(w, "Revert");
        g_assert_nonnull(entry);
        g_assert_nonnull(apply);
        g_assert_nonnull(revert);
        gtk_entry_set_text(GTK_ENTRY(entry), "Discard this edit");
        g_assert_true(gtk_widget_get_sensitive(apply));
        gtk_button_clicked(GTK_BUTTON(revert));
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(entry)), ==, c->config.keywords);
        gtk_entry_set_text(GTK_ENTRY(entry), "Keep this vocabulary");
        gtk_button_clicked(GTK_BUTTON(apply));
        g_assert_true(c->has_pending_config);
        ascolto_settings_tick(c);
        g_assert_cmpstr(c->config.keywords, ==, "Keep this vocabulary");
        gtk_window_close(GTK_WINDOW(w));
        while (gtk_events_pending())
            gtk_main_iteration();
        g_assert_false(gtk_widget_get_visible(w));
        ascolto_settings_open(c);
        g_assert_true(gtk_widget_get_visible(w));
        g_list_free(windows);
        ascolto_settings_close();
    }
    ascolto_tray_destroy(&c->tray);
    c->audio.destroy(&c->audio);
    pthread_mutex_destroy(&c->mutex);
    g_free(c);
    g_free(dir);
    puts("Rendered all seven pages in light and dark themes.");
    return 0;
}

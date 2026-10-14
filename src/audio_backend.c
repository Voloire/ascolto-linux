#define _POSIX_C_SOURCE 200809L
#include "ascolto/audio_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    ascolto_audio_backend_t *parent;
    ascolto_channel_t channel;
    char target_dev[128];
    bool is_sys_monitor;
    bool running;
    pid_t child_pid;
    int stdout_fd;
    pthread_t thread;
} pw_stream_worker_t;

typedef struct {
    ascolto_audio_backend_t *parent;
    bool watcher_running;
    pthread_t watcher_thread;
    bool last_mic_state;
    char last_app_name[128];

    bool capture_running;
    pw_stream_worker_t mic_worker;
    pw_stream_worker_t sys_worker;
} pw_backend_impl_t;

static char *read_command_output(const char *cmd, size_t *out_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 65536;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        pclose(fp);
        return NULL;
    }
    while (!feof(fp)) {
        if (len + 16384 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                pclose(fp);
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, 16384, fp);
        if (n == 0) break;
        len += n;
    }
    buf[len] = '\0';
    pclose(fp);
    if (out_len) *out_len = len;
    return buf;
}

bool ascolto_pipewire_check_mic_active(char *out_app_name, size_t app_name_cap) {
    if (out_app_name && app_name_cap > 0) out_app_name[0] = '\0';

    size_t json_len = 0;
    char *json_str = read_command_output("pw-dump 2>/dev/null", &json_len);
    if (!json_str || json_len == 0) {
        free(json_str);
        return false;
    }

    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, json_str, (gssize)json_len, &err)) {
        if (err) g_error_free(err);
        g_object_unref(parser);
        free(json_str);
        return false;
    }

    bool active_found = false;
    JsonNode *root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *arr = json_node_get_array(root);
        guint len = json_array_get_length(arr);
        for (guint i = 0; i < len; i++) {
            JsonObject *obj = json_array_get_object_element(arr, i);
            if (!obj || !json_object_has_member(obj, "info")) continue;
            JsonObject *info = json_object_get_object_member(obj, "info");
            if (!info || !json_object_has_member(info, "props")) continue;

            const char *state = json_object_get_string_member_with_default(info, "state", "");
            JsonObject *props = json_object_get_object_member(info, "props");
            if (!props) continue;

            const char *media_class = json_object_get_string_member_with_default(props, "media.class", "");
            if (strcmp(media_class, "Stream/Input/Audio") != 0) continue;

            const char *app_name = json_object_get_string_member_with_default(props, "application.name", "");
            const char *node_name = json_object_get_string_member_with_default(props, "node.name", "");

            /* Ignore ascolto's own streams and desktop volume meters */
            if (strstr(app_name, "ascolto") != NULL || strstr(node_name, "ascolto") != NULL) continue;
            if (strstr(app_name, "Volume Control") != NULL || strstr(app_name, "gnome-control-center") != NULL) continue;

            /* Ignore streams that explicitly capture a sink monitor rather than a microphone */
            if (json_object_has_member(props, "stream.capture.sink")) {
                JsonNode *cs_node = json_object_get_member(props, "stream.capture.sink");
                if (cs_node && JSON_NODE_HOLDS_VALUE(cs_node)) {
                    if (json_node_get_value_type(cs_node) == G_TYPE_BOOLEAN && json_node_get_boolean(cs_node)) {
                        continue;
                    }
                    const char *cs_str = json_object_get_string_member_with_default(props, "stream.capture.sink", "");
                    if (strcmp(cs_str, "true") == 0) continue;
                }
            }

            if (strcmp(state, "running") == 0) {
                active_found = true;
                if (out_app_name && app_name_cap > 0) {
                    snprintf(out_app_name, app_name_cap, "%s", (*app_name) ? app_name : ((*node_name) ? node_name : "CallApp"));
                }
                break;
            }
        }
    }

    g_object_unref(parser);
    free(json_str);
    return active_found;
}

int ascolto_pipewire_enumerate_devices(ascolto_device_info_t *out_devs, size_t max_devs, size_t *out_count) {
    if (!out_devs || max_devs == 0 || !out_count) return -1;
    *out_count = 0;

    /* Always add default MIC and default SYS monitor first */
    if (*out_count < max_devs) {
        ascolto_device_info_t *d = &out_devs[(*out_count)++];
        memset(d, 0, sizeof(*d));
        snprintf(d->id, sizeof(d->id), "default");
        snprintf(d->name, sizeof(d->name), "default");
        snprintf(d->description, sizeof(d->description), "Predefinito di sistema (MIC)");
        d->is_monitor = false;
        d->is_default = true;
    }
    if (*out_count < max_devs) {
        ascolto_device_info_t *d = &out_devs[(*out_count)++];
        memset(d, 0, sizeof(*d));
        snprintf(d->id, sizeof(d->id), "default");
        snprintf(d->name, sizeof(d->name), "default");
        snprintf(d->description, sizeof(d->description), "Predefinito di sistema (SYS Monitor)");
        d->is_monitor = true;
        d->is_default = true;
    }

    size_t json_len = 0;
    char *json_str = read_command_output("pw-dump 2>/dev/null", &json_len);
    if (!json_str || json_len == 0) {
        free(json_str);
        return 0;
    }

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, json_str, (gssize)json_len, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *arr = json_node_get_array(root);
            guint len = json_array_get_length(arr);
            for (guint i = 0; i < len && *out_count < max_devs; i++) {
                JsonObject *obj = json_array_get_object_element(arr, i);
                if (!obj || !json_object_has_member(obj, "info")) continue;
                gint64 id_num = json_object_get_int_member_with_default(obj, "id", -1);
                JsonObject *info = json_object_get_object_member(obj, "info");
                if (!info || !json_object_has_member(info, "props")) continue;
                JsonObject *props = json_object_get_object_member(info, "props");
                if (!props) continue;

                const char *media_class = json_object_get_string_member_with_default(props, "media.class", "");
                const char *node_name = json_object_get_string_member_with_default(props, "node.name", "");
                const char *node_desc = json_object_get_string_member_with_default(props, "node.description", node_name);

                if (strcmp(media_class, "Audio/Source") == 0) {
                    ascolto_device_info_t *d = &out_devs[(*out_count)++];
                    memset(d, 0, sizeof(*d));
                    snprintf(d->id, sizeof(d->id), "%lld", (long long)id_num);
                    snprintf(d->name, sizeof(d->name), "%s", node_name);
                    snprintf(d->description, sizeof(d->description), "%s", node_desc);
                    d->is_monitor = false;
                    d->is_default = false;
                } else if (strcmp(media_class, "Audio/Sink") == 0) {
                    ascolto_device_info_t *d = &out_devs[(*out_count)++];
                    memset(d, 0, sizeof(*d));
                    snprintf(d->id, sizeof(d->id), "%lld", (long long)id_num);
                    snprintf(d->name, sizeof(d->name), "%s", node_name);
                    snprintf(d->description, sizeof(d->description), "%s (Monitor)", node_desc);
                    d->is_monitor = true;
                    d->is_default = false;
                }
            }
        }
    }
    g_object_unref(parser);
    free(json_str);
    return 0;
}

static void *pw_watcher_loop(void *arg) {
    pw_backend_impl_t *impl = (pw_backend_impl_t *)arg;
    while (impl->watcher_running) {
        char app[128] = {0};
        bool active = ascolto_pipewire_check_mic_active(app, sizeof(app));
        if (active != impl->last_mic_state || (active && strcmp(app, impl->last_app_name) != 0)) {
            impl->last_mic_state = active;
            snprintf(impl->last_app_name, sizeof(impl->last_app_name), "%s", app);
            if (impl->parent->on_mic_trigger) {
                impl->parent->on_mic_trigger(impl->parent->cb_user_data, active, app);
            }
        }
        /* Sleep 250 ms between PipeWire graph inspections */
        for (int i = 0; i < 5 && impl->watcher_running; i++) {
            struct timespec ts = {0, 50 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void *pw_capture_worker_loop(void *arg) {
    pw_stream_worker_t *w = (pw_stream_worker_t *)arg;
    const size_t chunk_samples = 3200; /* 200 ms at 16 kHz */
    float buf[3200];
    size_t bytes_needed = chunk_samples * sizeof(float);
    double stream_sec = 0.0;

    while (w->running && w->stdout_fd >= 0) {
        size_t got = 0;
        unsigned char *ptr = (unsigned char *)buf;
        while (w->running && got < bytes_needed) {
            ssize_t r = read(w->stdout_fd, ptr + got, bytes_needed - got);
            if (r > 0) {
                got += (size_t)r;
            } else if (r == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                break;
            }
        }
        if (got >= sizeof(float) && w->parent->on_chunk) {
            size_t samples = got / sizeof(float);
            w->parent->on_chunk(w->parent->cb_user_data, w->channel, buf, samples, stream_sec, NULL);
            stream_sec += (double)samples / (double)ASCOLTO_SAMPLE_RATE;
        }
        if (got == 0) break;
    }
    return NULL;
}

static int start_pw_stream_worker(pw_stream_worker_t *w, ascolto_audio_backend_t *parent,
                                  ascolto_channel_t channel, const char *target_dev, bool is_sys_monitor) {
    memset(w, 0, sizeof(*w));
    w->parent = parent;
    w->channel = channel;
    w->is_sys_monitor = is_sys_monitor;
    w->child_pid = -1;
    w->stdout_fd = -1;
    snprintf(w->target_dev, sizeof(w->target_dev), "%s", (target_dev && *target_dev) ? target_dev : "default");

    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char props[256];
        if (is_sys_monitor) {
            snprintf(props, sizeof(props),
                     "{ application.name = \"ascolto-capture-sys\" node.name = \"ascolto-sys\" stream.capture.sink = true }");
        } else {
            snprintf(props, sizeof(props),
                     "{ application.name = \"ascolto-capture-mic\" node.name = \"ascolto-mic\" }");
        }

        if (strcmp(w->target_dev, "default") != 0 && w->target_dev[0] != '\0') {
            execlp("pw-record", "pw-record",
                   "--format=f32", "--rate=16000", "--channels=1",
                   "--properties", props,
                   "--target", w->target_dev,
                   "-", (char *)NULL);
        } else {
            execlp("pw-record", "pw-record",
                   "--format=f32", "--rate=16000", "--channels=1",
                   "--properties", props,
                   "-", (char *)NULL);
        }
        _exit(127);
    }

    close(pipefd[1]);
    w->child_pid = pid;
    w->stdout_fd = pipefd[0];
    w->running = true;
    pthread_create(&w->thread, NULL, pw_capture_worker_loop, w);
    return 0;
}

static void stop_pw_stream_worker(pw_stream_worker_t *w) {
    if (!w) return;
    w->running = false;
    if (w->child_pid > 0) {
        kill(w->child_pid, SIGTERM);
    }
    if (w->stdout_fd >= 0) {
        close(w->stdout_fd);
        w->stdout_fd = -1;
    }
    if (w->child_pid > 0) {
        pthread_join(w->thread, NULL);
        int status = 0;
        waitpid(w->child_pid, &status, 0);
        w->child_pid = -1;
    }
}

static int backend_start_watcher(ascolto_audio_backend_t *b) {
    if (!b || !b->impl) return -1;
    pw_backend_impl_t *impl = (pw_backend_impl_t *)b->impl;
    if (b->use_mock) {
        impl->watcher_running = true;
        return 0;
    }
    if (impl->watcher_running) return 0;
    impl->watcher_running = true;
    pthread_create(&impl->watcher_thread, NULL, pw_watcher_loop, impl);
    return 0;
}

static void backend_stop_watcher(ascolto_audio_backend_t *b) {
    if (!b || !b->impl) return;
    pw_backend_impl_t *impl = (pw_backend_impl_t *)b->impl;
    if (!impl->watcher_running) return;
    impl->watcher_running = false;
    if (!b->use_mock) {
        pthread_join(impl->watcher_thread, NULL);
    }
}

static int backend_start_capture(ascolto_audio_backend_t *b, const char *mic_dev, const char *sys_dev) {
    if (!b || !b->impl) return -1;
    pw_backend_impl_t *impl = (pw_backend_impl_t *)b->impl;
    if (impl->capture_running) return 0;
    impl->capture_running = true;
    if (b->use_mock) {
        return 0;
    }
    start_pw_stream_worker(&impl->mic_worker, b, ASCOLTO_CHANNEL_MIC, mic_dev, false);
    start_pw_stream_worker(&impl->sys_worker, b, ASCOLTO_CHANNEL_SYS, sys_dev, true);
    return 0;
}

static void backend_stop_capture(ascolto_audio_backend_t *b) {
    if (!b || !b->impl) return;
    pw_backend_impl_t *impl = (pw_backend_impl_t *)b->impl;
    if (!impl->capture_running) return;
    impl->capture_running = false;
    if (!b->use_mock) {
        stop_pw_stream_worker(&impl->mic_worker);
        stop_pw_stream_worker(&impl->sys_worker);
    }
}

static int backend_list_devices(ascolto_audio_backend_t *b, ascolto_device_info_t *out_devs, size_t max_devs, size_t *out_count) {
    if (!b || !out_devs || !out_count) return -1;
    if (b->use_mock) {
        *out_count = 0;
        if (*out_count < max_devs) {
            ascolto_device_info_t *d = &out_devs[(*out_count)++];
            memset(d, 0, sizeof(*d));
            snprintf(d->id, sizeof(d->id), "default");
            snprintf(d->name, sizeof(d->name), "mock_mic_default");
            snprintf(d->description, sizeof(d->description), "Mock Built-in Microphone");
            d->is_monitor = false;
            d->is_default = true;
        }
        if (*out_count < max_devs) {
            ascolto_device_info_t *d = &out_devs[(*out_count)++];
            memset(d, 0, sizeof(*d));
            snprintf(d->id, sizeof(d->id), "usb_headset_mic");
            snprintf(d->name, sizeof(d->name), "usb_headset_mic");
            snprintf(d->description, sizeof(d->description), "Mock USB Headset Microphone");
            d->is_monitor = false;
            d->is_default = false;
        }
        if (*out_count < max_devs) {
            ascolto_device_info_t *d = &out_devs[(*out_count)++];
            memset(d, 0, sizeof(*d));
            snprintf(d->id, sizeof(d->id), "default");
            snprintf(d->name, sizeof(d->name), "mock_sys_monitor");
            snprintf(d->description, sizeof(d->description), "Mock System Output Monitor");
            d->is_monitor = true;
            d->is_default = true;
        }
        if (*out_count < max_devs) {
            ascolto_device_info_t *d = &out_devs[(*out_count)++];
            memset(d, 0, sizeof(*d));
            snprintf(d->id, sizeof(d->id), "hdmi_monitor");
            snprintf(d->name, sizeof(d->name), "hdmi_monitor");
            snprintf(d->description, sizeof(d->description), "Mock HDMI Audio Monitor");
            d->is_monitor = true;
            d->is_default = false;
        }
        return 0;
    }
    return ascolto_pipewire_enumerate_devices(out_devs, max_devs, out_count);
}

static void backend_destroy(ascolto_audio_backend_t *b) {
    if (!b || !b->impl) return;
    backend_stop_capture(b);
    backend_stop_watcher(b);
    free(b->impl);
    b->impl = NULL;
}

int ascolto_audio_backend_create(
    ascolto_audio_backend_t *b,
    bool use_mock,
    ascolto_audio_chunk_cb on_chunk,
    ascolto_mic_trigger_cb on_mic_trigger,
    void *user_data
) {
    if (!b) return -1;
    memset(b, 0, sizeof(*b));
    pw_backend_impl_t *impl = (pw_backend_impl_t *)calloc(1, sizeof(pw_backend_impl_t));
    if (!impl) return -1;
    impl->parent = b;

    b->impl = impl;
    b->use_mock = use_mock;
    b->on_chunk = on_chunk;
    b->on_mic_trigger = on_mic_trigger;
    b->cb_user_data = user_data;
    b->start_watcher = backend_start_watcher;
    b->stop_watcher = backend_stop_watcher;
    b->start_capture = backend_start_capture;
    b->stop_capture = backend_stop_capture;
    b->list_devices = backend_list_devices;
    b->destroy = backend_destroy;
    return 0;
}

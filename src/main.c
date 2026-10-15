#define _POSIX_C_SOURCE 200809L
#include "ascolto/core.h"
#include "ascolto/settings.h"
#include <fcntl.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

static void print_usage(const char *prog) {
    printf(
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --config <path>          Path to config.ini\n"
        "  --weights <path>         Path to whistle.cact model weights\n"
        "  --output-dir <dir>       Directory for saved transcripts\n"
        "  --socket <path>          Unix domain control socket path\n"
        "  --telemetry-log <path>   Structured JSONL observability log file\n"
        "  --lang <code>            Language (auto, it, en, es, fr, de, nl, pl)\n"
        "  --keywords <list>        Comma-separated custom keywords\n"
        "  --sensitivity <level>    Speaker diarization sensitivity (low, medium, high)\n"
        "  --save-audio             Save one FLAC file per channel (MIC, SYS) next to the transcript\n"
        "  --cloud-recap            After each call: cloud transcription + Claude recap (needs "
        "OPENAI_API_KEY)\n"
        "  --recap-tts              Also render the recap to MP3 (OpenAI TTS)\n"
        "  --no-auto-mic            Disable automatic microphone trigger on startup\n"
        "  --headless               Run without GTK/AppIndicator GUI\n"
        "  --settings               Open Settings without recording\n"
        "  --mock-engine            Deterministic transcription from fixture hints (requires --mock-audio)\n"
        "  --mock-audio             Use deterministic mock audio backend (test harness)\n"
        "  -h, --help               Show this help message\n",
        prog);
}

int main(int argc, char **argv) {
    ascolto_config_t cfg;
    ascolto_config_init_defaults(&cfg);

    bool explicit_config = false, show_settings = false;
    /* First check if --config was passed so we load it before applying CLI overrides */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            explicit_config = true;
            snprintf(cfg.config_path, sizeof(cfg.config_path), "%s", argv[i + 1]);
        }
    }
    ascolto_config_load(&cfg, cfg.config_path);
    char *autostart_path =
        g_build_filename(g_get_user_config_dir(), "autostart", "ascolto-daemon.desktop", NULL);
    cfg.launch_at_login = g_file_test(autostart_path, G_FILE_TEST_IS_REGULAR);
    g_free(autostart_path);
    umask(0077);

    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        }
    if (explicit_config) {
        if (strlen(cfg.config_path) > 508)
            return 1;
        snprintf(cfg.database_path, sizeof(cfg.database_path), "%.508s.db", cfg.config_path);
    } else
        snprintf(cfg.database_path, sizeof(cfg.database_path), "%s/.local/share/ascolto/ascolto.db",
                 g_get_home_dir());
    char *parent = g_path_get_dirname(cfg.database_path);
    g_mkdir_with_parents(parent, 0700);
    g_free(parent);
    char *lockpath = g_strconcat(cfg.database_path, ".lock", NULL);
    int lockfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    g_free(lockpath);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "Ascolto is already running. Use ascoltoctl OPEN_SETTINGS.\n");
        return 1;
    }
    if (ascolto_store_init(&cfg, explicit_config)) {
        fprintf(stderr, "Cannot open or migrate Ascolto database\n");
        return 1;
    }
    bool use_mock_audio = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--weights") == 0 && i + 1 < argc) {
            snprintf(cfg.weights_path, sizeof(cfg.weights_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            snprintf(cfg.control_socket, sizeof(cfg.control_socket), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--telemetry-log") == 0 && i + 1 < argc) {
            snprintf(cfg.telemetry_log, sizeof(cfg.telemetry_log), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            snprintf(cfg.language, sizeof(cfg.language), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--keywords") == 0 && i + 1 < argc) {
            snprintf(cfg.keywords, sizeof(cfg.keywords), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--sensitivity") == 0 && i + 1 < argc) {
            cfg.diarization_sensitivity = ascolto_sensitivity_from_string(argv[++i]);
        } else if (strcmp(argv[i], "--save-audio") == 0) {
            cfg.save_audio = true;
        } else if (strcmp(argv[i], "--cloud-recap") == 0) {
            cfg.cloud_recap = true;
        } else if (strcmp(argv[i], "--recap-tts") == 0) {
            cfg.recap_tts = true;
        } else if (strcmp(argv[i], "--no-auto-mic") == 0) {
            cfg.auto_mic_trigger = false;
        } else if (strcmp(argv[i], "--settings") == 0) {
            show_settings = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            cfg.headless = true;
        } else if (strcmp(argv[i], "--mock-engine") == 0) {
            cfg.mock_engine = true;
        } else if (strcmp(argv[i], "--mock-audio") == 0) {
            use_mock_audio = true;
        }
    }

    if (cfg.mock_engine && !use_mock_audio) {
        fprintf(stderr, "--mock-engine requires --mock-audio\n");
        return 1;
    }
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGPIPE, SIG_IGN);

    ascolto_core_t *core = (ascolto_core_t *)calloc(1, sizeof(ascolto_core_t));
    if (!core)
        return 1;

    if (ascolto_core_init(core, &cfg, use_mock_audio) != 0) {
        fprintf(stderr, "ascolto: failed to initialize core\n");
        free(core);
        return 1;
    }

    core->show_settings = show_settings;
    printf("ascolto started (socket=%s, whistle=%s, tray_gui=%s, auto_mic=%s)\n", core->config.control_socket,
           core->whistle.model_loaded ? "loaded" : "fallback",
           core->tray.snapshot.using_gui_indicator ? "active" : "headless",
           core->config.auto_mic_trigger ? "on" : "off");
    fflush(stdout);

    while (core->running && !g_stop_requested) {
        ascolto_core_tick(core);
        struct timespec ts = {0, 50 * 1000 * 1000}; /* 50 ms main loop tick */
        nanosleep(&ts, NULL);
    }

    ascolto_core_destroy(core);
    free(core);
    return 0;
}

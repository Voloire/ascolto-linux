#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void default_socket_path(char *buf, size_t cap) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        snprintf(buf, cap, "%s/ascolto.sock", xdg);
    } else {
        snprintf(buf, cap, "/tmp/ascolto-%d.sock", (int)getuid());
    }
}

int main(int argc, char **argv) {
    char sock_path[512];
    default_socket_path(sock_path, sizeof(sock_path));

    int arg_idx = 1;
    if (arg_idx + 1 < argc && (strcmp(argv[arg_idx], "-s") == 0 || strcmp(argv[arg_idx], "--socket") == 0)) {
        snprintf(sock_path, sizeof(sock_path), "%s", argv[arg_idx + 1]);
        arg_idx += 2;
    }

    if (arg_idx >= argc || strcmp(argv[arg_idx], "-h") == 0 || strcmp(argv[arg_idx], "--help") == 0) {
        printf("Usage: %s [-s <socket_path>] <COMMAND> [ARGS...]\n\n"
               "Commands:\n"
               "  STATUS\n"
               "  START | STOP | PAUSE | RESUME\n"
               "  SET_AUTO_MIC <0|1>\n"
               "  SET_LANG <auto|it|en|es|fr|de|nl|pl>\n"
               "  SET_MIC <device_name>\n"
               "  SET_SYS <device_name>\n"
               "  SET_SENSITIVITY <low|medium|high>\n"
               "  SET_KEYWORDS <comma_separated_keywords>\n"
               "  SIM_MIC_ACTIVE <0|1> [app_name]\n"
               "  INJECT_WAV <MIC|SYS> <wav_path> [hint_text]\n"
               "  INJECT_SYNTH_SPEECH <MIC|SYS> <f0_hz> <f1_hz> <f2_hz> <dur_ms> <text>\n"
               "  LIST_DEVICES | LIST_SPEAKERS | LIST_SEGMENTS | GET_TRAY_STATE | GET_EVENTS\n"
               "  QUIT\n",
               argv[0]);
        return (arg_idx >= argc) ? 1 : 0;
    }

    char cmd[2048] = {0};
    size_t off = 0;
    for (int i = arg_idx; i < argc && off + 2 < sizeof(cmd); i++) {
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, "%s%s", (i > arg_idx) ? " " : "", argv[i]);
    }
    snprintf(cmd + off, sizeof(cmd) - off, "\n");

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "ascoltoctl: cannot connect to %s: %s\n", sock_path, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, cmd, strlen(cmd)) < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
    }
    close(fd);
    return 0;
}

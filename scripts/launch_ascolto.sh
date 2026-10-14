#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASCOLTO_BIN="${ROOT_DIR}/bin/ascolto"
CTL_BIN="${ROOT_DIR}/bin/ascoltoctl"
DATA_DIR="${HOME}/.local/share/ascolto"
TRANSCRIPTS_DIR="${DATA_DIR}/transcripts"
LOG_FILE="${DATA_DIR}/daemon.log"

mkdir -p "${TRANSCRIPTS_DIR}"

notify_user() {
    local title="$1"
    local body="$2"
    if command -v notify-send >/dev/null 2>&1; then
        notify-send -i "${ROOT_DIR}/assets/ascolto-icon.svg" "${title}" "${body}" || true
    elif command -v gdbus >/dev/null 2>&1; then
        gdbus call --session \
            --dest org.freedesktop.Notifications \
            --object-path /org/freedesktop/Notifications \
            --method org.freedesktop.Notifications.Notify \
            "Ascolto" 0 "${ROOT_DIR}/assets/ascolto-icon.svg" \
            "${title}" "${body}" "[]" "{}" 4000 >/dev/null 2>&1 || true
    fi
}

is_running() {
    "${CTL_BIN}" STATUS >/dev/null 2>&1
}

wait_for_daemon() {
    for _ in $(seq 1 40); do
        if is_running; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

MODE="${1:-app}"

case "${MODE}" in
    daemon)
        if is_running; then
            notify_user "Ascolto Demone Attivo" "Il demone e gia in esecuzione nella barra in alto di GNOME."
            exit 0
        fi
        cd "${ROOT_DIR}"
        nohup "${ASCOLTO_BIN}" >>"${LOG_FILE}" 2>&1 &
        wait_for_daemon || true
        notify_user "Ascolto Demone Avviato" "In ascolto automatico del microfono (icona nella top bar GNOME)."
        ;;
    app)
        if ! is_running; then
            cd "${ROOT_DIR}"
            nohup "${ASCOLTO_BIN}" >>"${LOG_FILE}" 2>&1 &
            wait_for_daemon || true
        fi
        STATUS_JSON="$("${CTL_BIN}" STATUS 2>/dev/null || true)"
        if echo "${STATUS_JSON}" | grep -q '"state":"IDLE"'; then
            "${CTL_BIN}" START >/dev/null 2>&1 || true
            notify_user "Ascolto - Trascrizione Avviata" "Registrazione e trascrizione dual-channel (MIC + SYS) avviata."
        else
            notify_user "Ascolto Attivo" "Trascrizione in corso. Usa l'icona nella top bar per configurare o fermare."
        fi
        ;;
    start)
        if ! is_running; then
            cd "${ROOT_DIR}"
            nohup "${ASCOLTO_BIN}" >>"${LOG_FILE}" 2>&1 &
            wait_for_daemon || true
        fi
        "${CTL_BIN}" START >/dev/null 2>&1 || true
        notify_user "Ascolto" "Trascrizione manuale avviata."
        ;;
    stop)
        if is_running; then
            "${CTL_BIN}" STOP >/dev/null 2>&1 || true
            notify_user "Ascolto" "Trascrizione fermata e salvata."
        fi
        ;;
    pause)
        if is_running; then
            "${CTL_BIN}" PAUSE >/dev/null 2>&1 || true
            notify_user "Ascolto" "Trascrizione in pausa."
        fi
        ;;
    resume)
        if is_running; then
            "${CTL_BIN}" RESUME >/dev/null 2>&1 || true
            notify_user "Ascolto" "Trascrizione ripresa."
        fi
        ;;
    open-folder)
        xdg-open "${TRANSCRIPTS_DIR}" >/dev/null 2>&1 &
        ;;
    quit)
        if is_running; then
            "${CTL_BIN}" QUIT >/dev/null 2>&1 || true
            notify_user "Ascolto" "Demone terminato."
        fi
        ;;
    *)
        echo "Usage: $0 {app|daemon|start|stop|pause|resume|open-folder|quit}"
        exit 1
        ;;
esac

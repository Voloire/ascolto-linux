#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASCOLTO_BIN="${ROOT_DIR}/bin/ascolto"
CTL_BIN="${ROOT_DIR}/bin/ascoltoctl"
DATA_DIR="${HOME}/.local/share/ascolto"
TRANSCRIPTS_DIR="${HOME}/Documents/Ascolto"
LOG_FILE="${DATA_DIR}/daemon.log"

mkdir -p "${DATA_DIR}" "${TRANSCRIPTS_DIR}"

notify_user() {
    local title="$1"
    local body="$2"
    if command -v gdbus >/dev/null 2>&1; then
        gdbus call --session \
            --dest org.freedesktop.Notifications \
            --object-path /org/freedesktop/Notifications \
            --method org.freedesktop.Notifications.Notify \
            "Ascolto" 0 "${ROOT_DIR}/assets/ascolto-icon.svg" \
            "${title}" "${body}" "[]" "{}" 3500 >/dev/null 2>&1 || true
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send -i "${ROOT_DIR}/assets/ascolto-icon.svg" "${title}" "${body}" || true
    fi
}

open_folder_gui() {
    mkdir -p "${TRANSCRIPTS_DIR}"
    if command -v gdbus >/dev/null 2>&1; then
        gdbus call --session \
            --dest org.freedesktop.FileManager1 \
            --object-path /org/freedesktop/FileManager1 \
            --method org.freedesktop.FileManager1.ShowFolders \
            "['file://${TRANSCRIPTS_DIR}']" "" >/dev/null 2>&1 && return 0
    fi
    gio open "${TRANSCRIPTS_DIR}" >/dev/null 2>&1 &
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

ensure_daemon_started() {
    if ! is_running; then
        cd "${ROOT_DIR}"
        setsid -f "${ASCOLTO_BIN}" >>"${LOG_FILE}" 2>&1 </dev/null
        wait_for_daemon || true
    fi
}

MODE="${1:-app}"

case "${MODE}" in
    daemon)
        if is_running; then
            STATUS_JSON="$("${CTL_BIN}" STATUS 2>/dev/null || true)"
            if ! echo "${STATUS_JSON}" | grep -q '"state":"IDLE"'; then
                "${CTL_BIN}" STOP >/dev/null 2>&1 || true
                notify_user "Ascolto Demone" "Sessione corrente fermata. Demone in attesa automatica del microfono."
            else
                notify_user "Ascolto Demone Attivo" "Demone in esecuzione nella barra GNOME (Auto-Mic attivo)."
            fi
            exit 0
        fi
        ensure_daemon_started
        notify_user "Ascolto Demone Avviato" "In ascolto automatico del microfono nella barra in alto di GNOME."
        ;;
    app|toggle)
        ensure_daemon_started
        STATUS_JSON="$("${CTL_BIN}" STATUS 2>/dev/null || true)"
        if echo "${STATUS_JSON}" | grep -q '"state":"IDLE"'; then
            "${CTL_BIN}" START >/dev/null 2>&1 || true
            notify_user "Ascolto - Trascrizione Avviata" "Registrazione dual-channel (MIC + SYS) in corso. Clicca di nuovo per fermare."
        else
            "${CTL_BIN}" STOP >/dev/null 2>&1 || true
            notify_user "Ascolto - Trascrizione Fermata" "File salvato in ~/Documents/Ascolto. Pronto per una nuova trascrizione."
        fi
        ;;
    start)
        ensure_daemon_started
        "${CTL_BIN}" START >/dev/null 2>&1 || true
        notify_user "Ascolto" "Nuova trascrizione manuale avviata."
        ;;
    stop)
        if is_running; then
            "${CTL_BIN}" STOP >/dev/null 2>&1 || true
            notify_user "Ascolto" "Trascrizione fermata e salvata in ~/Documents/Ascolto."
        fi
        ;;
    pause)
        if is_running; then
            STATUS_JSON="$("${CTL_BIN}" STATUS 2>/dev/null || true)"
            if echo "${STATUS_JSON}" | grep -q '"state":"PAUSED"'; then
                "${CTL_BIN}" RESUME >/dev/null 2>&1 || true
                notify_user "Ascolto" "Trascrizione ripresa."
            else
                "${CTL_BIN}" PAUSE >/dev/null 2>&1 || true
                notify_user "Ascolto" "Trascrizione in pausa."
            fi
        fi
        ;;
    resume)
        if is_running; then
            "${CTL_BIN}" RESUME >/dev/null 2>&1 || true
            notify_user "Ascolto" "Trascrizione ripresa."
        fi
        ;;
    open-folder)
        open_folder_gui
        ;;
    quit)
        if is_running; then
            "${CTL_BIN}" QUIT >/dev/null 2>&1 || true
            notify_user "Ascolto" "Demone terminato."
        fi
        ;;
    *)
        echo "Usage: $0 {app|toggle|daemon|start|stop|pause|resume|open-folder|quit}"
        exit 1
        ;;
esac

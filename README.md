# ascolto

Applicativo desktop nativo in C per Linux (Ubuntu / GNOME) che rileva automaticamente le call e le trascrive in tempo reale in locale usando il motore Whistle di Cactus Compute, con separazione doppio canale e diarizzazione degli interlocutori.

## Caratteristiche principali

- **Attivazione automatica sul microfono**: monitora lo stack audio PipeWire per rilevare quando un client (Meet, Zoom, Teams, Slack, browser) apre uno stream di cattura sul microfono e avvia o ferma automaticamente la sessione di trascrizione.
- **Motore di trascrizione Whistle on-device**: integrazione C nativa tramite `libneedle.a`, `needle.h` e il modello `whistle.cact` per inferenza streaming su CPU a bassa latenza, con supporto multilingua e vocabolario personalizzato.
- **Doppio canale audio e Speaker Diarization**:
  - **Canale MIC**: cattura il microfono dell'utente ed etichetta i segmenti come `ME`.
  - **Canale SYS**: cattura il monitor dell'output di sistema e distingue i diversi partecipanti (`SPEAKER_1`, `SPEAKER_2`, ...) tramite clustering coseno incrementale sugli speech embedding estratti da Whistle.
- **Integrazione barra di sistema GNOME**: icona nella top bar di Ubuntu (`StatusNotifierItem` / `Ayatana AppIndicator`) con indicatore di stato, timer di sessione e menu di configurazione completo (avvio/stop manuale, pausa, toggle attivazione automatica, lingua, selezione sorgenti audio MIC e SYS, sensibilità diarizzazione, keywords personalizzate e accesso rapido alle trascrizioni).
- **Test harness e osservabilità**: architettura a backend disaccoppiati con socket di controllo Unix, telemetria strutturata JSONL, iniezione di tracce multi-interlocutore e test automatizzati unitari ed end-to-end.

## Struttura del repository

- `src/`: codice sorgente C dell'applicativo e dei sottosistemi (core, audio, motore Whistle, diarizer, UI tray, telemetria).
- `include/`: header C e contratti delle interfacce modulari.
- `tests/`: test harness, simulatore multi-speaker, fixture audio e suite di verifica end-to-end.
- `tools/`: utility CLI (`ascoltoctl`) per ispezione stato, telemetria e pilotaggio in modalità headless.
- `docs/`: documentazione tecnica e architetturale (in inglese).
- `third_party/whistle/`: header `needle.h`, libreria statica `libneedle.a` e pesi del modello `whistle.cact`.

## Requisiti di sistema

- Ubuntu 24.04 LTS / 26.04 LTS con GNOME Shell e PipeWire (`pipewire`, `wireplumber`, `ubuntu-appindicators`).
- Compilatore C (`gcc` >= 13), `make`, `pkg-config`, `libglib2.0-dev`, `libgio-2.0-dev`, `libjson-glib-dev`.
- Runtime `libpipewire-0.3`, `libpulse0`, `libgtk-3-0`, `libayatana-appindicator3-1`.

## Compilazione e Test

Per scaricare automaticamente gli artefatti di Whistle (`needle.h`, `libneedle.a`, `libneedle.so`, `whistle.cact`), compilare tutti i binari (`bin/ascolto`, `bin/ascoltoctl`, `bin/test_harness`) ed eseguire la suite completa di test:

```sh
make test
```

## Avvio dell'applicativo

```sh
./bin/ascolto
```

Opzioni principali:
- `--lang <auto|it|en|es|fr|de|nl|pl>`: imposta la lingua di trascrizione.
- `--keywords "Bitrock, Whistle, PipeWire"`: imposta keywords personalizzate per il biasing del modello.
- `--sensitivity <low|medium|high>`: regola la sensibilita del clustering speaker diarization.
- `--output-dir <percorso>`: directory in cui salvare le trascrizioni `.md` e `.jsonl` (default `~/.local/share/ascolto/transcripts`).
- `--socket <percorso>`: path del socket Unix di controllo e osservabilita (default `$XDG_RUNTIME_DIR/ascolto.sock`).

## Controllo da CLI e Osservabilita (`ascoltoctl`)

Durante l'esecuzione del demone e possibile ispezionare lo stato o inviare comandi tramite `ascoltoctl`:

```sh
./bin/ascoltoctl STATUS
./bin/ascoltoctl GET_TRAY_STATE
./bin/ascoltoctl LIST_DEVICES
./bin/ascoltoctl LIST_SPEAKERS
./bin/ascoltoctl LIST_SEGMENTS
./bin/ascoltoctl START
./bin/ascoltoctl PAUSE
./bin/ascoltoctl RESUME
./bin/ascoltoctl STOP
```

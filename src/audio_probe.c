/* Local input diagnostic. No playback, persistent recording or network call. */
#include "mic_probe.h"
#include "rsxutil.h"
#include "ui.h"
#include <io/pad.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#define PROBEDIR "/dev_hdd0/game/ORB000002/USRDIR"
static int running = 1, loaded, initialized, started, opened, active, count, player;
static OrbitMicFormat format;
static unsigned long bytes_received;
static int level;
static uint64_t next_meter;
static uint64_t deadline;
static char rows[12][100],
    status[192] = "Bereit. KREUZ prueft dein Mikrofon direkt, ohne Voice-Port.";
static FILE *logfile;
static uint64_t millis(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000 + t.tv_usec / 1000;
}
static void callback(u64 event, u64 param, void *user) {
    (void)param;
    (void)user;
    if (event == SYSUTIL_EXIT_GAME)
        running = 0;
}
static int result(const char *name, int32_t code) {
    if (count < 12)
        snprintf(rows[count++], 100, "%s: 0x%08X", name, (unsigned)code);
    if (logfile) {
        fprintf(logfile, "%s: 0x%08X\n", name, (unsigned)code);
        fflush(logfile);
    }
    if (code)
        snprintf(status, sizeof(status), "Test gestoppt: %s meldet 0x%08X.", name, (unsigned)code);
    return code;
}
static void cleanup(void) {
    /* Preserve the first failure on screen; teardown results go to the log. */
#define CLOSE(label, call)                                                                         \
    do {                                                                                           \
        int32_t rc = (call);                                                                       \
        if (logfile)                                                                               \
            fprintf(logfile, "%s: 0x%08X\n", label, (unsigned)rc);                                 \
    } while (0)
    if (started)
        CLOSE("Stop", cellMicStop(player));
    if (opened)
        CLOSE("Mikrofon schliessen", cellMicClose(player));
    if (initialized)
        CLOSE("End", cellMicEnd());
    if (loaded)
        CLOSE("Unload", sysModuleUnload(SYSMODULE_MIC));
    if (logfile) {
        fprintf(logfile, "Tatsaechlich empfangene Rohdaten: %lu Bytes\n", bytes_received);
        fclose(logfile);
        logfile = NULL;
    }
    loaded = initialized = started = opened = active = 0;
#undef CLOSE
}
static void begin(void) {
    cleanup();
    count = 0;
    memset(rows, 0, sizeof(rows));
    bytes_received = 0;
    level = 0;
    next_meter = 0;
    logfile = fopen(PROBEDIR "/audio-test.txt", "w");
    if (logfile)
        fprintf(logfile, "Orbit Audio Probe 0.2; cellMic; device=%d\n", player);
    if (result("Mikrofon-Modul laden", sysModuleLoad(SYSMODULE_MIC)))
        goto fail;
    loaded = 1;
    if (result("Mikrofon initialisieren", cellMicInit()))
        goto fail;
    initialized = 1;
    int attached = cellMicIsAttached(player);
    if (attached <= 0) {
        if (attached < 0)
            result("Geraet suchen", attached);
        else
            snprintf(status, sizeof(status),
                     "An Zuordnung %d ist kein Mikrofon verfuegbar. Andere Zuordnung probieren.",
                     player);
        goto fail;
    }
    int32_t rc = cellMicOpenRaw(player, 48000, 1);
    result("Rohdaten oeffnen (48 kHz)", rc);
    if (rc && result("Rohdaten oeffnen (16 kHz)", cellMicOpenRaw(player, 16000, 1)))
        goto fail;
    opened = 1;
    memset(&format, 0, sizeof(format));
    if (result("Audioformat lesen", cellMicGetFormatRaw(player, &format)))
        goto fail;
    if (logfile)
        fprintf(logfile, "Format: rate=%u channels=%u bits=%u bytes=%u endian=%u\n",
                format.sample_rate, format.channels, format.bits, format.subframe, format.type);
    if (result("Aufnahme starten", cellMicStart(player)))
        goto fail;
    started = active = 1;
    deadline = millis() + 12000;
    snprintf(
        status, sizeof(status),
        "Sprich ins Mikrofon. Der Pegel zeigt echte Eingangsdaten. Kein Mithoeren in diesem Test.");
    return;
fail:
    cleanup();
}
static void capture(void) {
    unsigned char data[8192];
    int peak = 0;
    for (int chunk = 0; chunk < 4; chunk++) {
        int32_t n = cellMicReadRaw(player, data, sizeof(data));
        if (n < 0) {
            result("Mikrofondaten lesen", n);
            cleanup();
            return;
        }
        if (!n)
            break;
        if (n > (int)sizeof(data)) {
            snprintf(status, sizeof(status), "Ungueltige Audiolaenge vom Treiber.");
            cleanup();
            return;
        }
        bytes_received += (unsigned)n;
        if (format.bits == 16 && format.subframe == 2 && format.type <= 1) {
            int magnitude = orbit_pcm_peak16(data, (size_t)n, format.type);
            if (magnitude > peak)
                peak = magnitude;
        }
    }
    if (peak * 100 / 32768 > level)
        level = peak * 100 / 32768;
}
static unsigned buttons(void) {
    static unsigned held;
    padInfo info = {0};
    padData data;
    if (ioPadGetInfo(&info))
        return 0;
    for (int i = 0; i < 7; i++)
        if (info.status[i]) {
            memset(&data, 0, sizeof(data));
            if (ioPadGetData(i, &data) || data.len <= 0)
                return held;
            held = (data.BTN_CROSS ? 1 : 0) | (data.BTN_CIRCLE ? 2 : 0) | (data.BTN_START ? 4 : 0) |
                   (data.BTN_LEFT ? 8 : 0) | (data.BTN_RIGHT ? 16 : 0);
            return held;
        }
    return held = 0;
}
int main(void) {
    if (ui_init(PROBEDIR "/OrbitSans.ttf"))
        return 1;
    void *host = memalign(1024 * 1024, HOST_SIZE);
    if (!host)
        return 2;
    gcmContextData *context = initScreen(host, HOST_SIZE);
    u16 width, height;
    if (!context || !getResolution(&width, &height))
        return 3;
    rsxBuffer buffers[2] = {0};
    for (int i = 0; i < 2; i++) {
        if (!makeBuffer(&buffers[i], width, height, i))
            return 4;
        memset(buffers[i].ptr, 0, (size_t)width * height * 4);
    }
    uint32_t *canvas = memalign(128, (size_t)width * height * 4);
    if (!canvas)
        return 5;
    ioPadInit(7);
    sysUtilRegisterCallback(0, callback, NULL);
    int index = 0, generation = 0, versions[2] = {-1, -1}, old_remaining = -1, dirty = 1;
    unsigned previous = 0;
    flip(context, 1);
    while (running) {
        sysUtilCheckCallback();
        unsigned held = buttons(), pressed = held & ~previous;
        previous = held;
        if (pressed & 4)
            running = 0;
        if ((pressed & 2) || (active && millis() >= deadline)) {
            cleanup();
            snprintf(status, sizeof(status),
                     bytes_received ? "Test beendet: Mikrofon lieferte Daten. Headset-Ausgabe ist "
                                      "noch nicht getestet."
                                    : "Test beendet: Keine Mikrofon-Daten empfangen.");
            dirty = 1;
        }
        if (!active && !(pressed & (2 | 4))) {
            if (pressed & 8) {
                player = (player + 7) % 8;
                dirty = 1;
            }
            if (pressed & 16) {
                player = (player + 1) % 8;
                dirty = 1;
            }
            if (pressed & 1) {
                begin();
                dirty = 1;
            }
        }
        if (active)
            capture();
        uint64_t now = millis();
        if (active && now >= next_meter) {
            if (format.bits == 16 && format.subframe == 2 && format.type <= 1)
                snprintf(rows[8], 100, "Empfangen: %lu Bytes | Pegel: %d %%", bytes_received,
                         level);
            else
                snprintf(rows[8], 100,
                         "Empfangen: %lu Bytes | Pegel fuer dieses Format nicht verfuegbar",
                         bytes_received);
            count = 9;
            next_meter = now + 100;
            level = level * 2 / 3;
            dirty = 1;
        }
        int remaining = active && deadline > now ? (int)((deadline - now + 999) / 1000) : 0;
        if (remaining != old_remaining) {
            dirty = 1;
            old_remaining = remaining;
        }
        if (dirty) {
            ui_audio_probe(canvas, width, height, status, rows, count, player, remaining,
                           format.bits == 16 && format.subframe == 2 && format.type <= 1 ? level
                                                                                         : -1);
            generation++;
            dirty = 0;
        }
        waitFlip();
        if (versions[index] != generation) {
            memcpy(buffers[index].ptr, canvas, (size_t)width * height * 4);
            versions[index] = generation;
        }
        if (!flip(context, index))
            break;
        index ^= 1;
    }
    cleanup();
    sysUtilUnregisterCallback(0);
    ioPadEnd();
    rsxFinish(context, 1);
    free(canvas);
    ui_close();
    return 0;
}

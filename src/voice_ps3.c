/* PS3 voice devices: cellMic raw capture (as verified with Orbit Audio Test 0.2) and a libaudio
 * 2-channel port. Runs in its own thread so the UI frame rate is never affected. */
#include "mic_probe.h"
#include "orbit.h"
#include "voice.h"
#include <audio/audio.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sysmodule/sysmodule.h>
#include <unistd.h>
static pthread_t thread;
static volatile int running, has_mic;
static int mic_loaded, mic_init, mic_open, mic_started, mic_device = -1, audio_init, port_open;
static u32 port;
static audioPortConfig config;
static OrbitMicFormat format;
static void mic_close(void) {
    if (mic_started)
        cellMicStop(mic_device);
    if (mic_open)
        cellMicClose(mic_device);
    if (mic_init)
        cellMicEnd();
    if (mic_loaded)
        sysModuleUnload(SYSMODULE_MIC);
    mic_started = mic_open = mic_init = mic_loaded = 0;
    has_mic = 0;
}
static int mic_start(void) {
    if (sysModuleLoad(SYSMODULE_MIC))
        return -1;
    mic_loaded = 1;
    if (cellMicInit())
        return -1;
    mic_init = 1;
    /* The headset can sit on any device slot: use the first one that reports a microphone. */
    for (int i = 0; i < 8 && mic_device < 0; i++)
        if (cellMicIsAttached(i) > 0)
            mic_device = i;
    if (mic_device < 0)
        return -1;
    if (cellMicOpenRaw(mic_device, 48000, 1) && cellMicOpenRaw(mic_device, 16000, 1))
        return -1;
    mic_open = 1;
    memset(&format, 0, sizeof(format));
    if (cellMicGetFormatRaw(mic_device, &format) || format.bits != 16 || format.subframe != 2)
        return -1;
    if (cellMicStart(mic_device))
        return -1;
    mic_started = 1;
    has_mic = 1;
    return 0;
}
static void capture(void) {
    static uint8_t raw[8192];
    static int16_t pcm[4096 + 3], small[2048];
    static int carry;
    for (int round = 0; round < 4; round++) {
        int32_t n = cellMicReadRaw(mic_device, raw, sizeof(raw));
        if (n <= 0 || n > (int32_t)sizeof(raw))
            return;
        int channels = format.channels ? format.channels : 1, stride = 2 * channels;
        int count = n / stride;
        for (int i = 0; i < count && carry < 4096; i++) {
            const uint8_t *s = raw + i * stride;
            pcm[carry++] = (int16_t)(format.type ? (s[0] << 8 | s[1]) : (s[1] << 8 | s[0]));
        }
        int rate = format.sample_rate ? (int)format.sample_rate : 48000;
        int usable = rate == 48000 ? carry - carry % 3 : carry;
        int k = voice_resample_to16k(pcm, usable, rate, small, 2048);
        memmove(pcm, pcm + usable, (size_t)(carry - usable) * sizeof(int16_t));
        carry -= usable;
        voice_capture(small, k);
    }
}
static void *device_main(void *arg) {
    (void)arg;
    u64 last = *(volatile u64 *)(u64)config.readIndex;
    const int blocks = (int)config.numBlocks;
    float *base = (float *)(u64)config.audioDataStart;
    while (running) {
        if (has_mic)
            capture();
        u64 now = *(volatile u64 *)(u64)config.readIndex;
        /* Fill each block when it is half a buffer ahead of the hardware read position. */
        while (last != now) {
            last = (last + 1) % (u64)blocks;
            u64 target = (last + (u64)blocks / 2) % (u64)blocks;
            voice_mix48(base + target * 2 * AUDIO_BLOCK_SAMPLES, AUDIO_BLOCK_SAMPLES);
        }
        usleep(2000);
    }
    return NULL;
}
int voice_device_start(char *message, size_t cap) {
    if (running)
        return 0;
    voice_reset();
    message[0] = 0;
    if (mic_start()) {
        mic_close();
        snprintf(message, cap, "Kein Mikrofon gefunden - du hörst nur zu. Headset einstecken und "
                               "Sprachchat neu betreten.");
    }
    sysModuleLoad(SYSMODULE_AUDIO);
    if (audioInit() && !audio_init) {
        mic_close();
        snprintf(message, cap, "Tonausgabe konnte nicht gestartet werden.");
        return -1;
    }
    audio_init = 1;
    audioPortParam p;
    memset(&p, 0, sizeof(p));
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks = AUDIO_BLOCK_8;
    p.attrib = AUDIO_PORT_INITLEVEL;
    p.level = 1.0f;
    if (audioPortOpen(&p, &port) || audioGetPortConfig(port, &config)) {
        mic_close();
        snprintf(message, cap, "Tonausgabe belegt. Orbit neu starten.");
        return -1;
    }
    port_open = 1;
    memset((void *)(u64)config.audioDataStart, 0,
           config.numBlocks * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
    audioPortStart(port);
    running = 1;
    if (pthread_create(&thread, NULL, device_main, NULL)) {
        running = 0;
        voice_device_stop();
        snprintf(message, cap, "Sprachchat konnte nicht starten.");
        return -1;
    }
    return 0;
}
void voice_device_stop(void) {
    if (running) {
        running = 0;
        pthread_join(thread, NULL);
    }
    if (port_open) {
        audioPortStop(port);
        audioPortClose(port);
        port_open = 0;
    }
    if (audio_init) {
        audioQuit();
        audio_init = 0;
    }
    mic_close();
    mic_device = -1;
    voice_reset();
}
int voice_device_has_mic(void) {
    return has_mic;
}

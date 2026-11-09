/* Minimal public cellMic ABI; no proprietary SDK. */
#ifndef ORBIT_MIC_PROBE_H
#define ORBIT_MIC_PROBE_H
#include <stddef.h>
#include <stdint.h>
static inline int orbit_pcm_peak16(const uint8_t *data, size_t size, int big_endian) {
    int peak = 0;
    for (size_t i = 0; i + 1 < size; i += 2) {
        unsigned value =
            big_endian ? (data[i] * 256u + data[i + 1]) : (data[i + 1] * 256u + data[i]);
        int sample = (int16_t)value, magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak)
            peak = magnitude;
    }
    return peak;
}
typedef struct {
    uint8_t channels, subframe, bits, type;
    uint32_t sample_rate;
} OrbitMicFormat;
_Static_assert(sizeof(OrbitMicFormat) == 8, "microphone format ABI");
int32_t cellMicInit(void);
int32_t cellMicEnd(void);
int32_t cellMicIsAttached(int32_t);
int32_t cellMicOpenRaw(int32_t, int32_t, int32_t);
int32_t cellMicClose(int32_t);
int32_t cellMicStart(int32_t);
int32_t cellMicStop(int32_t);
int32_t cellMicGetFormatRaw(int32_t, OrbitMicFormat *);
int32_t cellMicReadRaw(int32_t, void *, int32_t);
#endif

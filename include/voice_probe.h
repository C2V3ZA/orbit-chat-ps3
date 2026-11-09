/* Minimal public ABI declarations; see THIRD-PARTY.md for references. */
#ifndef ORBIT_VOICE_PROBE_H
#define ORBIT_VOICE_PROBE_H
#include <stddef.h>
#include <stdint.h>
typedef struct __attribute__((aligned(32))) {
    uint32_t eventMask, version;
    int32_t appType;
    uint8_t reserved[20];
} OrbitVoiceInit;
typedef struct __attribute__((aligned(64))) {
    uint32_t portType;
    uint16_t threshold, mute;
    float volume;
    union {
        uint32_t playerId;
        uint8_t reserved[16];
    } device;
} OrbitVoicePort;
_Static_assert(sizeof(OrbitVoiceInit) == 32, "voice init ABI");
_Static_assert(sizeof(OrbitVoicePort) == 64, "voice port alignment");
_Static_assert(offsetof(OrbitVoicePort, device) == 12, "voice device ABI");
int32_t cellVoiceInit(const OrbitVoiceInit *);
int32_t cellVoiceEnd(void);
int32_t cellVoiceCreatePort(uint32_t *, const OrbitVoicePort *);
int32_t cellVoiceDeletePort(uint32_t);
int32_t cellVoiceConnectIPortToOPort(uint32_t, uint32_t);
int32_t cellVoiceStart(void);
int32_t cellVoiceStop(void);
int32_t cellVoiceResumePortAll(void);
#endif

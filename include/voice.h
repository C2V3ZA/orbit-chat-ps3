/* Orbit voice chat: 16 kHz mono, 20 ms frames, IMA-ADPCM (64 kbit/s), relayed by the group host. */
#ifndef ORBIT_VOICE_H
#define ORBIT_VOICE_H
#include <stddef.h>
#include <stdint.h>
#define VOICE_RATE 16000
#define VOICE_FRAME 320
#define VOICE_PACKET 168
#define VOICE_USERS 32
enum { VF_AUDIO = 1, VF_PING = 2 };
/* packet: type, user, seq(2), predictor(2), index, 0, 160 bytes ADPCM */
void voice_reset(void);
/* capture: 16 kHz mono samples from the microphone thread */
void voice_capture(const int16_t *pcm, int n);
int voice_take_packet(uint8_t out[VOICE_PACKET]);
void voice_set_muted(int muted);
int voice_muted(void);
int voice_mic_level(void);
int voice_self_speaking(void);
/* playback: packets from the network, pulled as 48 kHz stereo float by the audio thread */
void voice_receive(const uint8_t *packet, int n);
void voice_mix48(float *stereo, int frames);
int voice_speaking(int user);
/* helpers shared with the device layer */
int voice_resample_to16k(const int16_t *in, int n, int rate, int16_t *out, int cap);
/* device layer (PS3: cellMic + cellAudio; host: silent) */
int voice_device_start(char *message, size_t capacity);
void voice_device_stop(void);
int voice_device_has_mic(void);
#endif

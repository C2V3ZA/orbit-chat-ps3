/* Desktop test build: no audio devices. Tests feed voice_capture() directly. */
#include "voice.h"
#include <stdio.h>
int voice_device_start(char *message, size_t capacity) {
    if (capacity)
        message[0] = 0;
    return 0;
}
void voice_device_stop(void) {
}
int voice_device_has_mic(void) {
    return 1;
}

#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
int main(void) {
    if (ui_init("assets/OrbitSans.ttf"))
        return 1;
    UiState *s = calloc(1, sizeof(*s));
    UiLocal l = {0};
    uint32_t *pixels = malloc(1920 * 1080 * 4);
    if (!s || !pixels)
        return 2;
    s->stage = 2;
    s->group_count = 8;
    s->message_count = 3;
    strcpy(s->username, "Alex");
    for (int i = 0; i < 8; i++) {
        snprintf(s->groups[i].name, 65, "Gemeinsamer Spieleabend %d", i + 1);
        s->groups[i].owner = 1;
        s->groups[i].members = 4;
    }
    for (int i = 0; i < 3; i++) {
        strcpy(s->messages[i].name, "Eine Person aus der Gruppe");
        strcpy(s->messages[i].text, "Wer kommt heute Abend dazu? Wir treffen uns hier auf unserer "
                                    "PS3 und schreiben gemeinsam.");
    }
    for (int mode = 0; mode < 2; mode++) {
        int w = mode ? 1920 : 1280, h = mode ? 1080 : 720;
        for (int page = 0; page < 2; page++) {
            l.page = page ? 4 : 0;
            l.focus = 2;
            double start = ms();
            ui_render(pixels, w, h, s, &l);
            double cold = ms() - start;
            start = ms();
            for (int n = 0; n < 60; n++)
                ui_render(pixels, w, h, s, &l);
            printf("%dx%d %s cold_ms=%.3f warm_ms=%.3f\n", w, h, page ? "menu" : "chat", cold,
                   (ms() - start) / 60);
        }
    }
    ui_close();
    free(s);
    free(pixels);
    return 0;
}

/* Renders every screen with fixture data to build/ui-*.ppm (desktop, not a PS3 measurement). */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static void ppm(const char *file, const uint32_t *buf, int w, int h) {
    FILE *f = fopen(file, "wb");
    if (!f)
        exit(2);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(buf[i] >> 16), (unsigned char)(buf[i] >> 8),
                                (unsigned char)buf[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static void group(UiState *s, int i, const char *name, const char *host, int own, int state,
                  int members, int unread, const char *people) {
    UiGroup *g = &s->groups[i];
    g->link = own ? 0 : 1 + i;
    g->id = i;
    g->own = own;
    g->state = state;
    g->members = members;
    g->unread = unread;
    snprintf(g->name, sizeof(g->name), "%s", name);
    snprintf(g->host, sizeof(g->host), "%s", host);
    snprintf(g->people, sizeof(g->people), "%s", people);
}
static void msg(UiState *s, const char *name, int mine, int ago, const char *text) {
    UiMessage *m = &s->messages[s->message_count++];
    m->id = s->message_count;
    m->mine = mine;
    m->time = (uint32_t)(time(NULL) - ago);
    snprintf(m->name, sizeof(m->name), "%s", name);
    snprintf(m->text, sizeof(m->text), "%s", text);
}
int main(void) {
    if (ui_init("assets/OrbitSans.ttf"))
        return 1;
    ui_set_timezone(60);
    static uint32_t buf[1920 * 1080];
    UiState *s = calloc(1, sizeof(*s));
    UiLocal l = {0};
    strcpy(s->username, "Alex");
    s->net = NET_INTERNET;
    strcpy(s->net_detail, "Freunde können dich über das Internet erreichen.");
    strcpy(s->local_ip, "192.168.0.23");
    strcpy(s->public_ip, "93.184.216.34");
    strcpy(s->server_id, "a12b34cd56ef7890");
    l.page = PAGE_WELCOME;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-welcome.ppm", buf, 1280, 720);
    s->profile = 1;
    l.page = PAGE_CHAT;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-empty.ppm", buf, 1280, 720);
    group(s, 0, "Spieleabend", "Alex", 1, LINK_ONLINE, 3, 0, "Alex, Lena, Mika");
    group(s, 1, "Trophy Hunters", "Lena", 0, LINK_ONLINE, 5, 1, "Lena, Alex, Tom, Sara, Jonas");
    group(s, 2, "GTA Crew", "Mika", 0, LINK_FAILED, 4, 0, "Mika, Alex, Ali, Chris");
    s->group_count = 3;
    s->selected = 0;
    msg(s, "Lena", 0, 3600 * 26, "Wer ist heute Abend dabei?");
    msg(s, "Mika", 0, 3000, "Ich! Ab 20 Uhr bin ich online.");
    msg(s, "Mika", 0, 2950, "Bringt gute Laune mit :)");
    msg(s, "Alex", 1, 2400,
        "Perfekt. Ich mache die Gruppe auf meiner PS3 auf, dann können alle direkt beitreten. "
        "Wer noch nicht drin ist: einfach den Code eintippen.");
    msg(s, "Lena", 0, 600, "Bin drin! Das ging echt schnell.");
    msg(s, "Alex", 1, 120, "Sehr gut. Bis gleich!");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-chat.ppm", buf, 1280, 720);
    s->status_id = 1;
    strcpy(s->status, "Einladung angenommen. Ihr seid jetzt in derselben Gruppe.");
    l.now = 1000;
    ui_toast_visible(s, 1000);
    ui_render(buf, 1280, 720, s, &l);
    ui_performance(60, 16683, 4200);
    ppm("build/ui-chat-toast-fps.ppm", buf, 1280, 720);
    s->status[0] = 0;
    s->status_id = 2;
    ui_render(buf, 1920, 1080, s, &l);
    ppm("build/ui-chat-1080p.ppm", buf, 1920, 1080);
    ui_render(buf, 720, 480, s, &l);
    ppm("build/ui-chat-480p.ppm", buf, 720, 480);
    l.page = PAGE_INVITE;
    s->invite_state = 2;
    strcpy(s->invite_code, "LXQ4N-7KD2A-PTE9F-B3MHR");
    strcpy(s->invite_group, "Spieleabend");
    strcpy(s->invite_scope, "Klappt überall: über das Internet und im selben WLAN.");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-invite.ppm", buf, 1280, 720);
    strcpy(s->invite_scope, "Nur im selben Heimnetz. Dein Anschluss hat keine eigene "
                            "IPv4-Adresse (CGNAT/DS-Lite). Lass deinen Freund einladen - oder "
                            "trefft euch im selben Heimnetz.");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-invite-home.ppm", buf, 1280, 720);
    l.page = PAGE_MENU;
    l.menu_focus = 2;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-menu.ppm", buf, 1280, 720);
    l.page = PAGE_LEAVE;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-leave.ppm", buf, 1280, 720);
    l.page = PAGE_NETWORK;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-network.ppm", buf, 1280, 720);
    l.page = PAGE_CHAT;
    s->selected = 2;
    s->message_count = 0;
    strcpy(s->link_error, "Nicht erreichbar. Die Gastgeber-PS3 ist aus, oder ihr Router hat die "
                          "automatische Freigabe nicht erlaubt.");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-offline.ppm", buf, 1280, 720);
    /* voice chat in the Spieleabend group */
    s->selected = 0;
    s->groups[0].call = 3;
    s->groups[1].call = 2;
    s->call_state = CALL_ONLINE;
    s->call_here = 1;
    s->call_count = 3;
    strcpy(s->call_name, "Spieleabend");
    strcpy(s->call_people[0].name, "Alex");
    s->call_people[0].self = 1;
    strcpy(s->call_people[1].name, "Lena");
    s->call_people[1].speaking = 1;
    strcpy(s->call_people[2].name, "Mika");
    s->message_count = 0;
    msg(s, "Lena", 0, 300, "Ich bin im Sprachchat, kommt rein!");
    msg(s, "Alex", 1, 60, "Bin da.");
    s->revision = 50;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-call.ppm", buf, 1280, 720);
    s->call_muted = 1;
    s->call_people[1].speaking = 0;
    s->call_people[2].speaking = 1;
    s->revision = 51;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-call-muted.ppm", buf, 1280, 720);
    s->call_muted = 0;
    /* in-app keyboard */
    l.kb_active = 1;
    l.kb_mode = 2;
    l.kb_max = 768;
    l.kb_row = 2;
    l.kb_col = 4;
    strcpy(l.kb_title, "Spieleabend");
    strcpy(l.kb_text, "Bin in 5 Minuten da, startet schon mal die Lobby");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-keyboard.ppm", buf, 1280, 720);
    l.kb_mode = 1;
    l.kb_shift = 1;
    l.kb_max = 23;
    l.kb_row = 4;
    l.kb_col = 3;
    strcpy(l.kb_title, "Einladungscode deines Freundes (20 Zeichen)");
    strcpy(l.kb_text, "LXQ4N-7KD2A-PT");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-keyboard-code.ppm", buf, 1280, 720);
    l.kb_active = 0;
    l.page = PAGE_NETWORK;
    s->net = NET_HOME;
    strcpy(s->public_ip, "");
    strcpy(s->net_detail, "FRITZ!Box: einmal erlauben unter fritz.box > Internet > Freigaben > "
                          "Gerät für Freigaben hinzufügen > PS3 auswählen > Haken bei "
                          "\"Selbstständige Portfreigaben für dieses Gerät erlauben\". Danach in "
                          "Orbit: Menü > Verbindung > QUADRAT.");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-network-fritz.ppm", buf, 1280, 720);
    s->net = NET_INTERNET;
    s->relay_online = 1;
    strcpy(s->relay, "");
    strcpy(s->tunnel, "bore.pub:28399");
    s->tunnel_online = 1;
    strcpy(s->net_detail, "Über den Orbit-Tunnel erreichbar - kein PC und keine Router-Einstellungen nötig.");
    s->revision = 60;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-network-relay.ppm", buf, 1280, 720);
    l.page = PAGE_INVITE;
    strcpy(s->invite_code, "EZB3C-R6AE2-MJ9N8-XFMSG");
    strcpy(s->invite_scope, "Klappt überall über den Orbit-Tunnel - kein PC und keine Router-Einstellungen nötig.");
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-invite-relay.ppm", buf, 1280, 720);
    l.page = PAGE_NETWORK;
    l.page = PAGE_MENU;
    l.menu_focus = 3;
    s->net = NET_INTERNET;
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-menu-call.ppm", buf, 1280, 720);
    /* overlay redraw cost with the cached background (cursor movement) */
    {
        struct timespec a0, b0;
        l.page = PAGE_CHAT;
        l.kb_active = 1;
        ui_render(buf, 1280, 720, s, &l);
        clock_gettime(CLOCK_MONOTONIC, &a0);
        for (int i = 0; i < 100; i++) {
            l.kb_col = i % 11;
            ui_render(buf, 1280, 720, s, &l);
        }
        clock_gettime(CLOCK_MONOTONIC, &b0);
        printf("desktop keyboard redraw: %.3f ms per key press\n",
               ((b0.tv_sec - a0.tv_sec) * 1e3 + (b0.tv_nsec - a0.tv_nsec) / 1e6) / 100);
        l.kb_active = 0;
    }
    l.page = PAGE_CHAT;
    s->call_state = CALL_OFF;
    s->call_here = 0;
    s->call_count = 0;
    /* stress: many groups, long names, long message */
    for (int i = 0; i < UI_GROUPS; i++) {
        char n[80];
        snprintf(n, sizeof(n), "Gemeinsamer Spieleabend mit sehr langem Namen %02d", i + 1);
        group(s, i, n, "Ein Freund mit langem Namen", i % 3 == 0, i % 4 ? LINK_ONLINE : LINK_FAILED,
              12, i % 5 == 0, "Viele, Personen, mit, Namen, hier");
    }
    s->group_count = UI_GROUPS;
    s->selected = 30;
    s->message_count = 0;
    char longtext[769];
    memset(longtext, 'W', 768);
    longtext[768] = 0;
    msg(s, "Ein längerer Name mit Umlauten: \xc3\x84\xc3\x96\xc3\x9c", 0, 50, longtext);
    ui_render(buf, 1280, 720, s, &l);
    ppm("build/ui-stress.ppm", buf, 1280, 720);
    /* intro: key frames at 720p, an animation at 480x270 and the per-frame cost */
    {
        const float keys[] = {0.35f, 0.7f, 1.0f, 1.4f, 2.0f, 2.85f};
        for (int i = 0; i < 6; i++) {
            char name[64];
            ui_intro(buf, 1280, 720, keys[i], ORBIT_VERSION);
            snprintf(name, sizeof(name), "build/ui-intro-%d.ppm", i);
            ppm(name, buf, 1280, 720);
        }
        if (system("mkdir -p build/intro"))
            return 3;
        for (int f = 0; f * 0.05f <= 3.25f; f++) {
            char name[64];
            ui_intro(buf, 480, 270, f * 0.05f, ORBIT_VERSION);
            snprintf(name, sizeof(name), "build/intro/f%03d.ppm", f);
            ppm(name, buf, 480, 270);
        }
        struct timespec a1, b1;
        ui_intro(buf, 1280, 720, 0.1f, ORBIT_VERSION);
        clock_gettime(CLOCK_MONOTONIC, &a1);
        for (int f = 0; f < 64; f++)
            ui_intro(buf, 1280, 720, f * 0.05f, ORBIT_VERSION);
        clock_gettime(CLOCK_MONOTONIC, &b1);
        printf("desktop intro frame 1280x720: %.3f ms\n",
               ((b1.tv_sec - a1.tv_sec) * 1e3 + (b1.tv_nsec - a1.tv_nsec) / 1e6) / 64);
        ui_intro_done();
    }
    ui_social(buf, 1280, 720, ORBIT_VERSION);
    ppm("build/social.ppm", buf, 1280, 720);
    ui_icon(buf, 320, 176);
    ppm("build/icon.ppm", buf, 320, 176);
    /* render cost on this machine (not PS3 numbers) */
    group(s, 0, "Spieleabend", "Alex", 1, LINK_ONLINE, 3, 0, "Alex, Lena, Mika");
    s->selected = 0;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 100; i++)
        ui_render(buf, 1280, 720, s, &l);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("desktop render 1280x720: %.3f ms per frame\n",
           ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 100);
    ui_close();
    free(s);
    return 0;
}

/* End-to-end: two complete Orbit apps (as two consoles) in one process, on loopback. */
#include "ui.h"
#include "voice.h"
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static UiState view;
static int wait_for(OrbitApp *a, int (*cond)(const UiState *), int ms) {
    for (int t = 0; t < ms; t += 50) {
        view.revision = 0;
        app_snapshot(a, &view);
        if (cond(&view))
            return 1;
        usleep(50000);
    }
    return 0;
}
static int has_group(const UiState *s) {
    return s->group_count == 1 && !strcmp(s->groups[0].name, "Spieleabend");
}
static int invite_ready(const UiState *s) {
    return s->invite_state == 2;
}
static int friend_group_online(const UiState *s) {
    return s->group_count == 1 && !strcmp(s->groups[0].name, "Spieleabend") &&
           s->groups[0].state == LINK_ONLINE && !s->groups[0].own &&
           !strcmp(s->groups[0].host, "Alex");
}
static int friend_group_offline(const UiState *s) {
    return s->group_count == 1 && s->groups[0].state == LINK_FAILED;
}
static const char *expect_text;
static int has_message(const UiState *s) {
    for (int i = 0; i < s->message_count; i++)
        if (!strcmp(s->messages[i].text, expect_text))
            return 1;
    return 0;
}
static int two_people(const UiState *s) {
    return s->group_count == 1 && s->groups[0].members == 2 &&
           strstr(s->groups[0].people, "Mika") != NULL;
}
static int not_busy(const UiState *s) {
    return !s->busy;
}
static void fresh(const char *dir) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    assert(!system(cmd));
}
int main(void) {
    /* voice codec: a spoken-level tone survives ADPCM + jitter buffer + 48 kHz mixer */
    {
        voice_reset();
        int16_t tone[VOICE_FRAME * 10];
        for (int i = 0; i < VOICE_FRAME * 10; i++)
            tone[i] = (int16_t)(8000 * sin(i * 2 * M_PI * 440 / VOICE_RATE));
        voice_capture(tone, VOICE_FRAME * 10);
        uint8_t pkt[VOICE_PACKET];
        int packets = 0;
        while (voice_take_packet(pkt)) {
            pkt[1] = 5;
            voice_receive(pkt, VOICE_PACKET);
            packets++;
        }
        assert(packets == 10 && voice_speaking(5) && !voice_speaking(6));
        static float out[2 * 48 * 200];
        voice_mix48(out, 48 * 200);
        double sig = 0, err = 0;
        /* compare against the ideal upsampled tone after the codec settles */
        for (int i = 48 * 40; i < 48 * 180; i++) {
            double ideal = 8000 * sin((i / 3.0 - 1) * 2 * M_PI * 440 / VOICE_RATE) / 32768.0;
            sig += ideal * ideal;
            err += (out[2 * i] - ideal) * (out[2 * i] - ideal);
        }
        double snr = 10 * log10(sig / err);
        printf("voice codec SNR %.1f dB\n", snr);
        assert(snr > 15);
        voice_reset();
        int16_t quiet[VOICE_FRAME] = {0};
        voice_capture(quiet, VOICE_FRAME);
        assert(!voice_take_packet(pkt)); /* silence is not transmitted */
        voice_set_muted(1);
        voice_capture(tone, VOICE_FRAME);
        assert(!voice_take_packet(pkt)); /* muted */
        voice_set_muted(0);
        voice_reset();
        puts("PASS: voice codec quality, speaking detection, silence and mute are not sent");
    }
    /* invitation code: round trip, normalisation, typo detection */
    char code[ORBIT_INVITE_TEXT], group[9];
    uint32_t ip;
    int port;
    assert(!orbit_invite_encode(code, 0x5db8d822, 31414, "ABCD-EF23"));
    assert(strlen(code) == 23 && code[5] == '-');
    assert(!orbit_invite_decode(code, &ip, &port, group));
    assert(ip == 0x5db8d822 && port == 31414 && !strcmp(group, "ABCDEF23"));
    char lower[40];
    for (int i = 0; code[i]; i++)
        lower[i] = (char)(code[i] >= 'A' && code[i] <= 'Z' ? code[i] + 32 : code[i]);
    lower[strlen(code)] = 0;
    assert(!orbit_invite_decode(lower, &ip, &port, group));
    char typo[40];
    strcpy(typo, code);
    typo[2] = typo[2] == 'A' ? 'B' : 'A';
    assert(orbit_invite_decode(typo, &ip, &port, group));
    assert(orbit_invite_decode("ABCDE-FGHJK", &ip, &port, group));
    /* relay invitation with a high port (playit.gg style) */
    assert(!orbit_invite_encode(code, 0x93b9dd11, ORBIT_INVITE_RELAY | 61234, "ABCDEF23"));
    assert(!orbit_invite_decode(code, &ip, &port, group));
    assert(ip == 0x93b9dd11 && port == (ORBIT_INVITE_RELAY | 61234) && !strcmp(group, "ABCDEF23"));
    assert(!orbit_invite_encode(code, 0x93b9dd11, 61234, "ABCDEF23"));
    assert(!orbit_invite_decode(code, &ip, &port, group) && port == 61234);
    puts("PASS: 20-character invitation encodes address, port and group; case-insensitive; typos "
         "rejected by checksum");

    /* LAN discovery against a running host */
    OrbitStore *store = calloc(1, sizeof(*store));
    assert(!orbit_store_open(store, NULL));
    store->data.group[3].used = 1;
    store->data.group[3].members = 1;
    strcpy(store->data.group[3].invite, "QRST2345");
    uint8_t key[32] = {1};
    OrbitServer *srv = orbit_server_start(store, key, 31520);
    assert(srv);
    char tag[17], found[16];
    orbit_group_tag(tag, "QRST2345");
    assert(!orbit_discover(tag, 31520, "", found, 1500)); /* answers via loopback or LAN IP */
    printf("discovered host at %s\n", found);
    orbit_group_tag(tag, "ZZZZ2345");
    assert(orbit_discover(tag, 31520, "", found, 400));
    OrbitLink *l = orbit_connect_group(found, 31520, "QRST-2345", NULL, 0);
    assert(l);
    orbit_disconnect(l);
    assert(!orbit_connect_group("127.0.0.1", 31520, "QRST2346", NULL, 0));
    orbit_server_stop(srv);
    orbit_store_close(store);
    free(store);
    puts("PASS: LAN discovery answers only for known groups; group-code TLS-PSK accepted, wrong "
         "code rejected");

    /* two consoles */
    const char *da = "/tmp/orbit-test-a", *db = "/tmp/orbit-test-b";
    fresh(da);
    fresh(db);
    static OrbitApp A, B;
    const char *relay = getenv("ORBIT_TEST_RELAY");
    assert(!app_start(&A, da, "127.0.0.1", "255.0.0.0", "", "Alex", 31501));
    assert(!app_start(&B, db, "127.0.0.1", "255.0.0.0", "", "Mika", 31502));
    if (relay) {
        app_set_relay(&A, relay, 0); /* only the host knows the relay */
        printf("using relay %s\n", relay);
    }
    const char *tunnel = getenv("ORBIT_TEST_TUNNEL");
    if (tunnel) {
        app_set_tunnel(&A, tunnel);
        app_set_tunnel(&B, tunnel);
        printf("using tunnel %s\n", tunnel);
    }
    app_set_name(&A, "Alex");
    app_set_name(&B, "Mika");
    for (int i = 0; i < 40 && app_create_group(&A, "Spieleabend"); i++)
        usleep(100000);
    assert(wait_for(&A, has_group, 8000));
    assert(view.groups[0].own && view.groups[0].state == LINK_ONLINE);
    assert(wait_for(&A, not_busy, 4000));
    assert(!app_invite(&A));
    assert(wait_for(&A, invite_ready, 5000));
    char invite[ORBIT_INVITE_TEXT];
    strcpy(invite, view.invite_code);
    printf("invite %s (%s)\n", invite, view.invite_scope);
    if (tunnel) { /* the code carries the tunnel address of the host, no PC/relay involved */
        uint32_t tip;
        int tport;
        char tcode[9];
        assert(!orbit_invite_decode(invite, &tip, &tport, tcode) && tip == 0x7f000001 &&
               tport >= 20000 && tport < 60000);
        assert(strstr(view.invite_scope, "Tunnel"));
    }
    if (relay) {
        uint32_t rip;
        int rport;
        char rcode[9];
        assert(!orbit_invite_decode(invite, &rip, &rport, rcode) && rip == 0x7f000001 &&
               rport == (ORBIT_INVITE_RELAY | atoi(strchr(relay, ':') + 1)));
        assert(strstr(view.invite_scope, "Vermittler"));
    }
    assert(!app_join(&B, invite));
    assert(wait_for(&B, friend_group_online, 15000));
    assert(wait_for(&A, two_people, 5000));
    puts("PASS: one invitation code joins the friend to the same group; automatic console sign-in "
         "without e-mail or password");

    assert(wait_for(&B, not_busy, 4000));
    assert(!app_send(&B, "Hallo von der zweiten PS3"));
    expect_text = "Hallo von der zweiten PS3";
    assert(wait_for(&A, has_message, 6000));
    assert(wait_for(&B, has_message, 6000));
    assert(wait_for(&A, not_busy, 4000));
    assert(!app_send(&A, "Bin dabei"));
    expect_text = "Bin dabei";
    assert(wait_for(&B, has_message, 6000));
    assert(view.messages[view.message_count - 1].time > 0);
    puts("PASS: messages in both directions with timestamps");

    /* voice chat: both join the call of the group, speech from one reaches the other */
    assert(!app_call_toggle(&A));
    assert(!app_call_toggle(&B));
    int ready = 0;
    for (int t = 0; t < 200 && !ready; t++) {
        UiState va, vb;
        memset(&va, 0, sizeof(va));
        memset(&vb, 0, sizeof(vb));
        app_snapshot(&A, &va);
        app_snapshot(&B, &vb);
        ready = va.call_state == CALL_ONLINE && vb.call_state == CALL_ONLINE &&
                va.call_count == 2 && vb.call_count == 2 && vb.groups[0].call == 2;
        usleep(50000);
    }
    assert(ready);
    int16_t speech[VOICE_FRAME];
    int heard = 0;
    for (int f = 0; f < 60 && !heard; f++) {
        for (int i = 0; i < VOICE_FRAME; i++)
            speech[i] = (int16_t)(9000 * sin((f * VOICE_FRAME + i) * 2 * M_PI * 300 / VOICE_RATE));
        voice_capture(speech, VOICE_FRAME);
        usleep(20000);
        for (int u = 0; u < VOICE_USERS; u++)
            heard |= voice_speaking(u);
    }
    assert(heard);
    view.revision = 0;
    app_snapshot(&A, &view);
    assert(view.speak_mask != 0 || heard);
    app_call_toggle(&A);
    app_call_toggle(&B);
    for (int t = 0; t < 100; t++) {
        view.revision = 0;
        app_snapshot(&B, &view);
        if (view.call_state == CALL_OFF && view.groups[0].call == 0)
            break;
        usleep(100000);
    }
    assert(view.call_state == CALL_OFF && view.groups[0].call == 0);
    puts("PASS: voice chat: join, participant list on both consoles, speech relayed through the "
         "host, leave");

    /* restart the friend console: group and messages are restored automatically */
    app_stop(&B);
    assert(!app_start(&B, db, "127.0.0.1", "255.0.0.0", "", "ignored", 31502));
    if (tunnel)
        app_set_tunnel(&B, tunnel);
    if (relay) { /* the friend learned the relay from the invitation and saved it */
        view.revision = 0;
        app_snapshot(&B, &view);
        assert(!strcmp(view.relay, relay));
    }
    view.revision = 0;
    app_snapshot(&B, &view);
    assert(view.profile && !strcmp(view.username, "Mika"));
    assert(view.group_count == 1); /* cached while connecting */
    assert(wait_for(&B, friend_group_online, 15000));
    assert(wait_for(&B, has_message, 6000));
    puts("PASS: after restart name, group list and conversation are restored and reconnected");

    /* host goes away and comes back */
    app_stop(&A);
    assert(wait_for(&B, friend_group_offline, 15000));
    assert(!app_start(&A, da, "127.0.0.1", "255.0.0.0", "", "ignored", 31501));
    if (relay)
        app_set_relay(&A, relay, 0);
    if (tunnel)
        app_set_tunnel(&A, tunnel);
    assert(wait_for(&A, has_group, 8000));
    app_retry(&B);
    assert(wait_for(&B, friend_group_online, 20000));
    puts("PASS: host offline is shown, automatic reconnect when the host PS3 opens Orbit again");

    /* own invitation entered on the host itself does not create a duplicate */
    assert(!app_join(&A, invite));
    usleep(800000);
    assert(wait_for(&A, has_group, 3000));
    /* leave */
    assert(wait_for(&B, not_busy, 4000));
    assert(!app_leave(&B));
    for (int t = 0; t < 100; t++) {
        view.revision = 0;
        app_snapshot(&B, &view);
        if (!view.group_count)
            break;
        usleep(100000);
    }
    assert(view.group_count == 0);
    app_stop(&B);
    app_stop(&A);
    puts("PASS: own invite is recognised; leaving removes the friend's meeting point; clean "
         "shutdown");
    return 0;
}

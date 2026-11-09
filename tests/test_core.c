#include "mic_probe.h"
#include "orbit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static cJSON *request(OrbitStore *s, OrbitSession *a, const char *json, int expected) {
    cJSON *q = cJSON_Parse(json);
    assert(q);
    cJSON *r = orbit_request(s, a, q);
    cJSON_Delete(q);
    assert(r);
    int good = cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"));
    if (good != expected) {
        char *x = cJSON_Print(r);
        fprintf(stderr, "Unexpected response: %s\n", x);
        free(x);
        abort();
    }
    return r;
}
#define ASK(s, a, q, ok) cJSON_Delete(request(s, a, q, ok))
int main(void) {
    const uint8_t le[] = {0, 0, 0, 128, 255, 127, 9}, be[] = {0, 0, 128, 0, 127, 255, 9};
    assert(orbit_pcm_peak16(le, sizeof(le), 0) == 32768);
    assert(orbit_pcm_peak16(be, sizeof(be), 1) == 32768);
    assert(orbit_pcm_peak16(le, 2, 0) == 0);
    assert(orbit_pcm_peak16(be + 4, 2, 1) == 32767);
    assert(orbit_pcm_peak16(be, 1, 1) == 0);
    puts("PASS: microphone PCM peak decoding for both byte orders, silence, signed limits and "
         "partial sample");
    char path[128];
    snprintf(path, sizeof(path), "/tmp/orbit-test-%ld.json", (long)getpid());
    OrbitStore *s = calloc(1, sizeof(*s));
    assert(!orbit_store_open(s, path));
    OrbitSession a = {-1, 0}, b = {-1, 0}, c = {-1, 0};
    ASK(s, &a, "{\"op\":\"groups\"}", 0);
    ASK(s, &a,
        "{\"op\":\"register\",\"email\":\"A@example.com\",\"name\":\"Anna\",\"password\":\"long "
        "password 123\"}",
        1);
    ASK(s, &b,
        "{\"op\":\"register\",\"email\":\"b@example.com\",\"name\":\"Ben\",\"password\":\"long "
        "password 456\"}",
        1);
    ASK(s, &c,
        "{\"op\":\"register\",\"email\":\"a@example.com\",\"name\":\"duplicate\",\"password\":"
        "\"long password 456\"}",
        0);
    ASK(s, &c, "{\"op\":\"login\",\"email\":\"a@example.com\",\"password\":\"incorrect password\"}",
        0);
    assert(c.user == -1);
    ASK(s, &c, "{\"op\":\"login\",\"email\":\"a@example.com\",\"password\":\"long password 123\"}",
        1);
    assert(c.user == a.user);
    ASK(s, &a, "{\"op\":\"create\",\"name\":\"Spieleabend\"}", 1);
    ASK(s, &b, "{\"op\":\"history\",\"group\":0}", 0);
    ASK(s, &b, "{\"op\":\"send\",\"group\":0,\"text\":\"denied\"}", 0);
    cJSON *r = request(s, &a, "{\"op\":\"invite\",\"group\":0}", 1);
    char code[33];
    strcpy(code, orbit_string(r, "invite"));
    cJSON_Delete(r);
    assert(strlen(code) == 8);
    char q[256];
    snprintf(q, sizeof(q), "{\"op\":\"join\",\"invite\":\"%.4s-%s\"}", code, code + 4);
    ASK(s, &b, q, 1);
    ASK(s, &b, "{\"op\":\"invite\",\"group\":0}", 1); /* 0.7: every member can invite */
    ASK(s, &a, "{\"op\":\"send\",\"group\":0,\"text\":\"Gruesse aus der PS3!\"}", 1);
    ASK(s, &a, "{\"op\":\"send\",\"group\":0,\"text\":\"spam\"}", 0);
    r = request(s, &b, "{\"op\":\"history\",\"group\":0}", 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(r, "messages")) == 1);
    cJSON_Delete(r);
    ASK(s, &b, "{\"op\":\"delete\",\"group\":0}", 0);
    ASK(s, &b, "{\"op\":\"leave\",\"group\":0}", 1);
    ASK(s, &b, "{\"op\":\"history\",\"group\":0}", 0);
    ASK(s, &a, "{\"op\":\"rotate\",\"group\":0}", 1);
    ASK(s, &b, q, 0);
    ASK(s, &b, "{\"op\":\"invite_user\",\"group\":0,\"email\":\"a@example.com\"}", 0);
    ASK(s, &a, "{\"op\":\"invite_user\",\"group\":0,\"email\":\"B@example.com\"}", 1);
    r = request(s, &b, "{\"op\":\"invitations\"}", 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(r, "invitations")) == 1);
    cJSON_Delete(r);
    ASK(s, &b, "{\"op\":\"history\",\"group\":0}", 0);
    ASK(s, &a, "{\"op\":\"respond_invite\",\"group\":0,\"accept\":true}", 0);
    ASK(s, &b, "{\"op\":\"respond_invite\",\"group\":0,\"accept\":false}", 1);
    ASK(s, &b, "{\"op\":\"history\",\"group\":0}", 0);
    ASK(s, &a, "{\"op\":\"invite_user\",\"group\":0,\"email\":\"b@example.com\"}", 1);
    assert(!orbit_text_valid("\xc0\xaf", 1, 10));
    assert(!orbit_text_valid("\xed\xa0\x80", 1, 10));
    assert(orbit_text_valid("Hallo \xf0\x9f\x8e\xae", 1, 20));
    orbit_store_close(s);
    assert(!orbit_store_open(s, path));
    assert(s->data.users == 2 && s->data.message[1].id == 1);
    assert(s->data.group[0].pending == (UINT32_C(1) << b.user));
    ASK(s, &b, "{\"op\":\"respond_invite\",\"group\":0,\"accept\":true}", 1);
    ASK(s, &b, "{\"op\":\"history\",\"group\":0}", 1);
    ASK(s, &b, "{\"op\":\"respond_invite\",\"group\":0,\"accept\":true}", 0);
    ASK(s, &a, "{\"op\":\"delete\",\"group\":0}", 1);
    assert(!s->data.message[1].id);
    /* Simulate power loss after the old snapshot moved but before promotion. */
    orbit_store_close(s);
    char backup[160];
    snprintf(backup, sizeof(backup), "%s.bak", path);
    unlink(backup);
    assert(!rename(path, backup));
    assert(!orbit_store_open(s, path));
    assert(s->data.users == 2 && !s->data.group[0].used);
    /* Failed writes roll back account/group/session mutations. */
    strcpy(s->path, "/no-such-orbit-directory/data.json");
    ASK(s, &a, "{\"op\":\"create\",\"name\":\"Rollback\"}", 0);
    assert(!s->data.group[0].used);
    orbit_store_close(s);
    free(s);
    unlink(path);
    unlink(backup);
    puts("PASS: account hashing, duplicate identity, authentication, group access, invite "
         "rotation, invitation authorization/accept/reject/persistence, message limits, "
         "persistence, crash recovery, write rollback, UTF-8");
    s = calloc(1, sizeof(*s));
    assert(!orbit_store_open(s, path));
    OrbitSession legacy = {-1, 0};
    ASK(s, &legacy,
        "{\"op\":\"register\",\"email\":\"legacy@example.com\",\"name\":\"Legacy\",\"password\":"
        "\"a long password\"}",
        1);
    ASK(s, &legacy, "{\"op\":\"create\",\"name\":\"Old group\"}", 1);
    strcpy(s->data.group[0].invite, "0123456789abcdef0123456789abcdef");
    ASK(s, &legacy, "{\"op\":\"send\",\"group\":0,\"text\":\"Persist legacy code\"}", 1);
    orbit_store_close(s);
    assert(!orbit_store_open(s, path));
    assert(!strcmp(s->data.group[0].invite, "0123456789abcdef0123456789abcdef"));
    ASK(s, &legacy, "{\"op\":\"join\",\"invite\":\"0123456789ABCDEF0123456789ABCDEF\"}", 1);
    for (int i = 1; i < 12; i++)
        ASK(s, &legacy, "{\"op\":\"join\",\"invite\":\"WRONG\"}", 0);
    r = request(s, &legacy, "{\"op\":\"join\",\"invite\":\"0123456789abcdef0123456789abcdef\"}", 0);
    assert(strstr(orbit_string(r, "error"), "Minute"));
    cJSON_Delete(r);
    s->join_window[legacy.user] = orbit_millis() - 60001;
    ASK(s, &legacy, "{\"op\":\"join\",\"invite\":\"0123456789abcdef0123456789abcdef\"}", 1);
    orbit_store_close(s);
    free(s);
    unlink(path);
    unlink(backup);
    char normalized[33];
    assert(!orbit_group_code(normalized, "abcd-ef23") && !strcmp(normalized, "ABCDEF23"));
    assert(orbit_group_code(normalized, "ABCD-IO01"));
    assert(orbit_group_code(normalized, "ABCDEFG"));
    puts("PASS: eight-character group codes, separated/case-insensitive entry, legacy code "
         "compatibility, per-account join throttling/reset");
    char host[ORBIT_HOST];
    int port;
    uint8_t key[32];
    assert(!orbit_parse_endpoint("example.com:31415", host, &port) && port == 31415 &&
           !strcmp(host, "example.com"));
    assert(!orbit_parse_endpoint("192.168.1.2", host, &port) && port == 31413);
    assert(orbit_parse_endpoint("example.com:999999", host, &port));
    assert(orbit_parse_endpoint("bad host:31413", host, &port));
    assert(orbit_parse_endpoint("host:0", host, &port));
    assert(!orbit_parse_invite("orbit://example.com:31415/"
                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                               host, &port, key));
    assert(orbit_parse_invite("orbit://example.com:31415/0123", host, &port, key));
    puts("PASS: DNS/IPv4 invite parsing, custom ports, malformed endpoint/key rejection");
    return 0;
}

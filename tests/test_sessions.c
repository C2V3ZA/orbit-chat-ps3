#include "ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static cJSON *ask(OrbitStore *s, OrbitSession *session, const char *text, int success) {
    cJSON *q = cJSON_Parse(text), *r = orbit_request(s, session, q);
    cJSON_Delete(q);
    if (!r || cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")) != success) {
        fprintf(stderr, "Failed: %s\n", text);
        abort();
    }
    return r;
}
#define ASK(s, u, q, ok) cJSON_Delete(ask(s, u, q, ok))
int main(void) {
    char dir[] = "/tmp/orbit-sessions-XXXXXX", path[512], q[512];
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/data.json", dir);
    OrbitStore *s = calloc(1, sizeof(*s));
    assert(!orbit_store_open(s, path));
    OrbitSession owner = {-1, 0}, friend = {-1, 0}, resumed = {-1, 0};
    ASK(s, &owner,
        "{\"op\":\"register\",\"name\":\"Owner\",\"email\":\"owner@example.com\",\"password\":"
        "\"owner password\"}",
        1);
    ASK(s, &owner, "{\"op\":\"create\",\"name\":\"Shared group\"}", 1);
    cJSON *r =
        ask(s, &owner, "{\"op\":\"invite_user\",\"group\":0,\"email\":\"Friend@example.com\"}", 1);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r, "waiting_registration")));
    cJSON_Delete(r);
    r = ask(s, &owner, "{\"op\":\"remember\"}", 1);
    char token[65];
    snprintf(token, sizeof(token), "%s", orbit_string(r, "token"));
    assert(strlen(token) == 64);
    cJSON_Delete(r);
    orbit_store_close(s);
    assert(!orbit_store_open(s, path));
    snprintf(q, sizeof(q), "{\"op\":\"resume\",\"email\":\"owner@example.com\",\"token\":\"%s\"}",
             token);
    ASK(s, &resumed, q, 1);
    assert(resumed.user == owner.user);
    ASK(s, &friend,
        "{\"op\":\"resume\",\"email\":\"owner@example.com\",\"token\":"
        "\"0000000000000000000000000000000000000000000000000000000000000000\"}",
        0);
    ASK(s, &friend,
        "{\"op\":\"register\",\"name\":\"Friend\",\"email\":\"friend@example.com\",\"password\":"
        "\"friend password\"}",
        1);
    r = ask(s, &friend, "{\"op\":\"invitations\"}", 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(r, "invitations")) == 1);
    cJSON_Delete(r);
    ASK(s, &friend, "{\"op\":\"history\",\"group\":0}", 0);
    ASK(s, &friend, "{\"op\":\"respond_invite\",\"group\":0,\"accept\":true}", 1);
    ASK(s, &friend, "{\"op\":\"history\",\"group\":0}", 1);
    /* Tokens are account-scoped and explicit sign-out revokes only this device. */
    snprintf(q, sizeof(q), "{\"op\":\"resume\",\"email\":\"friend@example.com\",\"token\":\"%s\"}",
             token);
    OrbitSession wrong = {-1, 0};
    ASK(s, &wrong, q, 0);
    snprintf(q, sizeof(q), "{\"op\":\"forget_token\",\"token\":\"%s\"}", token);
    ASK(s, &resumed, q, 1);
    snprintf(q, sizeof(q), "{\"op\":\"resume\",\"email\":\"owner@example.com\",\"token\":\"%s\"}",
             token);
    ASK(s, &resumed, q, 0);
    /* Bounded device list evicts the oldest token. */
    for (int i = 0; i < 5; i++) {
        r = ask(s, &owner, "{\"op\":\"remember\"}", 1);
        if (!i)
            strcpy(token, orbit_string(r, "token"));
        cJSON_Delete(r);
    }
    snprintf(q, sizeof(q), "{\"op\":\"resume\",\"email\":\"owner@example.com\",\"token\":\"%s\"}",
             token);
    ASK(s, &resumed, q, 0);
    orbit_store_close(s);
    free(s);
    OrbitRemember record = {0}, loaded = {0};
    record.valid = 1;
    record.hosting = 0;
    strcpy(record.email, "friend@example.com");
    strcpy(record.token, token);
    strcpy(
        record.invite,
        "orbit://127.0.0.1:31415/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    assert(!orbit_remember_write(dir, &record));
    assert(!orbit_remember_read(dir, &loaded));
    assert(!memcmp(&record, &loaded, sizeof(record)));
    snprintf(path, sizeof(path), "%s/session.json", dir);
    FILE *f = fopen(path, "rb");
    assert(f);
    char text[1024] = {0};
    assert(fread(text, 1, 1023, f) > 0);
    fclose(f);
    assert(!strstr(text, "password"));
    char backup[512];
    snprintf(backup, sizeof(backup), "%s/session.bak", dir);
    assert(!rename(path, backup));
    assert(!orbit_remember_read(dir, &loaded));
    orbit_remember_clear(dir, &loaded);
    assert(orbit_remember_read(dir, &loaded));
    snprintf(path, sizeof(path), "%s/data.json", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/data.json.bak", dir);
    unlink(path);
    rmdir(dir);
    char host[ORBIT_HOST], group[33];
    int port;
    uint8_t key[32];
    assert(!orbit_parse_join(
        "orbit://example.com:31413/"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?g=abcd-ef23",
        host, &port, key, group));
    assert(!strcmp(group, "ABCDEF23"));
    assert(
        orbit_parse_join("orbit://example.com/"
                         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?g=bad",
                         host, &port, key, group));
    puts("PASS: invitations before registration persist and require acceptance; remembered "
         "sessions survive restart, reject wrong/revoked/cross-account tokens, bound devices, "
         "recover local record without password; combined invitation parsing");
    return 0;
}

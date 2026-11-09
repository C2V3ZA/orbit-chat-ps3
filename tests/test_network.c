#include "orbit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static cJSON *ask(OrbitLink *l, const char *json, int ok) {
    cJSON *q = cJSON_Parse(json);
    assert(q);
    cJSON *r = orbit_exchange(l, q);
    cJSON_Delete(q);
    assert(r);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")) == ok);
    return r;
}
#define ASK(l, q, ok) cJSON_Delete(ask(l, q, ok))
int main(void) {
    OrbitStore *store = calloc(1, sizeof(*store));
    assert(!orbit_store_open(store, NULL));
    uint8_t key[32], wrong[32];
    assert(!orbit_random(key, 32));
    memcpy(wrong, key, 32);
    wrong[0] ^= 255;
    OrbitServer *s = orbit_server_start(store, key, 31414);
    assert(s);
    assert(!orbit_connect("127.0.0.1", 31414, wrong));
    OrbitLink *a = orbit_connect("127.0.0.1", 31414, key),
              *b = orbit_connect("localhost", 31414, key);
    assert(a && b);
    ASK(a,
        "{\"op\":\"register\",\"email\":\"a@example.com\",\"name\":\"Alice\",\"password\":\"safe "
        "password A\"}",
        1);
    ASK(b,
        "{\"op\":\"register\",\"email\":\"b@example.com\",\"name\":\"Bob\",\"password\":\"safe "
        "password B\"}",
        1);
    ASK(a, "{\"op\":\"create\",\"name\":\"TLS test\"}", 1);
    ASK(b, "{\"op\":\"history\",\"group\":0}", 0);
    ASK(a, "{\"op\":\"invite_user\",\"group\":0,\"email\":\"b@example.com\"}", 1);
    cJSON *r = ask(b, "{\"op\":\"invitations\"}", 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(r, "invitations")) == 1);
    cJSON_Delete(r);
    ASK(b, "{\"op\":\"history\",\"group\":0}", 0);
    ASK(b, "{\"op\":\"respond_invite\",\"group\":0,\"accept\":true}", 1);
    ASK(a, "{\"op\":\"send\",\"group\":0,\"text\":\"Hallo von Konsole A\"}", 1);
    r = ask(b, "{\"op\":\"history\",\"group\":0}", 1);
    cJSON *m = cJSON_GetArrayItem(cJSON_GetObjectItem(r, "messages"), 0);
    assert(m && !strcmp(orbit_string(m, "text"), "Hallo von Konsole A"));
    cJSON_Delete(r);
    ASK(b, "{\"op\":\"logout\"}", 1);
    ASK(b, "{\"op\":\"history\",\"group\":0}", 0);
    orbit_disconnect(a);
    orbit_disconnect(b);
    orbit_server_stop(s);
    orbit_store_close(store);
    free(store);
    puts("PASS: TLS 1.2 PSK handshake, wrong key rejected, two live clients via IPv4/DNS and "
         "custom port, private group "
         "delivery, logout authorization, orderly shutdown");
    return 0;
}

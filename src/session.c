#include "monocypher.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void paths(const char *dir, char path[512], char tmp[512], char bak[512]) {
    snprintf(path, 512, "%s/session.json", dir);
    snprintf(tmp, 512, "%s/session.tmp", dir);
    snprintf(bak, 512, "%s/session.bak", dir);
}
static int valid(const OrbitRemember *r) {
    char host[ORBIT_HOST], group[33];
    int port;
    uint8_t key[32], token[32];
    return r->valid && (r->hosting == 0 || r->hosting == 1) && strchr(r->email, '@') &&
           orbit_text_valid(r->email, 3, 96) &&
           !orbit_parse_join(r->invite, host, &port, key, group) &&
           !orbit_unhex(token, r->token, 32);
}
int orbit_remember_read(const char *dir, OrbitRemember *r) {
    memset(r, 0, sizeof(*r));
    char path[512], tmp[512], bak[512];
    paths(dir, path, tmp, bak);
    FILE *f = fopen(path, "rb");
    if (!f)
        f = fopen(bak, "rb");
    if (!f)
        return -1;
    char text[1024];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    int end = fgetc(f);
    fclose(f);
    text[n] = 0;
    cJSON *j = end == EOF ? cJSON_ParseWithOpts(text, NULL, 1) : NULL;
    crypto_wipe(text, sizeof(text));
    if (!j)
        return -1;
    cJSON *h = cJSON_GetObjectItem(j, "host"), *version = cJSON_GetObjectItem(j, "version");
    r->valid = cJSON_IsBool(h) && cJSON_IsNumber(version) && version->valueint == 1;
    r->hosting = cJSON_IsTrue(h);
    const char *invite = orbit_string(j, "invite"), *email = orbit_string(j, "email"),
               *token = orbit_string(j, "token");
    if (strlen(invite) >= sizeof(r->invite) || strlen(email) >= sizeof(r->email) ||
        strlen(token) != 64)
        r->valid = 0;
    snprintf(r->invite, sizeof(r->invite), "%s", invite);
    snprintf(r->email, sizeof(r->email), "%s", email);
    snprintf(r->token, sizeof(r->token), "%s", token);
    cJSON *secret = cJSON_GetObjectItem(j, "token");
    if (cJSON_IsString(secret))
        crypto_wipe(secret->valuestring, strlen(secret->valuestring));
    cJSON_Delete(j);
    if (!valid(r)) {
        crypto_wipe(r, sizeof(*r));
        return -1;
    }
    return 0;
}
int orbit_remember_write(const char *dir, const OrbitRemember *r) {
    if (!valid(r))
        return -1;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "version", 1);
    cJSON_AddBoolToObject(j, "host", r->hosting);
    cJSON_AddStringToObject(j, "invite", r->invite);
    cJSON_AddStringToObject(j, "email", r->email);
    cJSON_AddStringToObject(j, "token", r->token);
    char *text = cJSON_PrintUnformatted(j);
    cJSON *secret = cJSON_GetObjectItem(j, "token");
    if (cJSON_IsString(secret))
        crypto_wipe(secret->valuestring, strlen(secret->valuestring));
    cJSON_Delete(j);
    if (!text)
        return -1;
    char path[512], tmp[512], bak[512];
    paths(dir, path, tmp, bak);
    FILE *f = fopen(tmp, "wb");
    size_t n = strlen(text);
    int ok = 0;
    if (f) {
        ok = fwrite(text, 1, n, f) == n;
        if (fflush(f) || fsync(fileno(f)))
            ok = 0;
        if (fclose(f))
            ok = 0;
        chmod(tmp, 0600);
    }
    crypto_wipe(text, n);
    free(text);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    struct stat st;
    int existed = !stat(path, &st);
    if (existed) {
        unlink(bak);
        if (rename(path, bak)) {
            unlink(tmp);
            return -1;
        }
    }
    if (rename(tmp, path)) {
        if (existed)
            rename(bak, path);
        unlink(tmp);
        return -1;
    }
    unlink(bak);
    return 0;
}
void orbit_remember_clear(const char *dir, OrbitRemember *r) {
    char path[512], tmp[512], bak[512];
    paths(dir, path, tmp, bak);
    unlink(path);
    unlink(tmp);
    unlink(bak);
    crypto_wipe(r, sizeof(*r));
}

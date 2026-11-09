#include "monocypher.h"
#include "orbit.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
const char *orbit_string(const cJSON *o, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static int number(const cJSON *o, const char *key, int fallback) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && v->valuedouble == v->valueint ? v->valueint : fallback;
}
static cJSON *ok(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    return r;
}
cJSON *orbit_error(const char *text) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", text);
    return r;
}
/* Eight base32 characters: 40 random bits, with no I/O/0/1 ambiguity. */
static int new_group_code(const OrbitData *d, char out[33]) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    _Static_assert(sizeof(alphabet) == 33, "32 symbol alphabet");
    for (int attempt = 0; attempt < 16; attempt++) {
        uint8_t random[8];
        if (orbit_random(random, sizeof(random)))
            return -1;
        for (int i = 0; i < 8; i++)
            out[i] = alphabet[random[i] & 31];
        out[8] = 0;
        int duplicate = 0;
        for (int i = 0; i < ORBIT_GROUPS; i++)
            if (d->group[i].used && !strcmp(d->group[i].invite, out))
                duplicate = 1;
        if (!duplicate)
            return 0;
    }
    return -1;
}
static int password_hash(uint8_t out[32], const char *pass, const uint8_t salt[16]) {
    void *work = malloc(8 * 1024 * 1024);
    if (!work)
        return -1;
    crypto_argon2_config c = {CRYPTO_ARGON2_ID, 8192, 3, 1};
    crypto_argon2_inputs in = {(const uint8_t *)pass, salt, (uint32_t)strlen(pass), 16};
    crypto_argon2(out, 32, work, c, in, crypto_argon2_no_extras);
    crypto_wipe(work, 8 * 1024 * 1024);
    free(work);
    return 0;
}
static void normalize_email(char out[97], const char *in) {
    size_t n = strlen(in);
    if (n > 96)
        n = 96;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[n] = 0;
}
static int valid_email(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > 96)
        return 0;
    const char *a = strchr(s, '@');
    if (!a || a == s || !a[1] || strchr(a + 1, '@'))
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] <= 32 || (unsigned char)s[i] >= 127)
            return 0;
    return 1;
}
static cJSON *encode(const OrbitData *d) {
    cJSON *root = cJSON_CreateObject(), *users = cJSON_AddArrayToObject(root, "users"),
          *groups = cJSON_AddArrayToObject(root, "groups"),
          *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "next_id", d->next_id);
    for (int i = 0; i < d->users; i++) {
        const OrbitUser *u = &d->user[i];
        cJSON *v = cJSON_CreateObject();
        char salt[33], hash[65];
        orbit_hex(salt, u->salt, 16);
        orbit_hex(hash, u->hash, 32);
        cJSON_AddStringToObject(v, "email", u->email);
        cJSON_AddStringToObject(v, "name", u->name);
        cJSON_AddStringToObject(v, "salt", salt);
        cJSON_AddStringToObject(v, "hash", hash);
        cJSON *devices = cJSON_AddArrayToObject(v, "devices");
        for (int j = 0; j < 4; j++) {
            char h[65];
            orbit_hex(h, u->devices[j], 32);
            cJSON_AddItemToArray(devices, cJSON_CreateString(h));
        }
        cJSON_AddItemToArray(users, v);
    }
    for (int i = 0; i < ORBIT_GROUPS; i++)
        if (d->group[i].used) {
            const OrbitGroup *g = &d->group[i];
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "id", i);
            cJSON_AddNumberToObject(v, "owner", g->owner);
            cJSON_AddNumberToObject(v, "members", g->members);
            cJSON_AddNumberToObject(v, "pending", g->pending);
            cJSON_AddStringToObject(v, "name", g->name);
            cJSON_AddStringToObject(v, "invite", g->invite);
            cJSON_AddItemToArray(groups, v);
        }
    for (int i = 0; i < ORBIT_MESSAGES; i++)
        if (d->message[i].id) {
            const OrbitMessage *m = &d->message[i];
            cJSON *v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "id", m->id);
            cJSON_AddNumberToObject(v, "group", m->group);
            cJSON_AddNumberToObject(v, "user", m->user);
            cJSON_AddNumberToObject(v, "time", m->time);
            cJSON_AddStringToObject(v, "text", m->text);
            cJSON_AddItemToArray(msgs, v);
        }
    cJSON *pending = cJSON_AddArrayToObject(root, "pending_emails");
    for (int i = 0; i < ORBIT_PENDING_EMAILS; i++)
        if (d->pending_email[i].used) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "group", d->pending_email[i].group);
            cJSON_AddStringToObject(item, "email", d->pending_email[i].email);
            cJSON_AddItemToArray(pending, item);
        }
    return root;
}
static int save(const OrbitStore *s) {
    if (!s->path[0])
        return 0;
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->path);
    cJSON *root = encode(&s->data);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(json);
        return -1;
    }
    size_t n = strlen(json);
    int good = fwrite(json, 1, n, f) == n;
    free(json);
    if (fflush(f))
        good = 0;
    if (fsync(fileno(f)))
        good = 0;
    if (fclose(f))
        good = 0;
    chmod(tmp, 0600);
    if (!good) {
        remove(tmp);
        return -1;
    }
    /* PS3 sys_fs_rename does not replace an existing destination. Keep the last
       committed snapshot as a recovery file instead of unlinking the only copy. */
    char backup[520];
    snprintf(backup, sizeof(backup), "%s.bak", s->path);
    if (remove(backup) && errno != ENOENT)
        return -1;
    int moved = rename(s->path, backup) == 0;
    if (!moved && errno != ENOENT)
        return -1;
    if (rename(tmp, s->path)) {
        if (moved)
            rename(backup, s->path);
        return -1;
    }
    return 0;
}
int orbit_store_open(OrbitStore *s, const char *path) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mutex, NULL);
    s->data.version = 1;
    s->data.next_id = 1;
    if (!path)
        return 0;
    if (strlen(path) >= sizeof(s->path))
        return -1;
    strcpy(s->path, path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT)
            return -1;
        char backup[520];
        snprintf(backup, sizeof(backup), "%s.bak", path);
        if (!rename(backup, path))
            f = fopen(path, "rb");
        else if (errno == ENOENT)
            return save(s);
        if (!f)
            return -1;
    }
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return -1;
    }
    long n = ftell(f);
    rewind(f);
    if (n < 2 || n > 400000) {
        fclose(f);
        return -1;
    }
    char *text = calloc(1, (size_t)n + 1);
    if (!text) {
        fclose(f);
        return -1;
    }
    int readok = fread(text, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    cJSON *root = readok ? cJSON_ParseWithOpts(text, NULL, 1) : NULL;
    free(text);
    if (!root)
        return -1;
    int good = number(root, "version", 0) == 1;
    int next = number(root, "next_id", 0);
    if (next < 1)
        good = 0;
    s->data.next_id = (uint32_t)next;
    cJSON *users = cJSON_GetObjectItemCaseSensitive(root, "users"),
          *groups = cJSON_GetObjectItemCaseSensitive(root, "groups"),
          *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages"), *v;
    if (!cJSON_IsArray(users) || !cJSON_IsArray(groups) || !cJSON_IsArray(msgs) ||
        cJSON_GetArraySize(users) > ORBIT_USERS || cJSON_GetArraySize(groups) > ORBIT_GROUPS ||
        cJSON_GetArraySize(msgs) > ORBIT_MESSAGES)
        good = 0;
    if (good)
        cJSON_ArrayForEach(v, users) {
            OrbitUser *u = &s->data.user[s->data.users++];
            const char *email = orbit_string(v, "email"), *name = orbit_string(v, "name");
            if (!valid_email(email) || !orbit_text_valid(name, 1, 48) ||
                orbit_unhex(u->salt, orbit_string(v, "salt"), 16) ||
                orbit_unhex(u->hash, orbit_string(v, "hash"), 32)) {
                good = 0;
                break;
            }
            strcpy(u->email, email);
            strcpy(u->name, name);
            cJSON *devices = cJSON_GetObjectItem(v, "devices");
            if (devices) {
                if (!cJSON_IsArray(devices) || cJSON_GetArraySize(devices) != 4) {
                    good = 0;
                    break;
                }
                for (int j = 0; j < 4; j++) {
                    cJSON *t = cJSON_GetArrayItem(devices, j);
                    if (!cJSON_IsString(t) || orbit_unhex(u->devices[j], t->valuestring, 32)) {
                        good = 0;
                        break;
                    }
                }
            }
        }
    if (good)
        cJSON_ArrayForEach(v, groups) {
            int id = number(v, "id", -1), owner = number(v, "owner", -1);
            const char *name = orbit_string(v, "name"), *invite = orbit_string(v, "invite");
            char normalized[33];
            cJSON *bits = cJSON_GetObjectItemCaseSensitive(v, "members");
            double b = cJSON_IsNumber(bits) ? bits->valuedouble : -1;
            if (id < 0 || id >= ORBIT_GROUPS || s->data.group[id].used || owner < 0 ||
                owner >= s->data.users || !orbit_text_valid(name, 1, 64) ||
                orbit_group_code(normalized, invite) || b < 1 || b > UINT32_MAX ||
                b != (uint32_t)b) {
                good = 0;
                break;
            }
            OrbitGroup *g = &s->data.group[id];
            g->used = 1;
            g->owner = owner;
            g->members = (uint32_t)b;
            cJSON *pending = cJSON_GetObjectItemCaseSensitive(v, "pending");
            if (pending) {
                double p = cJSON_IsNumber(pending) ? pending->valuedouble : -1;
                if (p < 0 || p > UINT32_MAX || p != (uint32_t)p ||
                    (s->data.users < 32 && ((uint32_t)p >> s->data.users))) {
                    good = 0;
                    break;
                }
                g->pending = (uint32_t)p & ~g->members;
            }
            if (!(g->members & (UINT32_C(1) << owner)) ||
                (s->data.users < 32 && (g->members >> s->data.users))) {
                good = 0;
                break;
            }
            strcpy(g->name, name);
            strcpy(g->invite, normalized);
        }
    cJSON *pending = cJSON_GetObjectItem(root, "pending_emails");
    if (pending) {
        if (!cJSON_IsArray(pending) || cJSON_GetArraySize(pending) > ORBIT_PENDING_EMAILS)
            good = 0;
        int i = 0;
        if (good)
            cJSON_ArrayForEach(v, pending) {
                int gid = number(v, "group", -1);
                const char *email = orbit_string(v, "email");
                if (gid < 0 || gid >= ORBIT_GROUPS || !s->data.group[gid].used ||
                    !valid_email(email)) {
                    good = 0;
                    break;
                }
                s->data.pending_email[i].used = 1;
                s->data.pending_email[i].group = gid;
                normalize_email(s->data.pending_email[i++].email, email);
            }
    }
    if (good)
        cJSON_ArrayForEach(v, msgs) {
            int id = number(v, "id", 0), g = number(v, "group", -1), u = number(v, "user", -1);
            const char *body = orbit_string(v, "text");
            if (id < 1 || (uint32_t)id >= s->data.next_id || g < 0 || g >= ORBIT_GROUPS ||
                !s->data.group[g].used || u < 0 || u >= s->data.users ||
                !orbit_text_valid(body, 1, 768) || s->data.message[id % ORBIT_MESSAGES].id) {
                good = 0;
                break;
            }
            OrbitMessage *m = &s->data.message[id % ORBIT_MESSAGES];
            m->id = (uint32_t)id;
            m->group = g;
            m->user = u;
            cJSON *t = cJSON_GetObjectItemCaseSensitive(v, "time");
            if (cJSON_IsNumber(t) && t->valuedouble >= 0 && t->valuedouble < 4294967295.0)
                m->time = (uint32_t)t->valuedouble;
            strcpy(m->text, body);
        }
    cJSON_Delete(root);
    return good ? 0 : -1;
}
void orbit_store_close(OrbitStore *s) {
    pthread_mutex_destroy(&s->mutex);
    crypto_wipe(&s->data, sizeof(s->data));
}
static cJSON *dispatch(OrbitStore *s, OrbitSession *sess, const cJSON *r, int *changed) {
    OrbitData *d = &s->data;
    const char *op = orbit_string(r, "op");
    if (!strcmp(op, "hello")) {
        cJSON *v = ok();
        cJSON_AddStringToObject(v, "server", s->server_id);
        cJSON_AddNumberToObject(v, "protocol", ORBIT_PROTOCOL);
        return v;
    }
    if (!strcmp(op, "device")) {
        /* Console sign-in: a per-host device secret replaces e-mail and password. */
        uint64_t now = orbit_millis();
        if (now < s->auth_window || now - s->auth_window > 60000) {
            s->auth_window = now;
            s->auth_count = 0;
        }
        if (++s->auth_count > 30)
            return orbit_error("Zu viele Anmeldungen. Gleich erneut versuchen.");
        uint8_t raw[32], hash[32];
        const char *name = orbit_string(r, "name");
        if (orbit_unhex(raw, orbit_string(r, "token"), 32))
            return orbit_error("Konsolen-Anmeldung ungültig.");
        if (!orbit_text_valid(name, 1, 48))
            return orbit_error("Name: 1 bis 48 Zeichen.");
        crypto_blake2b_keyed(hash, 32, (const uint8_t *)"orbit-device", 12, raw, 32);
        crypto_wipe(raw, 32);
        int uid = -1;
        for (int i = 0; i < d->users && uid < 0; i++)
            for (int j = 0; j < 4; j++)
                if (!crypto_verify32(hash, d->user[i].devices[j]))
                    uid = i;
        if (uid < 0) {
            if (d->users == ORBIT_USERS) {
                crypto_wipe(hash, 32);
                return orbit_error("Dieser Treffpunkt ist voll (32 Personen).");
            }
            OrbitUser *u = &d->user[d->users];
            memset(u, 0, sizeof(*u));
            char id[13];
            orbit_hex(id, hash, 6);
            snprintf(u->email, sizeof(u->email), "ps3-%s@orbit.local", id);
            if (orbit_random(u->salt, 16) || orbit_random(u->hash, 32)) {
                crypto_wipe(hash, 32);
                return orbit_error("Zufallsquelle nicht verfügbar.");
            }
            memcpy(u->devices[0], hash, 32);
            strcpy(u->name, name);
            uid = d->users++;
            *changed = 1;
        } else if (strcmp(d->user[uid].name, name)) {
            strcpy(d->user[uid].name, name);
            *changed = 1;
        }
        crypto_wipe(hash, 32);
        sess->user = uid;
        cJSON *v = ok();
        cJSON_AddNumberToObject(v, "user", uid);
        cJSON_AddStringToObject(v, "name", d->user[uid].name);
        return v;
    }
    if (!strcmp(op, "resume")) {
        const char *email = orbit_string(r, "email");
        uint8_t raw[32], hash[32];
        if (!valid_email(email) || orbit_unhex(raw, orbit_string(r, "token"), 32))
            return orbit_error("Gespeicherte Anmeldung ungültig. Bitte neu anmelden.");
        crypto_blake2b(hash, 32, raw, 32);
        crypto_wipe(raw, 32);
        char norm[97];
        normalize_email(norm, email);
        int uid = -1;
        for (int i = 0; i < d->users; i++)
            if (!strcmp(norm, d->user[i].email))
                for (int j = 0; j < 4; j++)
                    if (!crypto_verify32(hash, d->user[i].devices[j]))
                        uid = i;
        crypto_wipe(hash, 32);
        if (uid < 0)
            return orbit_error("Gespeicherte Anmeldung widerrufen. Bitte neu anmelden.");
        sess->user = uid;
        cJSON *v = ok();
        cJSON_AddNumberToObject(v, "user", uid);
        cJSON_AddStringToObject(v, "name", d->user[uid].name);
        return v;
    }
    if (!strcmp(op, "register") || !strcmp(op, "login")) {
        uint64_t now = orbit_millis();
        if (now < s->auth_window || now - s->auth_window > 60000) {
            s->auth_window = now;
            s->auth_count = 0;
        }
        if (++s->auth_count > 12)
            return orbit_error("Zu viele Anmeldeversuche. In einer Minute erneut versuchen.");
        const char *email = orbit_string(r, "email"), *pass = orbit_string(r, "password"),
                   *name = orbit_string(r, "name");
        char norm[97];
        if (!valid_email(email) || !orbit_text_valid(pass, 10, 128))
            return orbit_error("E-Mail und Passwort prüfen (10 bis 128 Bytes).");
        normalize_email(norm, email);
        int uid = -1;
        for (int i = 0; i < d->users; i++)
            if (!strcmp(norm, d->user[i].email))
                uid = i;
        if (!strcmp(op, "register")) {
            if (uid >= 0 || d->users == ORBIT_USERS)
                return orbit_error("Konto kann nicht erstellt werden.");
            if (!orbit_text_valid(name, 1, 48))
                return orbit_error("Name: 1 bis 48 UTF-8-Bytes.");
            OrbitUser *u = &d->user[d->users];
            if (orbit_random(u->salt, 16) || password_hash(u->hash, pass, u->salt))
                return orbit_error("Sicherheitsfunktion nicht verfügbar.");
            strcpy(u->email, norm);
            strcpy(u->name, name);
            uid = d->users++;
            for (int i = 0; i < ORBIT_PENDING_EMAILS; i++) {
                OrbitPendingEmail *e = &d->pending_email[i];
                if (e->used && !strcmp(e->email, norm)) {
                    if (d->group[e->group].used)
                        d->group[e->group].pending |= UINT32_C(1) << uid;
                    memset(e, 0, sizeof(*e));
                }
            }
            *changed = 1;
        } else {
            uint8_t hash[32], dummy[16] = {0};
            int h = password_hash(hash, pass, uid >= 0 ? d->user[uid].salt : dummy);
            int valid = h == 0 && uid >= 0 && crypto_verify32(hash, d->user[uid].hash) == 0;
            crypto_wipe(hash, sizeof(hash));
            if (!valid)
                return orbit_error("Anmeldung fehlgeschlagen.");
        }
        sess->user = uid;
        cJSON *v = ok();
        cJSON_AddNumberToObject(v, "user", uid);
        cJSON_AddStringToObject(v, "name", d->user[uid].name);
        return v;
    }
    if (sess->user < 0 || sess->user >= d->users)
        return orbit_error("Bitte zuerst anmelden.");
    int uid = sess->user;
    uint32_t bit = UINT32_C(1) << uid;
    if (!strcmp(op, "remember")) {
        uint8_t raw[32];
        if (orbit_random(raw, 32))
            return orbit_error("Anmeldung konnte nicht gespeichert werden.");
        OrbitUser *u = &d->user[uid];
        memmove(u->devices + 1, u->devices, 3 * 32);
        crypto_blake2b(u->devices[0], 32, raw, 32);
        char token[65];
        orbit_hex(token, raw, 32);
        crypto_wipe(raw, 32);
        cJSON *v = ok();
        cJSON_AddStringToObject(v, "token", token);
        crypto_wipe(token, 65);
        *changed = 1;
        return v;
    }
    if (!strcmp(op, "bind_device")) {
        /* Migration from 0.6: attach this console to the previously signed-in account. */
        uint8_t raw[32], hash[32];
        if (orbit_unhex(raw, orbit_string(r, "token"), 32))
            return orbit_error("Konsolen-Anmeldung ungültig.");
        crypto_blake2b_keyed(hash, 32, (const uint8_t *)"orbit-device", 12, raw, 32);
        crypto_wipe(raw, 32);
        for (int i = 0; i < d->users; i++)
            for (int j = 0; j < 4; j++)
                if (!crypto_verify32(hash, d->user[i].devices[j]))
                    memset(d->user[i].devices[j], 0, 32);
        OrbitUser *u = &d->user[uid];
        memmove(u->devices + 1, u->devices, 3 * 32);
        memcpy(u->devices[0], hash, 32);
        crypto_wipe(hash, 32);
        *changed = 1;
        return ok();
    }
    if (!strcmp(op, "forget_token")) {
        uint8_t raw[32], hash[32];
        if (orbit_unhex(raw, orbit_string(r, "token"), 32))
            return orbit_error("Anmeldung ungültig.");
        crypto_blake2b(hash, 32, raw, 32);
        crypto_wipe(raw, 32);
        for (int j = 0; j < 4; j++)
            if (!crypto_verify32(hash, d->user[uid].devices[j])) {
                memset(d->user[uid].devices[j], 0, 32);
                *changed = 1;
            }
        crypto_wipe(hash, 32);
        sess->user = -1;
        return ok();
    }
    if (!strcmp(op, "invitations")) {
        cJSON *v = ok(), *a = cJSON_AddArrayToObject(v, "invitations");
        for (int i = 0; i < ORBIT_GROUPS; i++) {
            OrbitGroup *g = &d->group[i];
            if (!g->used || !(g->pending & bit))
                continue;
            cJSON *x = cJSON_CreateObject();
            cJSON_AddNumberToObject(x, "group", i);
            cJSON_AddStringToObject(x, "name", g->name);
            cJSON_AddStringToObject(x, "from", d->user[g->owner].name);
            cJSON_AddItemToArray(a, x);
        }
        return v;
    }
    if (!strcmp(op, "respond_invite")) {
        int id = number(r, "group", -1);
        cJSON *accept = cJSON_GetObjectItemCaseSensitive(r, "accept");
        if (id < 0 || id >= ORBIT_GROUPS || !d->group[id].used || !(d->group[id].pending & bit) ||
            !cJSON_IsBool(accept))
            return orbit_error("Diese Einladung ist nicht mehr verfügbar.");
        d->group[id].pending &= ~bit;
        if (cJSON_IsTrue(accept))
            d->group[id].members |= bit;
        *changed = 1;
        return ok();
    }
    if (!strcmp(op, "logout")) {
        sess->user = -1;
        return ok();
    }
    if (!strcmp(op, "groups")) {
        cJSON *v = ok(), *a = cJSON_AddArrayToObject(v, "groups");
        for (int i = 0; i < ORBIT_GROUPS; i++) {
            OrbitGroup *g = &d->group[i];
            if (g->used && (g->members & bit)) {
                cJSON *x = cJSON_CreateObject();
                cJSON_AddNumberToObject(x, "id", i);
                cJSON_AddStringToObject(x, "name", g->name);
                cJSON_AddBoolToObject(x, "owner", g->owner == uid);
                int count = 0;
                for (int j = 0; j < d->users; j++)
                    if (g->members & (UINT32_C(1) << j))
                        count++;
                cJSON_AddNumberToObject(x, "members", count);
                cJSON_AddStringToObject(x, "host", d->user[g->owner].name);
                uint32_t last = 0;
                for (int k = 0; k < ORBIT_MESSAGES; k++)
                    if (d->message[k].id && d->message[k].group == i && d->message[k].id > last)
                        last = d->message[k].id;
                cJSON_AddNumberToObject(x, "last", last);
                cJSON *call = cJSON_AddArrayToObject(x, "call");
                for (int j = 0; j < d->users; j++)
                    if (s->call[i] & g->members & (UINT32_C(1) << j)) {
                        cJSON *c = cJSON_CreateObject();
                        cJSON_AddNumberToObject(c, "user", j);
                        cJSON_AddStringToObject(c, "name", d->user[j].name);
                        cJSON_AddItemToArray(call, c);
                    }
                cJSON *names = cJSON_AddArrayToObject(x, "people");
                for (int j = 0, shown = 0; j < d->users && shown < 8; j++)
                    if (g->members & (UINT32_C(1) << j)) {
                        cJSON_AddItemToArray(names, cJSON_CreateString(d->user[j].name));
                        shown++;
                    }
                cJSON_AddItemToArray(a, x);
            }
        }
        return v;
    }
    if (!strcmp(op, "create")) {
        const char *name = orbit_string(r, "name");
        if (!orbit_text_valid(name, 1, 64))
            return orbit_error("Gruppenname: 1 bis 64 UTF-8-Bytes.");
        int id = -1;
        for (int i = 0; i < ORBIT_GROUPS; i++)
            if (!d->group[i].used) {
                id = i;
                break;
            }
        if (id < 0)
            return orbit_error("Alle 24 Gruppenplaetze belegt.");
        char generated[33];
        if (new_group_code(d, generated))
            return orbit_error("Zufallsquelle nicht verfügbar.");
        OrbitGroup *g = &d->group[id];
        g->used = 1;
        g->owner = uid;
        g->members = bit;
        strcpy(g->name, name);
        strcpy(g->invite, generated);
        *changed = 1;
        cJSON *v = ok();
        cJSON_AddNumberToObject(v, "group", id);
        return v;
    }
    if (!strcmp(op, "join")) {
        uint64_t now = orbit_millis();
        if (now - s->join_window[uid] >= 60000) {
            s->join_window[uid] = now;
            s->join_count[uid] = 0;
        }
        if (s->join_count[uid] >= 12)
            return orbit_error("Zu viele Codeversuche. Bitte eine Minute warten.");
        s->join_count[uid]++;
        char invite[33];
        if (orbit_group_code(invite, orbit_string(r, "invite")))
            return orbit_error("Code passt nicht zu diesem Treffpunkt. Vergleicht eure "
                               "Treffpunkt-ID; verbindet euch zuerst mit demselben Gastgeber.");
        for (int i = 0; i < ORBIT_GROUPS; i++) {
            OrbitGroup *g = &d->group[i];
            if (g->used && !strcmp(invite, g->invite)) {
                g->members |= bit;
                g->pending &= ~bit;
                *changed = 1;
                cJSON *v = ok();
                cJSON_AddNumberToObject(v, "group", i);
                return v;
            }
        }
        return orbit_error("Code passt nicht zu diesem Treffpunkt. Vergleicht eure Treffpunkt-ID; "
                           "verbindet euch zuerst mit demselben Gastgeber.");
    }
    int gid = number(r, "group", -1);
    if (gid < 0 || gid >= ORBIT_GROUPS || !d->group[gid].used || !(d->group[gid].members & bit))
        return orbit_error("Kein Zugriff auf diese Gruppe.");
    OrbitGroup *g = &d->group[gid];
    if (!strcmp(op, "invite_user")) {
        if (g->owner != uid)
            return orbit_error("Nur der Gruppeninhaber kann einladen.");
        const char *email = orbit_string(r, "email");
        if (!valid_email(email))
            return orbit_error("Bitte eine gültige Konto-E-Mail eingeben.");
        char norm[97];
        normalize_email(norm, email);
        int target = -1;
        for (int i = 0; i < d->users; i++)
            if (!strcmp(d->user[i].email, norm))
                target = i;
        if (target < 0) {
            int slot = -1;
            for (int i = 0; i < ORBIT_PENDING_EMAILS; i++) {
                OrbitPendingEmail *e = &d->pending_email[i];
                if (e->used && e->group == gid && !strcmp(e->email, norm)) {
                    slot = i;
                    break;
                }
                if (!e->used)
                    slot = i;
            }
            if (slot < 0)
                return orbit_error("Zu viele offene Einladungen. Bitte später erneut versuchen.");
            OrbitPendingEmail *e = &d->pending_email[slot];
            e->used = 1;
            e->group = gid;
            strcpy(e->email, norm);
            *changed = 1;
            cJSON *v = ok();
            cJSON_AddBoolToObject(v, "waiting_registration", 1);
            return v;
        }
        uint32_t other = UINT32_C(1) << target;
        if (g->members & other)
            return orbit_error("Diese Person ist schon in der Gruppe.");
        g->pending |= other;
        *changed = 1;
        return ok();
    }
    if (!strcmp(op, "invite")) {
        /* Every member may share the group: one invitation is all a friend needs. */
        cJSON *v = ok();
        cJSON_AddStringToObject(v, "invite", g->invite);
        return v;
    }
    if (!strcmp(op, "rotate")) {
        if (g->owner != uid)
            return orbit_error("Nur der Gruppeninhaber kann Codes erneuern.");
        char generated[33];
        if (new_group_code(d, generated))
            return orbit_error("Zufallsquelle nicht verfügbar.");
        strcpy(g->invite, generated);
        *changed = 1;
        return ok();
    }
    if (!strcmp(op, "leave")) {
        if (g->owner == uid)
            return orbit_error("Inhaber: Gruppe mit delete löschen.");
        g->members &= ~bit;
        *changed = 1;
        return ok();
    }
    if (!strcmp(op, "delete")) {
        if (g->owner != uid)
            return orbit_error("Nur der Gruppeninhaber kann löschen.");
        for (int i = 0; i < ORBIT_PENDING_EMAILS; i++)
            if (d->pending_email[i].used && d->pending_email[i].group == gid)
                memset(&d->pending_email[i], 0, sizeof(d->pending_email[i]));
        memset(g, 0, sizeof(*g));
        for (int i = 0; i < ORBIT_MESSAGES; i++)
            if (d->message[i].group == gid)
                memset(&d->message[i], 0, sizeof(d->message[i]));
        *changed = 1;
        return ok();
    }
    if (!strcmp(op, "send")) {
        const char *body = orbit_string(r, "text");
        if (!orbit_text_valid(body, 1, 768))
            return orbit_error("Nachricht: 1 bis 768 UTF-8-Bytes, ohne Steuerzeichen.");
        uint64_t now = orbit_millis();
        if (now >= sess->last_write && now - sess->last_write < 300)
            return orbit_error("Bitte kurz warten.");
        if (d->next_id >= 2147483600)
            return orbit_error("Nachrichtenzähler voll.");
        OrbitMessage *m = &d->message[d->next_id % ORBIT_MESSAGES];
        m->id = d->next_id++;
        m->user = uid;
        m->group = gid;
        m->time = orbit_unix_time();
        strcpy(m->text, body);
        sess->last_write = now;
        *changed = 1;
        cJSON *v = ok();
        cJSON_AddNumberToObject(v, "id", m->id);
        return v;
    }
    if (!strcmp(op, "history")) {
        int after = number(r, "after", 0);
        if (after < 0)
            return orbit_error("Ungültiger Nachrichtenzähler.");
        cJSON *v = ok(), *a = cJSON_AddArrayToObject(v, "messages");
        uint32_t start = d->next_id > ORBIT_MESSAGES ? d->next_id - ORBIT_MESSAGES : 1;
        int emitted = 0;
        for (uint32_t id = start; id < d->next_id; id++) {
            OrbitMessage *m = &d->message[id % ORBIT_MESSAGES];
            if (m->id == id && m->group == gid && id > (uint32_t)after) {
                if (emitted++ == 40)
                    break;
                cJSON *x = cJSON_CreateObject();
                cJSON_AddNumberToObject(x, "id", id);
                cJSON_AddNumberToObject(x, "user", m->user);
                cJSON_AddStringToObject(x, "name", d->user[m->user].name);
                cJSON_AddNumberToObject(x, "time", m->time);
                cJSON_AddStringToObject(x, "text", m->text);
                cJSON_AddItemToArray(a, x);
            }
        }
        return v;
    }
    return orbit_error("Unbekannte Aktion.");
}
int orbit_store_code_for_tag(OrbitStore *s, const char tag[17], char code[9]) {
    int found = -1;
    pthread_mutex_lock(&s->mutex);
    for (int i = 0; i < ORBIT_GROUPS && found < 0; i++) {
        const OrbitGroup *g = &s->data.group[i];
        char t[17];
        if (!g->used || strlen(g->invite) != 8)
            continue;
        orbit_group_tag(t, g->invite);
        if (!strcmp(t, tag)) {
            memcpy(code, g->invite, 9);
            found = 0;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return found;
}
cJSON *orbit_request(OrbitStore *s, OrbitSession *session, const cJSON *req) {
    if (!cJSON_IsObject(req))
        return orbit_error("Ungültige Anfrage.");
    pthread_mutex_lock(&s->mutex);
    OrbitData *before = malloc(sizeof(*before));
    if (!before) {
        pthread_mutex_unlock(&s->mutex);
        return orbit_error("Speicher belegt.");
    }
    memcpy(before, &s->data, sizeof(*before));
    OrbitSession prior = *session;
    int changed = 0;
    cJSON *r = dispatch(s, session, req, &changed);
    if (changed && save(s)) {
        memcpy(&s->data, before, sizeof(*before));
        *session = prior;
        cJSON_Delete(r);
        r = orbit_error("Speichern fehlgeschlagen. Aktion nicht übernommen.");
    }
    crypto_wipe(before, sizeof(*before));
    free(before);
    pthread_mutex_unlock(&s->mutex);
    return r;
}

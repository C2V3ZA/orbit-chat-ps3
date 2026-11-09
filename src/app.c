/* Orbit 0.7 application core.
 * Every console runs its own meeting point in the background. Groups you create live on your
 * PS3; groups you were invited to live on the inviting PS3. The app keeps one encrypted link per
 * meeting point, signs in automatically with a per-console secret and saves everything. */
#include "monocypher.h"
#include "ui.h"
#include "voice.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define LOCK(a) pthread_mutex_lock(&(a)->lock)
#define UNLOCK(a) pthread_mutex_unlock(&(a)->lock)
static void notice_locked(OrbitApp *a, const char *text) {
    snprintf(a->status, sizeof(a->status), "%s", text);
    a->status_id++;
    a->changed++;
}
void app_notice(OrbitApp *a, const char *text) {
    LOCK(a);
    notice_locked(a, text);
    UNLOCK(a);
}
static int success(const cJSON *r) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok"));
}
static int num(const cJSON *o, const char *key, int fallback) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}
static cJSON *command(const char *op) {
    cJSON *q = cJSON_CreateObject();
    cJSON_AddStringToObject(q, "op", op);
    return q;
}
static void wipe_token(cJSON *q) {
    cJSON *t = cJSON_GetObjectItemCaseSensitive(q, "token");
    if (cJSON_IsString(t))
        crypto_wipe(t->valuestring, strlen(t->valuestring));
}
/* ---------- persistence: data/profile.json (atomic, with backup) ---------- */
static int write_atomic(const char *path, const char *text) {
    char tmp[540], bak[540];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    if (fflush(f) || fsync(fileno(f)))
        ok = 0;
    if (fclose(f))
        ok = 0;
    if (!ok) {
        remove(tmp);
        return -1;
    }
    /* PS3 rename does not replace an existing file: keep the last copy as backup. */
    if (remove(bak) && errno != ENOENT)
        return -1;
    int moved = rename(path, bak) == 0;
    if (rename(tmp, path)) {
        if (moved)
            rename(bak, path);
        return -1;
    }
    return 0;
}
static char *read_file(const char *path, long limit) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *text = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        long n = ftell(f);
        rewind(f);
        if (n > 0 && n < limit && (text = calloc(1, (size_t)n + 1)) &&
            fread(text, 1, (size_t)n, f) != (size_t)n) {
            free(text);
            text = NULL;
        }
    }
    fclose(f);
    return text;
}
static void save_profile_locked(OrbitApp *a) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 7);
    cJSON_AddStringToObject(root, "name", a->name);
    if (a->relay_user)
        cJSON_AddStringToObject(root, "relay", a->relay);
    const AppLink *sel = a->selected_link >= 0 && a->selected_link < APP_LINKS
                             ? &a->links[a->selected_link]
                             : NULL;
    if (sel && sel->used && sel->server[0]) {
        cJSON *s = cJSON_AddObjectToObject(root, "selected");
        cJSON_AddStringToObject(s, "server", sel->server);
        cJSON_AddNumberToObject(s, "group", a->selected_group);
    }
    cJSON *rooms = cJSON_AddArrayToObject(root, "rooms"), *seen = cJSON_AddArrayToObject(root, "seen");
    for (int i = 0; i < APP_LINKS; i++) {
        const AppLink *L = &a->links[i];
        if (!L->used || L->remove)
            continue;
        if (L->server[0])
            for (int g = 0; g < L->group_count; g++) {
                uint32_t v = a->seen[i][L->groups[g].id];
                if (!v)
                    continue;
                cJSON *x = cJSON_CreateObject();
                cJSON_AddStringToObject(x, "server", L->server);
                cJSON_AddNumberToObject(x, "group", L->groups[g].id);
                cJSON_AddNumberToObject(x, "id", v);
                cJSON_AddItemToArray(seen, x);
            }
        if (L->own)
            continue;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "host", L->host);
        cJSON_AddNumberToObject(r, "port", L->port);
        cJSON_AddStringToObject(r, "code", L->code);
        cJSON_AddNumberToObject(r, "code_group", L->code_group);
        cJSON_AddStringToObject(r, "server", L->server);
        cJSON_AddStringToObject(r, "join", L->join_code);
        cJSON_AddStringToObject(r, "relay", L->relay);
        cJSON *gs = cJSON_AddArrayToObject(r, "groups");
        for (int g = 0; g < L->group_count; g++) {
            cJSON *x = cJSON_CreateObject();
            cJSON_AddNumberToObject(x, "id", L->groups[g].id);
            cJSON_AddStringToObject(x, "name", L->groups[g].name);
            cJSON_AddStringToObject(x, "host", L->groups[g].host);
            cJSON_AddItemToArray(gs, x);
        }
        cJSON_AddItemToArray(rooms, r);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char path[512];
    snprintf(path, sizeof(path), "%s/profile.json", a->data_dir);
    if (!text || write_atomic(path, text))
        notice_locked(a, "Speichern fehlgeschlagen. Ist die Festplatte voll?");
    else
        a->dirty = 0;
    free(text);
}
static void load_profile(OrbitApp *a) {
    char path[512];
    snprintf(path, sizeof(path), "%s/profile.json", a->data_dir);
    char *text = read_file(path, 1 << 20);
    if (!text) {
        char bak[520];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        text = read_file(bak, 1 << 20);
    }
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root)
        return;
    const char *name = orbit_string(root, "name");
    if (orbit_text_valid(name, 1, 48)) {
        snprintf(a->name, sizeof(a->name), "%s", name);
        a->profile = 1;
    }
    cJSON *relay = cJSON_GetObjectItem(root, "relay");
    if (cJSON_IsString(relay) && strlen(relay->valuestring) < sizeof(a->relay)) {
        snprintf(a->relay, sizeof(a->relay), "%s", relay->valuestring);
        a->relay_user = 1;
    }
    cJSON *r;
    int slot = 1;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(root, "rooms")) {
        if (slot >= APP_LINKS)
            break;
        AppLink *L = &a->links[slot];
        const char *host = orbit_string(r, "host"), *code = orbit_string(r, "code"),
                   *server = orbit_string(r, "server"), *join = orbit_string(r, "join");
        int port = num(r, "port", 0);
        char norm[33];
        struct in_addr probe;
        if (inet_pton(AF_INET, host, &probe) != 1 || port < 1 || port > 65535 ||
            orbit_group_code(norm, code) || strlen(norm) != 8 || strlen(server) > 16)
            continue;
        memset(L, 0, sizeof(*L));
        L->owner = a;
        L->used = 1;
        L->port = port;
        L->code_group = num(r, "code_group", -1);
        snprintf(L->host, sizeof(L->host), "%s", host);
        snprintf(L->code, sizeof(L->code), "%s", norm);
        snprintf(L->server, sizeof(L->server), "%s", server);
        if (!orbit_group_code(norm, join) && strlen(norm) == 8)
            snprintf(L->join_code, sizeof(L->join_code), "%s", norm);
        if (strlen(orbit_string(r, "relay")) < sizeof(L->relay))
            snprintf(L->relay, sizeof(L->relay), "%s", orbit_string(r, "relay"));
        cJSON *g;
        cJSON_ArrayForEach(g, cJSON_GetObjectItem(r, "groups")) {
            if (L->group_count >= ORBIT_GROUPS)
                break;
            int id = num(g, "id", -1);
            if (id < 0 || id >= ORBIT_GROUPS)
                continue;
            L->groups[L->group_count].id = id;
            snprintf(L->groups[L->group_count].name, 65, "%s", orbit_string(g, "name"));
            snprintf(L->groups[L->group_count].host, 49, "%s", orbit_string(g, "host"));
            L->group_count++;
        }
        L->cached = 1;
        L->history_group = -1;
        slot++;
    }
    cJSON *s = cJSON_GetObjectItem(root, "selected");
    const char *selserver = orbit_string(s, "server");
    int selgroup = num(s, "group", -1);
    for (int i = 0; i < APP_LINKS; i++) {
        AppLink *L = &a->links[i];
        if (!L->used)
            continue;
        const char *id = L->own ? a->server_id : L->server;
        if (selserver[0] && !strcmp(id, selserver)) {
            a->selected_link = i;
            a->selected_group = selgroup;
        }
        cJSON *x;
        cJSON_ArrayForEach(x, cJSON_GetObjectItem(root, "seen")) {
            int g = num(x, "group", -1);
            double v = cJSON_IsNumber(cJSON_GetObjectItem(x, "id"))
                           ? cJSON_GetObjectItem(x, "id")->valuedouble
                           : 0;
            if (g >= 0 && g < ORBIT_GROUPS && v > 0 && !strcmp(orbit_string(x, "server"), id))
                a->seen[i][g] = (uint32_t)v;
        }
    }
    cJSON_Delete(root);
}
/* ---------- one link per meeting point ---------- */
static void device_token(const OrbitApp *a, const char *server, char out[65]) {
    uint8_t t[32];
    crypto_blake2b_keyed(t, 32, a->device, 32, (const uint8_t *)server, strlen(server));
    orbit_hex(out, t, 32);
    crypto_wipe(t, 32);
}
static int sign_in(OrbitApp *a, AppLink *L, OrbitLink *c, const char *server) {
    char name[49], token[65];
    LOCK(a);
    snprintf(name, sizeof(name), "%s", a->name);
    UNLOCK(a);
    device_token(a, server, token);
    if (L->own) {
        /* Migration from 0.6: keep the account (groups, messages) that signed in on this PS3. */
        OrbitRemember old;
        if (!orbit_remember_read(a->data_dir, &old) && old.hosting) {
            cJSON *q = command("resume");
            cJSON_AddStringToObject(q, "email", old.email);
            cJSON_AddStringToObject(q, "token", old.token);
            cJSON *r = orbit_exchange(c, q);
            wipe_token(q);
            cJSON_Delete(q);
            if (r && success(r)) {
                cJSON *b = command("bind_device");
                cJSON_AddStringToObject(b, "token", token);
                cJSON *br = orbit_exchange(c, b);
                wipe_token(b);
                cJSON_Delete(b);
                if (br && success(br))
                    orbit_remember_clear(a->data_dir, &old);
                cJSON_Delete(br);
            }
            cJSON_Delete(r);
        }
        crypto_wipe(&old, sizeof(old));
    }
    cJSON *q = command("device");
    cJSON_AddStringToObject(q, "token", token);
    cJSON_AddStringToObject(q, "name", name);
    cJSON *r = orbit_exchange(c, q);
    wipe_token(q);
    cJSON_Delete(q);
    crypto_wipe(token, sizeof(token));
    int ok = r && success(r);
    LOCK(a);
    if (ok)
        L->user = num(r, "user", -1);
    else if (r)
        snprintf(L->error, sizeof(L->error), "%s", orbit_string(r, "error"));
    UNLOCK(a);
    int rc = ok ? 0 : r ? -1 : -2;
    cJSON_Delete(r);
    return rc;
}
/* Returns 1 on a duplicate or own meeting point: the link is folded into another one. */
static int fold_duplicate(OrbitApp *a, AppLink *L, const char *server) {
    int index = (int)(L - a->links), folded = 0;
    LOCK(a);
    if (!L->own && !strcmp(server, a->server_id)) {
        /* An invitation to my own PS3: I am already in all its groups as the host's member. */
        AppLink *own = &a->links[0];
        if (L->join_code[0])
            snprintf(own->join_code, sizeof(own->join_code), "%s", L->join_code);
        folded = 1;
    }
    for (int i = 0; i < APP_LINKS && !folded; i++) {
        AppLink *o = &a->links[i];
        if (i == index || !o->used || o->remove || o->own || strcmp(o->server, server))
            continue;
        if (L->join_code[0])
            snprintf(o->join_code, sizeof(o->join_code), "%s", L->join_code);
        snprintf(o->host, sizeof(o->host), "%s", L->host);
        o->port = L->port;
        o->retry_at = 0;
        folded = 1;
    }
    if (folded) {
        L->remove = 1;
        a->dirty = 1;
        if (a->selected_link == index)
            a->selected_link = 0;
    }
    UNLOCK(a);
    return folded;
}
static OrbitLink *link_open(OrbitApp *a, AppLink *L, int voice) {
    char reason[200] = "Keine Verbindung.", host[16], code[9], broadcast[16];
    int port;
    LOCK(a);
    snprintf(host, sizeof(host), "%s", L->host);
    snprintf(code, sizeof(code), "%s", L->code);
    snprintf(broadcast, sizeof(broadcast), "%s", a->broadcast);
    port = L->port;
    UNLOCK(a);
    OrbitLink *c = NULL;
    if (L->own)
        c = orbit_connect_detailed("127.0.0.1", a->port, a->server_key, reason, sizeof(reason));
    else {
        /* Same home network first: no router involved, works even without UPnP. */
        char tag[17], lan[16];
        orbit_group_tag(tag, code);
        if (!orbit_discover(tag, a->port, broadcast, lan, 700))
            c = orbit_connect_group(lan, a->port, code, reason, sizeof(reason));
        if (!c && strcmp(host, "0.0.0.0"))
            c = orbit_connect_group(host, port, code, reason, sizeof(reason));
        /* No router settings anywhere: both consoles meet at the relay. */
        char relay[104];
        LOCK(a);
        snprintf(relay, sizeof(relay), "%s", L->relay[0] ? L->relay : a->relay);
        UNLOCK(a);
        if (!c && relay[0]) {
            char why[200];
            int fd = orbit_relay_dial(relay, tag, why, sizeof(why));
            if (fd >= 0)
                c = orbit_wrap_group(fd, code, reason, sizeof(reason));
            else
                snprintf(reason, sizeof(reason), "%s", why);
        }
    }
    if (!c) {
        LOCK(a);
        snprintf(L->error, sizeof(L->error), "%s", reason);
        UNLOCK(a);
        return NULL;
    }
    cJSON *q = command("hello"), *r = orbit_exchange(c, q);
    cJSON_Delete(q);
    char server[17] = {0};
    if (r && success(r) && strlen(orbit_string(r, "server")) == 16)
        snprintf(server, sizeof(server), "%s", orbit_string(r, "server"));
    cJSON_Delete(r);
    if (!server[0]) {
        LOCK(a);
        snprintf(L->error, sizeof(L->error),
                 "Die andere PS3 nutzt eine alte Orbit-Version. Bitte dort auf %s aktualisieren.",
                 ORBIT_VERSION);
        UNLOCK(a);
        orbit_disconnect(c);
        return NULL;
    }
    if (!voice && fold_duplicate(a, L, server)) {
        orbit_disconnect(c);
        return NULL;
    }
    if (sign_in(a, L, c, server)) {
        orbit_disconnect(c);
        return NULL;
    }
    LOCK(a);
    if (strcmp(L->server, server)) {
        snprintf(L->server, sizeof(L->server), "%s", server);
        a->dirty = 1;
    }
    UNLOCK(a);
    return c;
}
static void apply_groups(OrbitApp *a, AppLink *L, const cJSON *r) {
    static char backup[sizeof(L->groups)];
    static pthread_mutex_t backup_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&backup_lock);
    LOCK(a);
    int index = (int)(L - a->links), before = L->group_count;
    memcpy(backup, L->groups, sizeof(L->groups));
    memset(L->groups, 0, sizeof(L->groups));
    L->group_count = 0;
    const cJSON *g;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(r, "groups")) {
        if (L->group_count >= ORBIT_GROUPS)
            break;
        int id = num(g, "id", -1);
        if (id < 0 || id >= ORBIT_GROUPS)
            continue;
        typeof(L->groups[0]) *x = &L->groups[L->group_count++];
        x->id = id;
        x->members = num(g, "members", 1);
        cJSON *last = cJSON_GetObjectItem(g, "last");
        x->last = cJSON_IsNumber(last) && last->valuedouble > 0 ? (uint32_t)last->valuedouble : 0;
        snprintf(x->name, sizeof(x->name), "%s", orbit_string(g, "name"));
        snprintf(x->host, sizeof(x->host), "%s", orbit_string(g, "host"));
        x->people[0] = 0;
        int in_call = a->call_running && a->call_link == index && a->call_group == id;
        if (in_call)
            a->call_count = 0;
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(g, "call")) {
            x->call++;
            if (in_call && a->call_count < CALL_PEOPLE) {
                a->call_people[a->call_count].user = num(c, "user", -1);
                snprintf(a->call_people[a->call_count].name, 49, "%s", orbit_string(c, "name"));
                a->call_count++;
            }
        }
        if (in_call)
            snprintf(a->call_name, sizeof(a->call_name), "%s", x->name);
        const cJSON *p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItem(g, "people")) {
            if (!cJSON_IsString(p))
                continue;
            size_t n = strlen(x->people);
            snprintf(x->people + n, sizeof(x->people) - n, "%s%s", n ? ", " : "", p->valuestring);
        }
    }
    /* Redraw only when something visible changed: no needless full-screen renders. */
    int changed = before != L->group_count || L->cached ||
                  memcmp(backup, L->groups, sizeof(L->groups));
    int renamed = before != L->group_count || L->cached;
    for (int i = 0; i < L->group_count && !renamed; i++)
        renamed = strcmp(((typeof(L->groups[0]) *)backup)[i].name, L->groups[i].name) != 0;
    L->cached = 0;
    if (renamed)
        a->dirty = 1;
    if (changed)
        a->changed++;
    UNLOCK(a);
    pthread_mutex_unlock(&backup_lock);
}
static void apply_history(OrbitApp *a, AppLink *L, int gid, const cJSON *r) {
    LOCK(a);
    int index = (int)(L - a->links);
    if (a->selected_link != index || a->selected_group != gid) {
        UNLOCK(a);
        return;
    }
    if (L->history_group != gid) {
        L->history_group = gid;
        L->message_count = 0;
    }
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(r, "messages")) {
        int id = num(m, "id", 0);
        if (id <= 0 || (L->message_count && id <= L->messages[L->message_count - 1].id))
            continue;
        if (L->message_count == UI_MESSAGES) {
            memmove(L->messages, L->messages + 1, (UI_MESSAGES - 1) * sizeof(UiMessage));
            L->message_count--;
        }
        UiMessage *u = &L->messages[L->message_count++];
        u->id = id;
        u->mine = num(m, "user", -2) == L->user;
        cJSON *t = cJSON_GetObjectItem(m, "time");
        u->time = cJSON_IsNumber(t) && t->valuedouble > 0 ? (uint32_t)t->valuedouble : 0;
        snprintf(u->name, sizeof(u->name), "%s", orbit_string(m, "name"));
        snprintf(u->text, sizeof(u->text), "%s", orbit_string(m, "text"));
        if ((uint32_t)id > a->seen[index][gid]) {
            a->seen[index][gid] = (uint32_t)id;
            a->dirty = 1;
        }
    }
    a->changed++;
    UNLOCK(a);
}
/* Runs one queued action. Returns -1 when the connection broke. */
static int run_job(OrbitApp *a, AppLink *L, OrbitLink *c, cJSON *job) {
    const char *op = orbit_string(job, "op");
    cJSON *r = orbit_exchange(c, job);
    if (!r) {
        /* Never leave the app "busy": that froze every later action in 0.7. */
        LOCK(a);
        a->busy = 0;
        if (!strcmp(op, "invite"))
            a->invite_state = 3;
        notice_locked(a, "Verbindung kurz unterbrochen. Bitte noch einmal versuchen.");
        UNLOCK(a);
        return -1;
    }
    int index = (int)(L - a->links);
    LOCK(a);
    if (!success(r)) {
        notice_locked(a, orbit_string(r, "error"));
        if (!strcmp(op, "invite"))
            a->invite_state = 3;
    } else if (!strcmp(op, "send")) {
        a->sent_count++;
    } else if (!strcmp(op, "create")) {
        a->selected_link = index;
        a->selected_group = num(r, "group", -1);
        a->dirty = 1;
        notice_locked(a, "Gruppe erstellt. Mit DREIECK lädst du Freunde ein.");
    } else if (!strcmp(op, "leave") || !strcmp(op, "delete")) {
        a->selected_group = -1;
        notice_locked(a, !strcmp(op, "delete") ? "Gruppe gelöscht." : "Gruppe verlassen.");
    } else if (!strcmp(op, "invite")) {
        const char *code = orbit_string(r, "invite");
        uint32_t ip = 0;
        int port = 0;
        char scope[200];
        struct in_addr in;
        static const char via_relay[] =
            "Klappt überall über den Orbit-Vermittler - ohne Router-Einstellungen.";
        if (L->own) {
            if (!a->nat.ok && a->bore_online && a->bore_ip && a->bore_port) {
                ip = a->bore_ip; /* public tunnel address of this console */
                port = a->bore_port;
                snprintf(scope, sizeof(scope), "%s",
                         "Klappt überall über den Orbit-Tunnel - kein PC und keine "
                         "Router-Einstellungen nötig.");
            } else if (a->relay_ip && !a->nat.ok) {
                ip = a->relay_ip; /* relay address, flagged in the port field */
                port = ORBIT_INVITE_RELAY | a->relay_port;
                snprintf(scope, sizeof(scope), "%s", via_relay);
            } else if (a->nat.ok && inet_pton(AF_INET, a->nat.external, &in) == 1) {
                ip = ntohl(in.s_addr);
                port = a->nat.port;
                snprintf(scope, sizeof(scope),
                         "Klappt überall: über das Internet und im selben WLAN.");
            } else if (inet_pton(AF_INET, a->ip, &in) == 1) {
                ip = ntohl(in.s_addr);
                port = a->port;
                snprintf(scope, sizeof(scope), "Nur im selben Heimnetz. %.150s",
                         a->nat.reason[0] ? a->nat.reason : "Internet-Freigabe wird noch geprüft.");
            }
        } else if (!strcmp(L->host, "0.0.0.0")) {
            char rh[ORBIT_HOST];
            int rp = ORBIT_RELAY_PORT;
            const char *r = L->relay[0] ? L->relay : a->relay;
            if (!orbit_parse_endpoint(r, rh, &rp) && inet_pton(AF_INET, rh, &in) == 1) {
                ip = ntohl(in.s_addr);
                port = ORBIT_INVITE_RELAY | (strchr(r, ':') ? rp : ORBIT_RELAY_PORT);
            } else if (a->relay_ip) {
                ip = a->relay_ip;
                port = ORBIT_INVITE_RELAY | a->relay_port;
            }
            snprintf(scope, sizeof(scope), "%s", via_relay);
        } else if (inet_pton(AF_INET, L->host, &in) == 1) {
            ip = ntohl(in.s_addr);
            port = L->port;
            snprintf(scope, sizeof(scope), "%s",
                     orbit_ipv4_private(ip) ? "Nur im Heimnetz des Gastgebers."
                                            : "Klappt über das Internet, solange der Gastgeber "
                                              "Orbit offen hat.");
        }
        if (port && !orbit_invite_encode(a->invite_code, ip, port, code)) {
            a->invite_state = 2;
            snprintf(a->invite_scope, sizeof(a->invite_scope), "%s", scope);
            int gid = num(job, "group", -1);
            a->invite_group[0] = 0;
            for (int i = 0; i < L->group_count; i++)
                if (L->groups[i].id == gid)
                    snprintf(a->invite_group, sizeof(a->invite_group), "%s", L->groups[i].name);
        } else {
            a->invite_state = 3;
            notice_locked(a, "Keine Netzwerkadresse. Ist die PS3 mit dem Internet verbunden?");
        }
    }
    a->busy = 0;
    a->changed++;
    UNLOCK(a);
    cJSON_Delete(r);
    return 0;
}
static void *link_main(void *arg) {
    AppLink *L = arg;
    OrbitApp *a = L->owner;
    int index = (int)(L - a->links);
    OrbitLink *c = NULL;
    uint64_t poll_at = 0;
    int watched = -2;
    uint32_t watched_last = 0;
    for (;;) {
        LOCK(a);
        int stop = a->stop || L->remove;
        cJSON *job = c ? L->job : NULL;
        if (c)
            L->job = NULL;
        int reauth = L->reauth;
        L->reauth = 0;
        int watch = a->selected_link == index ? a->selected_group : -1;
        char join[9];
        snprintf(join, sizeof(join), "%s", L->join_code);
        uint64_t retry = L->retry_at;
        UNLOCK(a);
        if (stop) {
            cJSON_Delete(job);
            break;
        }
        uint64_t now = orbit_millis();
        if (!c) {
            if (now < retry) {
                LOCK(a);
                if (L->job) {
                    /* No connection: fail fast instead of freezing the UI. */
                    cJSON_Delete(L->job);
                    L->job = NULL;
                    a->busy = 0;
                    if (a->invite_state == 1)
                        a->invite_state = 3;
                    notice_locked(a, "Der Gastgeber dieser Gruppe ist gerade offline.");
                }
                UNLOCK(a);
                usleep(100000);
                continue;
            }
            LOCK(a);
            L->state = LINK_CONNECTING;
            a->changed++;
            UNLOCK(a);
            c = link_open(a, L, 0);
            LOCK(a);
            if (c) {
                L->state = LINK_ONLINE;
                L->backoff = 2000;
                L->error[0] = 0;
            } else if (!L->remove) {
                L->state = LINK_FAILED;
                if (L->backoff < 2000)
                    L->backoff = 2000;
                L->retry_at = orbit_millis() + L->backoff;
                /* short waits: a friend should be back within seconds, not half a minute */
                L->backoff = L->backoff * 2 > 6000 ? 6000 : L->backoff * 2;
            }
            a->changed++;
            UNLOCK(a);
            poll_at = 0;
            watched = -2;
            continue;
        }
        int broken = 0;
        if (reauth) {
            LOCK(a);
            const char *server = L->own ? a->server_id : L->server;
            char id[17];
            snprintf(id, sizeof(id), "%s", server);
            UNLOCK(a);
            broken = sign_in(a, L, c, id) == -2;
            poll_at = 0;
        }
        if (!broken && join[0]) {
            cJSON *q = command("join");
            cJSON_AddStringToObject(q, "invite", join);
            cJSON *r = orbit_exchange(c, q);
            cJSON_Delete(q);
            if (!r)
                broken = 1;
            else {
                LOCK(a);
                if (!strcmp(L->join_code, join))
                    L->join_code[0] = 0;
                if (success(r)) {
                    a->selected_link = index;
                    a->selected_group = num(r, "group", -1);
                    if (!L->own && (L->code_group < 0 || !strcmp(L->code, join)))
                        L->code_group = a->selected_group;
                    notice_locked(a, "Einladung angenommen. Ihr seid jetzt in derselben Gruppe.");
                } else
                    notice_locked(a, orbit_string(r, "error"));
                a->dirty = 1;
                UNLOCK(a);
                cJSON_Delete(r);
                poll_at = 0;
            }
        }
        if (!broken && job) {
            broken = run_job(a, L, c, job) != 0;
            poll_at = 0;
        }
        cJSON_Delete(job);
        if (!broken && (now >= poll_at || watch != watched)) {
            poll_at = now + 700;
            cJSON *q = command("groups"), *r = orbit_exchange(c, q);
            cJSON_Delete(q);
            if (!r)
                broken = 1;
            else if (success(r)) {
                apply_groups(a, L, r);
                LOCK(a);
                int present = 0, empty = !L->own && L->group_count == 0 && !L->join_code[0];
                uint32_t last = 0;
                for (int i = 0; i < L->group_count; i++) {
                    if (L->groups[i].id == L->code_group)
                        present = 1;
                    if (L->groups[i].id == watch)
                        last = L->groups[i].last;
                }
                int refresh = !L->own && !present && L->group_count ? L->groups[0].id : -1;
                if (empty) {
                    L->remove = 1;
                    a->dirty = 1;
                }
                UNLOCK(a);
                if (refresh >= 0) {
                    /* Keep a valid invitation for reconnecting when a group was removed. */
                    cJSON *iq = command("invite");
                    cJSON_AddNumberToObject(iq, "group", refresh);
                    cJSON *ir = orbit_exchange(c, iq);
                    cJSON_Delete(iq);
                    char norm[33];
                    LOCK(a);
                    if (ir && success(ir) && !orbit_group_code(norm, orbit_string(ir, "invite")) &&
                        strlen(norm) == 8) {
                        snprintf(L->code, sizeof(L->code), "%s", norm);
                        L->code_group = refresh;
                        a->dirty = 1;
                    }
                    UNLOCK(a);
                    cJSON_Delete(ir);
                }
                if (watch >= 0 && (watch != watched || last != watched_last)) {
                    LOCK(a);
                    int after = L->history_group == watch && L->message_count
                                    ? L->messages[L->message_count - 1].id
                                    : 0;
                    UNLOCK(a);
                    cJSON *h = command("history");
                    cJSON_AddNumberToObject(h, "group", watch);
                    cJSON_AddNumberToObject(h, "after", after);
                    cJSON *hr = orbit_exchange(c, h);
                    cJSON_Delete(h);
                    if (!hr)
                        broken = 1;
                    else if (success(hr)) {
                        apply_history(a, L, watch, hr);
                        /* More than one page waiting: fetch the rest right away. */
                        if (cJSON_GetArraySize(cJSON_GetObjectItem(hr, "messages")) >= 40)
                            poll_at = 0, last = 0;
                    }
                    cJSON_Delete(hr);
                    watched = watch;
                    watched_last = last;
                } else if (watch < 0)
                    watched = watch;
            }
            cJSON_Delete(r);
        }
        if (broken) {
            orbit_disconnect(c);
            c = NULL;
            LOCK(a);
            L->state = LINK_FAILED;
            snprintf(L->error, sizeof(L->error), "Verbindung unterbrochen. Neuer Versuch ...");
            L->retry_at = orbit_millis() + 1500;
            a->changed++;
            UNLOCK(a);
            continue;
        }
        usleep(25000);
    }
    orbit_disconnect(c);
    LOCK(a);
    if (L->job) {
        cJSON_Delete(L->job);
        L->job = NULL;
        a->busy = 0;
    }
    L->running = 0;
    L->state = LINK_OFF;
    UNLOCK(a);
    return NULL;
}
static int queue(OrbitApp *a, int link, cJSON *job);
static void broadcast_of(char out[16], const char *ip, const char *mask);
static void start_link_locked(OrbitApp *a, AppLink *L) {
    L->owner = a;
    L->history_group = -1;
    L->backoff = 2000;
    if (!pthread_create(&L->thread, NULL, link_main, L))
        L->running = L->thread_joinable = 1;
}
/* ---------- manager: hosting, router opening, saving ---------- */
static void *manager_main(void *arg) {
    OrbitApp *a = arg;
    uint64_t nat_at = 0, net_check = 0;
    for (;;) {
        LOCK(a);
        int stop = a->stop;
        UNLOCK(a);
        if (stop)
            break;
        uint64_t now = orbit_millis();
        if (a->netinfo && now >= net_check) {
            /* The PS3 may still be connecting at start, or get a new address later. */
            net_check = now + 5000;
            char ip[16] = "", mask[16] = "", gw[16] = "";
            if (!a->netinfo(ip, mask, gw) && ip[0]) {
                LOCK(a);
                if (strcmp(ip, a->ip) || strcmp(gw, a->gateway)) {
                    snprintf(a->ip, sizeof(a->ip), "%s", ip);
                    snprintf(a->netmask, sizeof(a->netmask), "%s", mask);
                    snprintf(a->gateway, sizeof(a->gateway), "%s", gw);
                    broadcast_of(a->broadcast, a->ip, a->netmask);
                    a->nat_now = 1;
                    for (int i = 0; i < APP_LINKS; i++)
                        a->links[i].retry_at = 0;
                }
                UNLOCK(a);
            }
        }
        /* Internet reachability without router settings: built-in tunnel first, then relay. */
        LOCK(a);
        {
            char why[200], bwhy[200];
            int online = orbit_relay_host_state(a->relay_host, why, sizeof(why));
            uint32_t bip = 0;
            int bport = 0;
            int bon = orbit_bore_state(a->bore, &bip, &bport, NULL, 0, bwhy, sizeof(bwhy));
            int dirty_state = online != a->relay_online || strcmp(why, a->relay_reason) ||
                              bon != a->bore_online || bport != a->bore_port ||
                              strcmp(bwhy, a->bore_reason);
            a->relay_online = online;
            snprintf(a->relay_reason, sizeof(a->relay_reason), "%s", why);
            a->bore_online = bon;
            a->bore_ip = bip;
            a->bore_port = bport;
            snprintf(a->bore_reason, sizeof(a->bore_reason), "%s", bwhy);
            if (dirty_state) {
                if (!a->nat.ok && a->net != NET_CHECKING && strcmp(a->ip, "127.0.0.1")) {
                    a->net = bon || online ? NET_INTERNET : NET_HOME;
                    snprintf(a->net_detail, sizeof(a->net_detail), "%s",
                             bon      ? "Über den Orbit-Tunnel erreichbar - kein PC und keine "
                                        "Router-Einstellungen nötig."
                             : online ? "Über den Orbit-Vermittler erreichbar - ohne "
                                        "Router-Einstellungen."
                             : a->bore ? a->bore_reason
                                       : a->relay_reason);
                }
                a->changed++;
            }
            /* A waiting invitation goes out as soon as the best address is known. */
            if (a->invite_wait && a->net != NET_CHECKING &&
                (a->nat.ok || bon || !a->bore || now - a->invite_wait_since > 15000)) {
                a->invite_wait = 0;
                a->invite_dispatch = 1;
            }
        }
        int dispatch = a->invite_dispatch, wl = a->invite_wait_link, wg = a->invite_wait_group;
        a->invite_dispatch = 0;
        UNLOCK(a);
        if (dispatch) {
            cJSON *q = cJSON_CreateObject();
            cJSON_AddStringToObject(q, "op", "invite");
            cJSON_AddNumberToObject(q, "group", wg);
            if (queue(a, wl, q)) {
                LOCK(a);
                a->invite_state = 3;
                UNLOCK(a);
            }
        }
        LOCK(a);
        if (a->nat_now) {
            a->nat_now = 0;
            nat_at = 0;
        }
        /* Safety net: no action may block the interface for more than 12 seconds. */
        if (a->busy && now - a->busy_since > 12000) {
            a->busy = 0;
            if (a->invite_state == 1 && !a->invite_wait)
                a->invite_state = 3;
            a->changed++;
        }
        UNLOCK(a);
        if (now >= nat_at) {
            /* Refresh twice per hour: lease renewal and changed public addresses. */
            nat_at = now + 30 * 60 * 1000;
            LOCK(a);
            if (a->net != NET_INTERNET) {
                a->net = NET_CHECKING;
                snprintf(a->net_detail, sizeof(a->net_detail), "Internet-Freigabe wird geprüft ...");
                a->changed++;
            }
            UNLOCK(a);
            OrbitNat n;
            char ip[16], gw[16];
            LOCK(a);
            snprintf(ip, sizeof(ip), "%s", a->ip);
            snprintf(gw, sizeof(gw), "%s", a->gateway);
            UNLOCK(a);
            int ok = !orbit_nat_open(ip, gw, a->port, &n);
            LOCK(a);
            a->nat = n;
            a->net = ok ? NET_INTERNET : !strcmp(ip, "127.0.0.1") ? NET_OFFLINE : NET_HOME;
            snprintf(a->net_detail, sizeof(a->net_detail), "%s",
                     ok ? "Freunde können dich über das Internet erreichen." : n.reason);
            a->relay_online = -1; /* recompute the combined state below */
            if (!ok && !n.cgnat && a->net == NET_HOME)
                nat_at = now + 5 * 60 * 1000; /* the router may allow it later */
            a->changed++;
            UNLOCK(a);
        }
        LOCK(a);
        for (int i = 1; i < APP_LINKS; i++) {
            AppLink *L = &a->links[i];
            if (L->used && L->remove && !L->running) {
                if (L->thread_joinable) {
                    pthread_t t = L->thread;
                    L->thread_joinable = 0;
                    UNLOCK(a);
                    pthread_join(t, NULL);
                    LOCK(a);
                }
                if (a->selected_link == i)
                    a->selected_link = 0, a->selected_group = -1;
                memset(a->seen[i], 0, sizeof(a->seen[i]));
                memset(L, 0, sizeof(*L));
                a->dirty = 1;
                a->changed++;
            }
        }
        if (a->dirty)
            save_profile_locked(a);
        UNLOCK(a);
        usleep(250000);
    }
    return NULL;
}
static void broadcast_of(char out[16], const char *ip, const char *mask) {
    struct in_addr i, m;
    out[0] = 0;
    if (inet_pton(AF_INET, ip, &i) == 1 && inet_pton(AF_INET, mask, &m) == 1) {
        uint32_t b = ntohl(i.s_addr) | ~ntohl(m.s_addr);
        snprintf(out, 16, "%u.%u.%u.%u", b >> 24, (b >> 16) & 255, (b >> 8) & 255, b & 255);
    }
}
static void relay_restart(OrbitApp *a);
int app_start(OrbitApp *a, const char *dir, const char *ip, const char *netmask,
              const char *gateway, const char *suggested, int port) {
    memset(a, 0, sizeof(*a));
    a->port = port > 0 ? port : ORBIT_PORT;
    pthread_mutex_init(&a->lock, NULL);
    snprintf(a->data_dir, sizeof(a->data_dir), "%s", dir);
    snprintf(a->ip, sizeof(a->ip), "%s", ip && ip[0] ? ip : "127.0.0.1");
    snprintf(a->netmask, sizeof(a->netmask), "%s", netmask ? netmask : "");
    snprintf(a->gateway, sizeof(a->gateway), "%s", gateway ? gateway : "");
    broadcast_of(a->broadcast, a->ip, a->netmask);
    a->selected_group = -1;
    a->changed = 1;
    a->net = NET_CHECKING;
    snprintf(a->net_detail, sizeof(a->net_detail), "Internet-Freigabe wird geprüft ...");
    char path[512];
    snprintf(path, sizeof(path), "%s/device.key", dir);
    int device_ok = !orbit_keyfile(path, a->device);
    snprintf(path, sizeof(path), "%s/server.key", dir);
    int key_ok = !orbit_keyfile(path, a->server_key);
    uint8_t fp[8];
    crypto_blake2b(fp, 8, a->server_key, 32);
    orbit_hex(a->server_id, fp, 8);
    AppLink *own = &a->links[0];
    own->used = 1;
    own->own = 1;
    own->code_group = -1;
    load_profile(a);
    if (!a->profile && suggested && orbit_text_valid(suggested, 1, 48))
        snprintf(a->name, sizeof(a->name), "%s", suggested);
    if (!a->name[0])
        snprintf(a->name, sizeof(a->name), "PS3-Spieler");
    a->store = calloc(1, sizeof(*a->store));
    snprintf(path, sizeof(path), "%s/data.json", dir);
    if (!device_ok || !key_ok || !a->store || orbit_store_open(a->store, path)) {
        free(a->store);
        a->store = NULL;
        notice_locked(a, "Speicher konnte nicht geöffnet werden. Festplatte prüfen.");
    } else {
        memcpy(a->store->server_id, a->server_id, 17);
        a->server = orbit_server_start(a->store, a->server_key, a->port);
        if (!a->server)
            notice_locked(a, "Dein Treffpunkt konnte nicht starten (Port belegt?). App neu "
                             "starten.");
    }
    if (pthread_create(&a->manager, NULL, manager_main, a)) {
        pthread_mutex_destroy(&a->lock);
        return -1;
    }
    LOCK(a);
    for (int i = 0; i < APP_LINKS; i++)
        if (a->links[i].used && (i || a->server))
            start_link_locked(a, &a->links[i]);
    UNLOCK(a);
    if (a->relay[0])
        relay_restart(a);
    return 0;
}
void app_set_netinfo(OrbitApp *a, AppNetInfo fn) {
    LOCK(a);
    a->netinfo = fn;
    UNLOCK(a);
}
void app_stop(OrbitApp *a) {
    LOCK(a);
    a->stop = 1;
    if (a->dirty)
        save_profile_locked(a);
    int call = a->call_joinable;
    a->call_joinable = 0;
    UNLOCK(a);
    if (call)
        pthread_join(a->call_thread, NULL);
    pthread_join(a->manager, NULL);
    for (int i = 0; i < APP_LINKS; i++) {
        LOCK(a);
        int join = a->links[i].thread_joinable;
        a->links[i].thread_joinable = 0;
        UNLOCK(a);
        if (join)
            pthread_join(a->links[i].thread, NULL);
        cJSON_Delete(a->links[i].job);
        a->links[i].job = NULL;
    }
    LOCK(a);
    if (a->dirty)
        save_profile_locked(a);
    UNLOCK(a);
    orbit_bore_stop(a->bore);
    a->bore = NULL;
    orbit_relay_host_stop(a->relay_host);
    a->relay_host = NULL;
    orbit_nat_close(&a->nat, a->gateway);
    orbit_server_stop(a->server);
    if (a->store) {
        orbit_store_close(a->store);
        free(a->store);
    }
    crypto_wipe(a->device, 32);
    crypto_wipe(a->server_key, 32);
    pthread_mutex_destroy(&a->lock);
}
/* ---------- UI side ---------- */
void app_snapshot(OrbitApp *a, UiState *s) {
    LOCK(a);
    /* Who is talking changes many times per second: compare it cheaply on every frame. */
    uint32_t speak = 0;
    int mic = 0;
    if (a->call_running) {
        const AppLink *CL = &a->links[a->call_link];
        for (int i = 0; i < a->call_count; i++) {
            int self = a->call_people[i].user == CL->user;
            if (self ? voice_self_speaking() : voice_speaking(a->call_people[i].user))
                speak |= 1u << i;
        }
        speak |= (uint32_t)voice_muted() << 31;
        mic = voice_mic_level() / 25;
    }
    if (s->revision && a->changed == s->revision && speak == s->speak_mask && mic == s->call_mic) {
        UNLOCK(a);
        return;
    }
    s->speak_mask = speak;
    s->call_mic = mic;
    s->call_state = a->call_running ? a->call_state : CALL_OFF;
    s->call_muted = voice_muted();
    s->call_count = a->call_running ? a->call_count : 0;
    snprintf(s->call_name, sizeof(s->call_name), "%s", a->call_name);
    snprintf(s->call_note, sizeof(s->call_note), "%s", a->call_note);
    for (int i = 0; i < s->call_count; i++) {
        snprintf(s->call_people[i].name, 49, "%s", a->call_people[i].name);
        s->call_people[i].self = a->call_people[i].user == a->links[a->call_link].user;
        s->call_people[i].speaking = (speak >> i) & 1;
    }
    s->revision = a->changed ? a->changed : 1;
    s->profile = a->profile;
    s->busy = a->busy;
    s->sent_count = a->sent_count;
    s->net = a->net;
    s->relay_online = a->relay_online > 0;
    s->tunnel_online = a->bore_online;
    if (a->bore && a->bore_port)
        snprintf(s->tunnel, sizeof(s->tunnel), "%s:%d", a->bore_host, a->bore_port);
    else
        snprintf(s->tunnel, sizeof(s->tunnel), "%s", a->bore ? "wird aufgebaut ..." : "aus");
    snprintf(s->relay, sizeof(s->relay), "%s", a->relay);
    s->invite_state = a->invite_state;
    s->status_id = a->status_id;
    snprintf(s->status, sizeof(s->status), "%s", a->status);
    snprintf(s->username, sizeof(s->username), "%s", a->name);
    snprintf(s->invite_code, sizeof(s->invite_code), "%s", a->invite_code);
    snprintf(s->invite_group, sizeof(s->invite_group), "%s", a->invite_group);
    snprintf(s->invite_scope, sizeof(s->invite_scope), "%s", a->invite_scope);
    snprintf(s->net_detail, sizeof(s->net_detail), "%s", a->net_detail);
    snprintf(s->local_ip, sizeof(s->local_ip), "%s", a->ip);
    snprintf(s->public_ip, sizeof(s->public_ip), "%s", a->nat.ok ? a->nat.external : "");
    snprintf(s->server_id, sizeof(s->server_id), "%s", a->server_id);
    s->group_count = 0;
    s->selected = -1;
    s->link_error[0] = 0;
    for (int i = 0; i < APP_LINKS; i++) {
        const AppLink *L = &a->links[i];
        if (!L->used || L->remove)
            continue;
        for (int g = 0; g < L->group_count && s->group_count < UI_GROUPS; g++) {
            UiGroup *u = &s->groups[s->group_count];
            u->link = i;
            u->id = L->groups[g].id;
            u->own = L->own;
            u->state = L->state;
            u->members = L->groups[g].members;
            u->unread = L->groups[g].last > a->seen[i][u->id] &&
                        !(a->selected_link == i && a->selected_group == u->id);
            memcpy(u->name, L->groups[g].name, sizeof(u->name));
            memcpy(u->host, L->groups[g].host, sizeof(u->host));
            memcpy(u->people, L->groups[g].people, sizeof(u->people));
            u->call = L->groups[g].call;
            if (a->selected_link == i && a->selected_group == u->id)
                s->selected = s->group_count;
            s->group_count++;
        }
    }
    s->call_here = s->call_state != CALL_OFF && a->call_link == a->selected_link &&
                   a->call_group == a->selected_group;
    if (s->selected < 0 && s->group_count) {
        /* Selected group vanished (left / deleted) or nothing chosen yet. */
        s->selected = 0;
        a->selected_link = s->groups[0].link;
        a->selected_group = s->groups[0].id;
    }
    s->message_count = 0;
    s->loading = 0;
    if (s->selected >= 0) {
        const AppLink *L = &a->links[s->groups[s->selected].link];
        if (L->history_group == s->groups[s->selected].id) {
            s->message_count = L->message_count;
            memcpy(s->messages, L->messages, sizeof(UiMessage) * (size_t)L->message_count);
        } else
            s->loading = L->state == LINK_ONLINE || L->state == LINK_CONNECTING;
        if (L->state != LINK_ONLINE)
            snprintf(s->link_error, sizeof(s->link_error), "%s", L->error);
    }
    UNLOCK(a);
}
void app_select(OrbitApp *a, int dir) {
    UiState *tmp = malloc(sizeof(*tmp));
    if (!tmp)
        return;
    memset(tmp, 0, sizeof(*tmp));
    app_snapshot(a, tmp);
    if (tmp->group_count > 1) {
        int next = (tmp->selected + dir + tmp->group_count) % tmp->group_count;
        LOCK(a);
        a->selected_link = tmp->groups[next].link;
        a->selected_group = tmp->groups[next].id;
        a->dirty = 1;
        a->changed++;
        UNLOCK(a);
    }
    free(tmp);
}
static int queue(OrbitApp *a, int link, cJSON *job) {
    LOCK(a);
    AppLink *L = link >= 0 && link < APP_LINKS ? &a->links[link] : NULL;
    int ok = L && L->used && !L->remove && !L->job && !a->busy && L->state == LINK_ONLINE;
    if (ok) {
        L->job = job;
        a->busy = 1;
        a->busy_since = orbit_millis();
        a->changed++;
    } else if (L && L->used && L->state == LINK_CONNECTING)
        notice_locked(a, "Verbindung wird gerade aufgebaut. Gleich nochmal versuchen.");
    else if (L && L->used && L->state != LINK_ONLINE)
        notice_locked(a, "Der Gastgeber dieser Gruppe ist gerade offline.");
    else if (a->busy)
        notice_locked(a, "Einen Moment ...");
    UNLOCK(a);
    if (!ok)
        cJSON_Delete(job);
    return ok ? 0 : -1;
}
static int selected(OrbitApp *a, int *link, int *gid) {
    LOCK(a);
    *link = a->selected_link;
    *gid = a->selected_group;
    int ok = *gid >= 0 && a->links[*link].used;
    UNLOCK(a);
    return ok ? 0 : -1;
}
int app_set_name(OrbitApp *a, const char *name) {
    if (!orbit_text_valid(name, 1, 48))
        return -1;
    LOCK(a);
    snprintf(a->name, sizeof(a->name), "%s", name);
    a->profile = 1;
    a->dirty = 1;
    a->changed++;
    for (int i = 0; i < APP_LINKS; i++)
        if (a->links[i].used)
            a->links[i].reauth = 1;
    UNLOCK(a);
    return 0;
}
int app_create_group(OrbitApp *a, const char *name) {
    if (!orbit_text_valid(name, 1, 64)) {
        app_notice(a, "Gruppenname: 1 bis 64 Zeichen.");
        return -1;
    }
    cJSON *q = command("create");
    cJSON_AddStringToObject(q, "name", name);
    return queue(a, 0, q);
}
int app_send(OrbitApp *a, const char *text) {
    int link, gid;
    if (selected(a, &link, &gid))
        return -1;
    cJSON *q = command("send");
    cJSON_AddNumberToObject(q, "group", gid);
    cJSON_AddStringToObject(q, "text", text);
    return queue(a, link, q);
}
int app_invite(OrbitApp *a) {
    int link, gid;
    if (selected(a, &link, &gid)) {
        app_notice(a, "Erstelle zuerst eine Gruppe.");
        return -1;
    }
    LOCK(a);
    a->invite_state = 1;
    a->invite_code[0] = 0;
    if (a->links[link].own && !a->nat.ok &&
        (a->net == NET_CHECKING || (a->bore && !a->bore_online))) {
        /* Wait for the router check / tunnel, otherwise the code would only work at home. */
        a->invite_wait = 1;
        a->invite_wait_since = orbit_millis();
        a->invite_wait_link = link;
        a->invite_wait_group = gid;
        a->changed++;
        UNLOCK(a);
        return 0;
    }
    UNLOCK(a);
    cJSON *q = command("invite");
    cJSON_AddNumberToObject(q, "group", gid);
    int rc = queue(a, link, q);
    if (rc) {
        LOCK(a);
        a->invite_state = 3;
        UNLOCK(a);
    }
    return rc;
}
int app_leave(OrbitApp *a) {
    int link, gid;
    if (selected(a, &link, &gid))
        return -1;
    LOCK(a);
    int own = a->links[link].own;
    UNLOCK(a);
    cJSON *q = command(own ? "delete" : "leave");
    cJSON_AddNumberToObject(q, "group", gid);
    return queue(a, link, q);
}
void app_retry(OrbitApp *a) {
    LOCK(a);
    a->nat_now = 1;
    for (int i = 0; i < APP_LINKS; i++)
        a->links[i].retry_at = 0;
    UNLOCK(a);
}
int app_join(OrbitApp *a, const char *text) {
    uint32_t ip;
    int port;
    char code[9];
    if (orbit_invite_decode(text, &ip, &port, code)) {
        app_notice(a, "Einladung nicht erkannt. Bitte die 20 Zeichen genau abtippen.");
        return -1;
    }
    char host[16], relay[24] = "";
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255,
             ip & 255);
    if (port & ORBIT_INVITE_RELAY) {
        /* Invitation through a relay: the code carries the relay address itself. */
        snprintf(relay, sizeof(relay), "%s:%d", host, port & 0xffff);
        snprintf(host, sizeof(host), "0.0.0.0");
        port = 1;
        ip = 0;
        LOCK(a);
        int adopt = !a->relay[0];
        UNLOCK(a);
        if (adopt)
            app_set_relay(a, relay, 1); /* now this console can invite through it, too */
    }
    char tag[17], local[9];
    orbit_group_tag(tag, code);
    if (a->store && !orbit_store_code_for_tag(a->store, tag, local)) {
        /* My own invitation: join locally (already the host). */
        LOCK(a);
        snprintf(a->links[0].join_code, sizeof(a->links[0].join_code), "%s", code);
        UNLOCK(a);
        return 0;
    }
    LOCK(a);
    int slot = -1;
    for (int i = 1; i < APP_LINKS; i++) {
        AppLink *L = &a->links[i];
        if (L->used && !L->remove && ip && !strcmp(L->host, host) && L->port == port) {
            snprintf(L->join_code, sizeof(L->join_code), "%s", code);
            L->retry_at = 0;
            notice_locked(a, "Einladung wird angenommen ...");
            UNLOCK(a);
            return 0;
        }
        if (!L->used && !L->running && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        notice_locked(a, "Zu viele Treffpunkte. Verlasse zuerst eine fremde Gruppe.");
        UNLOCK(a);
        return -1;
    }
    AppLink *L = &a->links[slot];
    memset(L, 0, sizeof(*L));
    L->used = 1;
    L->port = port;
    L->code_group = -1;
    snprintf(L->host, sizeof(L->host), "%s", host);
    snprintf(L->relay, sizeof(L->relay), "%s", relay);
    snprintf(L->code, sizeof(L->code), "%s", code);
    snprintf(L->join_code, sizeof(L->join_code), "%s", code);
    start_link_locked(a, L);
    a->dirty = 1;
    notice_locked(a, "Verbinde mit deinem Freund ...");
    UNLOCK(a);
    return 0;
}
/* ---------- voice chat ---------- */
static void call_state_set(OrbitApp *a, int state) {
    LOCK(a);
    if (a->call_state != state) {
        a->call_state = state;
        a->changed++;
    }
    UNLOCK(a);
}
static void *call_main(void *arg) {
    OrbitApp *a = arg;
    char message[200];
    if (voice_device_start(message, sizeof(message))) {
        LOCK(a);
        a->call_running = 0;
        a->call_state = CALL_OFF;
        notice_locked(a, message);
        UNLOCK(a);
        return NULL;
    }
    LOCK(a);
    snprintf(a->call_note, sizeof(a->call_note), "%s", message);
    if (message[0])
        notice_locked(a, message);
    UNLOCK(a);
    OrbitLink *c = NULL;
    uint64_t retry = 0, ping = 0;
    uint8_t packet[VOICE_PACKET + 8];
    for (;;) {
        LOCK(a);
        int stop = a->stop || a->call_leave, link = a->call_link, gid = a->call_group;
        UNLOCK(a);
        if (stop)
            break;
        uint64_t now = orbit_millis();
        if (!c) {
            if (now < retry) {
                usleep(50000);
                continue;
            }
            c = link_open(a, &a->links[link], 1);
            if (c) {
                cJSON *q = command("voice");
                cJSON_AddNumberToObject(q, "group", gid);
                cJSON *r = orbit_exchange(c, q);
                cJSON_Delete(q);
                if (!r || !success(r)) {
                    if (r)
                        app_notice(a, orbit_string(r, "error"));
                    orbit_disconnect(c);
                    c = NULL;
                }
                cJSON_Delete(r);
            }
            if (!c) {
                call_state_set(a, CALL_RECONNECTING);
                retry = orbit_millis() + 2000;
                continue;
            }
            call_state_set(a, CALL_ONLINE);
            LOCK(a);
            a->links[link].retry_at = 0;
            UNLOCK(a);
        }
        int bad = 0;
        while (!bad && voice_take_packet(packet))
            bad = orbit_frame_send(c, packet, VOICE_PACKET);
        if (!bad && now >= ping) {
            uint8_t p = VF_PING;
            bad = orbit_frame_send(c, &p, 1);
            ping = now + 2000;
        }
        for (int i = 0; i < 8 && !bad; i++) {
            int n = orbit_frame_recv(c, packet, sizeof(packet), i ? 0 : 8);
            if (n < 0)
                bad = 1;
            else if (!n)
                break;
            else
                voice_receive(packet, n);
        }
        if (bad) {
            orbit_disconnect(c);
            c = NULL;
            call_state_set(a, CALL_RECONNECTING);
            retry = orbit_millis() + 1000;
        }
    }
    orbit_disconnect(c);
    voice_device_stop();
    LOCK(a);
    a->call_running = 0;
    a->call_state = CALL_OFF;
    a->call_count = 0;
    a->call_note[0] = 0;
    a->changed++;
    UNLOCK(a);
    return NULL;
}
int app_call_toggle(OrbitApp *a) {
    LOCK(a);
    if (a->call_running) {
        a->call_leave = 1;
        notice_locked(a, "Sprachchat verlassen.");
        UNLOCK(a);
        return 0;
    }
    int joinable = a->call_joinable;
    a->call_joinable = 0;
    UNLOCK(a);
    if (joinable)
        pthread_join(a->call_thread, NULL); /* previous call has already ended */
    int link, gid;
    if (selected(a, &link, &gid)) {
        app_notice(a, "Erstelle zuerst eine Gruppe.");
        return -1;
    }
    LOCK(a);
    if (a->links[link].state != LINK_ONLINE) {
        notice_locked(a, "Der Gastgeber dieser Gruppe ist gerade offline.");
        UNLOCK(a);
        return -1;
    }
    a->call_link = link;
    a->call_group = gid;
    a->call_leave = 0;
    a->call_count = 0;
    a->call_state = CALL_CONNECTING;
    a->call_name[0] = 0;
    for (int i = 0; i < a->links[link].group_count; i++)
        if (a->links[link].groups[i].id == gid)
            snprintf(a->call_name, sizeof(a->call_name), "%s", a->links[link].groups[i].name);
    a->call_running = 1;
    voice_set_muted(0);
    int ok = !pthread_create(&a->call_thread, NULL, call_main, a);
    if (ok)
        a->call_joinable = 1;
    else {
        a->call_running = 0;
        a->call_state = CALL_OFF;
    }
    a->changed++;
    UNLOCK(a);
    return ok ? 0 : -1;
}
void app_call_mute(OrbitApp *a) {
    LOCK(a);
    if (a->call_running) {
        voice_set_muted(!voice_muted());
        notice_locked(a, voice_muted() ? "Mikrofon stumm." : "Mikrofon an.");
    }
    UNLOCK(a);
}
/* ---------- relay ("Vermittler") ---------- */
static void relay_restart(OrbitApp *a) {
    LOCK(a);
    OrbitRelayHost *old = a->relay_host;
    a->relay_host = NULL;
    char relay[104];
    snprintf(relay, sizeof(relay), "%s", a->relay);
    UNLOCK(a);
    orbit_relay_host_stop(old);
    OrbitRelayHost *h = NULL;
    if (relay[0] && a->store && a->server)
        h = orbit_relay_host_start(relay, a->server_id, a->server_key, a->store, a->port);
    /* The invitation carries the relay as IPv4: resolve a name once, here. */
    uint32_t rip = 0;
    int rport = ORBIT_RELAY_PORT;
    char rhost[ORBIT_HOST];
    if (relay[0] && !orbit_parse_endpoint(relay, rhost, &rport)) {
        if (!strchr(relay, ':'))
            rport = ORBIT_RELAY_PORT;
        rip = orbit_resolve(rhost);
    }
    LOCK(a);
    a->relay_ip = rport > 0 && rport <= 65535 ? rip : 0;
    a->relay_port = rport;
    a->relay_host = h;
    a->relay_online = -1;
    for (int i = 0; i < APP_LINKS; i++)
        a->links[i].retry_at = 0;
    a->changed++;
    UNLOCK(a);
}
void app_set_relay(OrbitApp *a, const char *relay, int user) {
    char host[ORBIT_HOST];
    int port;
    if (relay && relay[0] && orbit_parse_endpoint(relay, host, &port)) {
        app_notice(a, "Vermittler-Adresse ungültig (Beispiel: 203.0.113.5 oder name.de:31400).");
        return;
    }
    LOCK(a);
    if (!user && a->relay_user) {
        UNLOCK(a);
        if (!a->relay_host)
            relay_restart(a);
        return; /* the user's own choice wins over the package default */
    }
    snprintf(a->relay, sizeof(a->relay), "%s", relay ? relay : "");
    if (user) {
        a->relay_user = 1;
        a->dirty = 1;
    }
    UNLOCK(a);
    relay_restart(a);
}
/* ---------- built-in internet tunnel ---------- */
void app_set_tunnel(OrbitApp *a, const char *server) {
    LOCK(a);
    OrbitBore *old = a->bore;
    a->bore = NULL;
    a->bore_online = a->bore_port = 0;
    UNLOCK(a);
    orbit_bore_stop(old);
    if (!server || !server[0] || !a->server)
        return;
    /* A fixed public port per console keeps invitation codes valid across restarts. */
    unsigned long h = strtoul((char[9]){a->server_id[0], a->server_id[1], a->server_id[2],
                                        a->server_id[3], a->server_id[4], a->server_id[5],
                                        a->server_id[6], a->server_id[7], 0},
                              NULL, 16);
    int want = 20000 + (int)(h % 40000);
    OrbitBore *b = orbit_bore_start(server, want, a->port);
    char host[ORBIT_HOST];
    int port;
    LOCK(a);
    a->bore = b;
    if (!orbit_parse_endpoint(server, host, &port))
        snprintf(a->bore_host, sizeof(a->bore_host), "%s", host);
    a->changed++;
    UNLOCK(a);
}

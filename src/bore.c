/* Built-in internet tunnel for the host PS3: speaks the open bore protocol
 * (github.com/ekzhang/bore, MIT) with a public bore server such as bore.pub.
 * The console connects outwards only, so no router settings and no PC are needed.
 * Every visitor arrives as a new data connection and is spliced to the local Orbit server;
 * TLS stays end to end between the consoles. */
#include "orbit.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __PPU__
#include <net/net.h>
#include <net/poll.h>
#else
#include <poll.h>
#include <sys/socket.h>
#endif
#define BORE_CONTROL 7835
#define BORE_SPLICES 24
typedef struct {
    pthread_t thread;
    int used, done;
    struct OrbitBore *bore;
    char id[48];
} BoreSplice;
struct OrbitBore {
    pthread_t thread;
    pthread_mutex_t lock;
    int stop, online, port, want, local_port, control_port;
    uint32_t ip;
    char host[ORBIT_HOST], reason[200];
    BoreSplice splices[BORE_SPLICES];
};
static int wait_io(int fd, int writing, int ms) {
    struct pollfd p = {fd, writing ? POLLOUT : POLLIN, 0};
    return poll(&p, 1, ms) > 0 && (p.revents & (writing ? POLLOUT : POLLIN | POLLHUP | POLLERR));
}
static int send_all(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n) {
        if (!wait_io(fd, 1, 5000))
            return -1;
        int k = (int)send(fd, p, n, 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}
/* One 0-terminated JSON message, read byte by byte so nothing after it is consumed. */
static cJSON *read_msg(int fd, int ms) {
    char buf[512];
    size_t n = 0;
    uint64_t end = orbit_millis() + (uint64_t)ms;
    while (n + 1 < sizeof(buf)) {
        uint64_t now = orbit_millis();
        if (now >= end || !wait_io(fd, 0, (int)(end - now)))
            return NULL;
        char c;
        if (recv(fd, &c, 1, 0) != 1)
            return NULL;
        if (!c) {
            buf[n] = 0;
            return cJSON_Parse(buf);
        }
        buf[n++] = c;
    }
    return NULL;
}
static int send_msg(int fd, const char *json) {
    return send_all(fd, json, strlen(json) + 1); /* includes the terminating 0 */
}
static int stopped(struct OrbitBore *b) {
    pthread_mutex_lock(&b->lock);
    int s = b->stop;
    pthread_mutex_unlock(&b->lock);
    return s;
}
static void *splice_main(void *arg) {
    BoreSplice *s = arg;
    struct OrbitBore *b = s->bore;
    int a = orbit_tcp_connect(b->host, b->control_port, NULL, 0), c = -1;
    if (a >= 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"Accept\":\"%s\"}", s->id);
        if (!send_msg(a, msg))
            c = orbit_tcp_connect("127.0.0.1", b->local_port, NULL, 0);
    }
    char *buf = malloc(16384);
    while (buf && a >= 0 && c >= 0 && !stopped(b)) {
        struct pollfd p[2] = {{a, POLLIN, 0}, {c, POLLIN, 0}};
        int r = poll(p, 2, 500);
        if (r < 0)
            break;
        if (!r)
            continue;
        int bad = 0;
        for (int i = 0; i < 2 && !bad; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int from = i ? c : a, to = i ? a : c;
            int k = (int)recv(from, buf, 16384, 0);
            bad = k <= 0 || send_all(to, buf, (size_t)k);
        }
        if (bad)
            break;
    }
    free(buf);
    orbit_socket_close(a);
    orbit_socket_close(c);
    pthread_mutex_lock(&b->lock);
    s->done = 1;
    pthread_mutex_unlock(&b->lock);
    return NULL;
}
static void reap(struct OrbitBore *b, int all) {
    for (int i = 0; i < BORE_SPLICES; i++) {
        pthread_mutex_lock(&b->lock);
        int join = b->splices[i].used && (b->splices[i].done || all);
        pthread_mutex_unlock(&b->lock);
        if (join) {
            pthread_join(b->splices[i].thread, NULL);
            pthread_mutex_lock(&b->lock);
            b->splices[i].used = 0;
            pthread_mutex_unlock(&b->lock);
        }
    }
}
static void accept_visitor(struct OrbitBore *b, const char *id) {
    reap(b, 0);
    pthread_mutex_lock(&b->lock);
    BoreSplice *s = NULL;
    for (int i = 0; i < BORE_SPLICES && !s; i++)
        if (!b->splices[i].used)
            s = &b->splices[i];
    if (s) {
        memset(s, 0, sizeof(*s));
        s->bore = b;
        snprintf(s->id, sizeof(s->id), "%s", id);
        s->used = !pthread_create(&s->thread, NULL, splice_main, s);
    }
    pthread_mutex_unlock(&b->lock);
}
static void set_state(struct OrbitBore *b, int online, int port, const char *reason) {
    pthread_mutex_lock(&b->lock);
    b->online = online;
    b->port = port;
    snprintf(b->reason, sizeof(b->reason), "%s", reason);
    pthread_mutex_unlock(&b->lock);
}
static void pause_ms(struct OrbitBore *b, int ms) {
    for (int t = 0; t < ms && !stopped(b); t += 100)
        usleep(100000);
}
static void *bore_main(void *arg) {
    struct OrbitBore *b = arg;
    int delay = 2000, want = b->want;
    uint64_t taken_since = 0; /* our fixed port is still held by a previous session */
    while (!stopped(b)) {
        if (!b->ip) {
            uint32_t ip = orbit_resolve(b->host);
            pthread_mutex_lock(&b->lock);
            b->ip = ip;
            pthread_mutex_unlock(&b->lock);
        }
        int fd = orbit_tcp_connect(b->host, b->control_port, NULL, 0);
        if (fd < 0) {
            set_state(b, 0, 0, "Internet-Tunnel nicht erreichbar. Neuer Versuch läuft ...");
            pause_ms(b, delay);
            delay = delay * 2 > 30000 ? 30000 : delay * 2;
            continue;
        }
        char hello[48];
        snprintf(hello, sizeof(hello), "{\"Hello\":%d}", want);
        cJSON *m = send_msg(fd, hello) ? NULL : read_msg(fd, 15000);
        while (m && cJSON_IsString(m)) { /* skip heartbeats */
            cJSON_Delete(m);
            m = read_msg(fd, 15000);
        }
        cJSON *port = cJSON_GetObjectItem(m, "Hello"), *err = cJSON_GetObjectItem(m, "Error");
        if (!cJSON_IsNumber(port) || port->valueint <= 0 || port->valueint > 65535) {
            if (cJSON_IsString(err) && want) {
                /* After a crash the server may hold our old port for a while. Keep asking for it
                 * for two minutes so invitations stay valid, then take any free port. */
                if (!taken_since)
                    taken_since = orbit_millis();
                if (orbit_millis() - taken_since > 120000) {
                    want = 0;
                    delay = 0;
                } else
                    delay = 5000;
            }
            cJSON_Delete(m);
            orbit_socket_close(fd);
            set_state(b, 0, 0, "Internet-Tunnel abgelehnt. Neuer Versuch läuft ...");
            pause_ms(b, delay);
            delay = delay * 2 > 30000 ? 30000 : (delay ? delay * 2 : 2000);
            continue;
        }
        taken_since = 0;
        set_state(b, 1, port->valueint, "");
        cJSON_Delete(m);
        delay = 2000;
        while (!stopped(b)) {
            if (!wait_io(fd, 0, 300))
                continue;
            m = read_msg(fd, 60000);
            if (!m)
                break;
            cJSON *conn = cJSON_GetObjectItem(m, "Connection");
            if (cJSON_IsString(conn) && strlen(conn->valuestring) < 48)
                accept_visitor(b, conn->valuestring);
            cJSON_Delete(m);
        }
        orbit_socket_close(fd);
        set_state(b, 0, 0, "Internet-Tunnel unterbrochen. Neuer Versuch läuft ...");
        pause_ms(b, 1000);
    }
    reap(b, 1);
    return NULL;
}
OrbitBore *orbit_bore_start(const char *server, int want_port, int local_port) {
    char host[ORBIT_HOST];
    int port;
    if (!server || orbit_parse_endpoint(server, host, &port))
        return NULL;
    struct OrbitBore *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    pthread_mutex_init(&b->lock, NULL);
    snprintf(b->host, sizeof(b->host), "%s", host);
    b->control_port = strchr(server, ':') ? port : BORE_CONTROL;
    b->want = want_port;
    b->local_port = local_port;
    snprintf(b->reason, sizeof(b->reason), "Internet-Tunnel wird aufgebaut ...");
    if (pthread_create(&b->thread, NULL, bore_main, b)) {
        pthread_mutex_destroy(&b->lock);
        free(b);
        return NULL;
    }
    return b;
}
int orbit_bore_state(OrbitBore *b, uint32_t *ip, int *port, char *host, size_t host_cap,
                     char *reason, size_t cap) {
    if (!b)
        return 0;
    pthread_mutex_lock(&b->lock);
    int online = b->online && b->ip;
    if (ip)
        *ip = b->ip;
    if (port)
        *port = b->port;
    if (host && host_cap)
        snprintf(host, host_cap, "%s", b->host);
    if (reason && cap)
        snprintf(reason, cap, "%s", b->reason);
    pthread_mutex_unlock(&b->lock);
    return online;
}
void orbit_bore_stop(OrbitBore *b) {
    if (!b)
        return;
    pthread_mutex_lock(&b->lock);
    b->stop = 1;
    pthread_mutex_unlock(&b->lock);
    pthread_join(b->thread, NULL);
    pthread_mutex_destroy(&b->lock);
    free(b);
}

/* Relay client ("Vermittler"): works behind any router without configuration, because both
 * consoles only connect outwards. TLS is end to end; the relay just splices TCP streams. */
#include "monocypher.h"
#include "orbit.h"
#include <arpa/inet.h>
#include <errno.h>
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
#define SPLICES 24
typedef struct {
    pthread_t thread;
    int used, done, a, b;
    struct OrbitRelayHost *host;
    char id[32];
} Splice;
struct OrbitRelayHost {
    pthread_t thread;
    pthread_mutex_t lock;
    int stop, online, local_port, relay_port;
    char reason[200], relay_host[ORBIT_HOST], sid[17], secret[65];
    char active_host[ORBIT_HOST]; /* relay actually used: found in the home network or configured */
    int active_port;
    OrbitStore *store;
    Splice splices[SPLICES];
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
/* Reads exactly one line, byte by byte, so nothing after the newline is consumed. */
static int read_line(int fd, char *buf, size_t cap, int ms) {
    size_t n = 0;
    uint64_t end = orbit_millis() + (uint64_t)ms;
    while (n + 1 < cap) {
        uint64_t now = orbit_millis();
        if (now >= end || !wait_io(fd, 0, (int)(end - now)))
            return -1;
        char c;
        int k = (int)recv(fd, &c, 1, 0);
        if (k <= 0)
            return -1;
        if (c == '\n') {
            buf[n] = 0;
            return (int)n;
        }
        if (c != '\r')
            buf[n++] = c;
    }
    return -1;
}
static int relay_endpoint(const char *relay, char host[ORBIT_HOST], int *port) {
    if (!relay || !relay[0] || orbit_parse_endpoint(relay, host, port))
        return -1;
    if (!strchr(relay, ':'))
        *port = ORBIT_RELAY_PORT;
    return 0;
}
int orbit_relay_dial(const char *relay, const char tag[17], char *reason, size_t cap) {
    char host[ORBIT_HOST], line[64];
    int port;
    if (relay_endpoint(relay, host, &port)) {
        snprintf(reason, cap, "Kein Vermittler eingestellt.");
        return -1;
    }
    int fd = orbit_tcp_connect(host, port, reason, cap);
    if (fd < 0) {
        snprintf(reason, cap, "Vermittler nicht erreichbar. Ist der Orbit-Server an?");
        return -1;
    }
    char q[64];
    int n = snprintf(q, sizeof(q), "ORBIT-RELAY 1 JOIN %s\n", tag);
    if (send_all(fd, q, (size_t)n) || read_line(fd, line, sizeof(line), 12000) < 0 ||
        strcmp(line, "OK")) {
        snprintf(reason, cap, "%s",
                 !strcmp(line, "OFFLINE") ? "Die Gastgeber-PS3 ist gerade nicht online (Orbit dort "
                                            "geschlossen?). Es geht automatisch weiter."
                                          : "Vermittler antwortet nicht richtig.");
        orbit_socket_close(fd);
        return -1;
    }
    return fd;
}
static void *splice_main(void *arg) {
    Splice *s = arg;
    struct OrbitRelayHost *h = s->host;
    char host[ORBIT_HOST], q[80];
    int port, a = -1, b = -1;
    pthread_mutex_lock(&h->lock);
    snprintf(host, sizeof(host), "%s", h->active_host);
    port = h->active_port;
    pthread_mutex_unlock(&h->lock);
    if (host[0] && (a = orbit_tcp_connect(host, port, NULL, 0)) >= 0) {
        int n = snprintf(q, sizeof(q), "ORBIT-RELAY 1 ACCEPT %s\n", s->id);
        if (!send_all(a, q, (size_t)n))
            b = orbit_tcp_connect("127.0.0.1", h->local_port, NULL, 0);
    }
    char *buf = malloc(16384);
    while (buf && a >= 0 && b >= 0) {
        pthread_mutex_lock(&h->lock);
        int stop = h->stop;
        pthread_mutex_unlock(&h->lock);
        if (stop)
            break;
        struct pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
        int r = poll(p, 2, 500);
        if (r < 0)
            break;
        if (!r)
            continue;
        int bad = 0;
        for (int i = 0; i < 2 && !bad; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int from = i ? b : a, to = i ? a : b;
            int k = (int)recv(from, buf, 16384, 0);
            bad = k <= 0 || send_all(to, buf, (size_t)k);
        }
        if (bad)
            break;
    }
    free(buf);
    orbit_socket_close(a);
    orbit_socket_close(b);
    pthread_mutex_lock(&h->lock);
    s->done = 1;
    pthread_mutex_unlock(&h->lock);
    return NULL;
}
static void reap(struct OrbitRelayHost *h, int all) {
    for (int i = 0; i < SPLICES; i++) {
        pthread_mutex_lock(&h->lock);
        int join = h->splices[i].used && (h->splices[i].done || all);
        pthread_mutex_unlock(&h->lock);
        if (join) {
            pthread_join(h->splices[i].thread, NULL);
            pthread_mutex_lock(&h->lock);
            h->splices[i].used = 0;
            pthread_mutex_unlock(&h->lock);
        }
    }
}
static void accept_conn(struct OrbitRelayHost *h, const char *id) {
    reap(h, 0);
    pthread_mutex_lock(&h->lock);
    Splice *s = NULL;
    for (int i = 0; i < SPLICES && !s; i++)
        if (!h->splices[i].used)
            s = &h->splices[i];
    if (s) {
        memset(s, 0, sizeof(*s));
        s->host = h;
        snprintf(s->id, sizeof(s->id), "%s", id);
        s->used = !pthread_create(&s->thread, NULL, splice_main, s);
    }
    pthread_mutex_unlock(&h->lock);
}
static void tags_of(OrbitStore *store, char *out, size_t cap) {
    size_t n = (size_t)snprintf(out, cap, "TAGS");
    pthread_mutex_lock(&store->mutex);
    for (int i = 0; i < ORBIT_GROUPS && n + 18 < cap; i++) {
        const OrbitGroup *g = &store->data.group[i];
        if (!g->used || strlen(g->invite) != 8)
            continue;
        char tag[17];
        orbit_group_tag(tag, g->invite);
        n += (size_t)snprintf(out + n, cap - n, " %s", tag);
    }
    pthread_mutex_unlock(&store->mutex);
    snprintf(out + n, cap - n, "\n");
}
static void set_state(struct OrbitRelayHost *h, int online, const char *reason) {
    pthread_mutex_lock(&h->lock);
    h->online = online;
    snprintf(h->reason, sizeof(h->reason), "%s", reason);
    pthread_mutex_unlock(&h->lock);
}
static int stopped(struct OrbitRelayHost *h) {
    pthread_mutex_lock(&h->lock);
    int s = h->stop;
    pthread_mutex_unlock(&h->lock);
    return s;
}
static void pause_ms(struct OrbitRelayHost *h, int ms) {
    for (int t = 0; t < ms && !stopped(h); t += 100)
        usleep(100000);
}
/* A relay running on a PC in the same home network answers a UDP broadcast. Using it directly
 * avoids sending the host's traffic out to the tunnel service and back. */
int orbit_relay_discover(int port, char ip[16], unsigned timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    to.sin_addr.s_addr = htonl(0xffffffffu);
#ifdef __PPU__
    to.sin_len = sizeof(to);
#endif
    int found = -1;
    uint64_t end = orbit_millis() + timeout_ms, next = 0;
    while (found && orbit_millis() < end) {
        if (orbit_millis() >= next) {
            sendto(fd, "ORBIT-RELAY?", 12, 0, (struct sockaddr *)&to, sizeof(to));
            next = orbit_millis() + 250;
        }
        if (!wait_io(fd, 0, 100))
            continue;
        char buf[32];
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int n = (int)recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len);
        if (n == 12 && !memcmp(buf, "ORBIT-RELAY!", 12)) {
            uint32_t a = ntohl(from.sin_addr.s_addr);
            snprintf(ip, 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
            found = 0;
        }
    }
    orbit_socket_close(fd);
    return found;
}
static void *host_main(void *arg) {
    struct OrbitRelayHost *h = arg;
    int backoff = 2000;
    char host[ORBIT_HOST];
    int port;
    relay_endpoint(h->relay_host, host, &port);
    while (!stopped(h)) {
        char reason[200] = "", use_host[ORBIT_HOST], lan[16];
        int use_port = port;
        snprintf(use_host, sizeof(use_host), "%s", host);
        if (!orbit_relay_discover(ORBIT_RELAY_PORT, lan, 600)) {
            snprintf(use_host, sizeof(use_host), "%s", lan);
            use_port = ORBIT_RELAY_PORT;
        }
        pthread_mutex_lock(&h->lock);
        snprintf(h->active_host, sizeof(h->active_host), "%s", use_host);
        h->active_port = use_port;
        pthread_mutex_unlock(&h->lock);
        int fd = orbit_tcp_connect(use_host, use_port, reason, sizeof(reason));
        if (fd < 0) {
            set_state(h, 0, "Vermittler nicht erreichbar. Neuer Versuch läuft automatisch.");
            pause_ms(h, backoff);
            backoff = backoff * 2 > 10000 ? 10000 : backoff * 2;
            continue;
        }
        char line[160], q[200];
        int n = snprintf(q, sizeof(q), "ORBIT-RELAY 1 HOST %s %s\n", h->sid, h->secret);
        if (send_all(fd, q, (size_t)n) || read_line(fd, line, sizeof(line), 10000) < 0 ||
            strcmp(line, "OK")) {
            set_state(h, 0, !strncmp(line, "ERR ", 4) ? line + 4 : "Vermittler antwortet nicht.");
            orbit_socket_close(fd);
            pause_ms(h, 30000);
            continue;
        }
        backoff = 2000;
        set_state(h, 1, "");
        char tags[24 * 17 + 16], sent[sizeof(tags)] = "";
        uint64_t last_rx = orbit_millis(), tag_at = 0;
        while (!stopped(h)) {
            uint64_t now = orbit_millis();
            if (now >= tag_at) {
                tag_at = now + 2000;
                tags_of(h->store, tags, sizeof(tags));
                if (strcmp(tags, sent)) {
                    if (send_all(fd, tags, strlen(tags)))
                        break;
                    strcpy(sent, tags);
                }
            }
            if (now - last_rx > 80000)
                break; /* relay silent: reconnect */
            if (!wait_io(fd, 0, 300))
                continue;
            if (read_line(fd, line, sizeof(line), 5000) < 0)
                break;
            last_rx = orbit_millis();
            if (!strcmp(line, "PING")) {
                if (send_all(fd, "PONG\n", 5))
                    break;
            } else if (!strncmp(line, "CONN ", 5) && strlen(line + 5) < 32)
                accept_conn(h, line + 5);
        }
        orbit_socket_close(fd);
        set_state(h, 0, "Verbindung zum Vermittler unterbrochen. Neuer Versuch ...");
        pause_ms(h, 1000);
    }
    reap(h, 1);
    return NULL;
}
OrbitRelayHost *orbit_relay_host_start(const char *relay, const char server_id[17],
                                       const uint8_t key[32], OrbitStore *store, int local_port) {
    char host[ORBIT_HOST];
    int port;
    if (!store || relay_endpoint(relay, host, &port))
        return NULL;
    struct OrbitRelayHost *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    pthread_mutex_init(&h->lock, NULL);
    snprintf(h->relay_host, sizeof(h->relay_host), "%s", relay);
    snprintf(h->sid, sizeof(h->sid), "%s", server_id);
    uint8_t secret[32];
    crypto_blake2b_keyed(secret, 32, key, 32, (const uint8_t *)"orbit-relay", 11);
    orbit_hex(h->secret, secret, 32);
    crypto_wipe(secret, 32);
    h->store = store;
    h->local_port = local_port;
    snprintf(h->reason, sizeof(h->reason), "Verbinde mit dem Vermittler ...");
    if (pthread_create(&h->thread, NULL, host_main, h)) {
        pthread_mutex_destroy(&h->lock);
        free(h);
        return NULL;
    }
    return h;
}
int orbit_relay_host_state(OrbitRelayHost *h, char *reason, size_t cap) {
    if (!h) {
        if (cap)
            snprintf(reason, cap, "Kein Vermittler eingestellt.");
        return 0;
    }
    pthread_mutex_lock(&h->lock);
    int online = h->online;
    if (cap)
        snprintf(reason, cap, "%s", h->reason);
    pthread_mutex_unlock(&h->lock);
    return online;
}
void orbit_relay_host_stop(OrbitRelayHost *h) {
    if (!h)
        return;
    pthread_mutex_lock(&h->lock);
    h->stop = 1;
    pthread_mutex_unlock(&h->lock);
    pthread_join(h->thread, NULL);
    pthread_mutex_destroy(&h->lock);
    crypto_wipe(h->secret, sizeof(h->secret));
    free(h);
}

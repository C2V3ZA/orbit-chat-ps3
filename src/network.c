#include "monocypher.h"
#include "orbit.h"
#include "voice.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __PPU__
#include <net/net.h>
#include <net/netdb.h>
#define CLOSE(fd) netClose((fd) & ~SOCKET_FD_MASK)
#else
#include <netdb.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#define CLOSE close
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define PEERS 16
#define VOICE_QUEUE 24
struct OrbitLink {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    uint64_t deadline;
};
typedef struct {
    struct OrbitServer *server;
    pthread_t thread;
    int state, fd;
    int voice, vgroup, vuser, qhead, qcount;
    pthread_mutex_t qlock;
    uint8_t queue[VOICE_QUEUE][VOICE_PACKET];
} Peer;
typedef struct {
    char tag[17], code[9];
    uint8_t psk[32];
} PskCache;
struct OrbitServer {
    OrbitStore *store;
    uint8_t key[32];
    int fd, udp, stop, udp_running;
    pthread_t acceptor, discovery;
    pthread_mutex_t lock;
    Peer peers[PEERS];
    PskCache cache[ORBIT_GROUPS];
};
#define LEGACY_IDENTITY "orbit-ps3-v1"
static int random_bytes(void *ctx, unsigned char *out, size_t n) {
    (void)ctx;
    return orbit_random(out, n);
}
static int ready(int fd, int writing, unsigned ms) {
    struct pollfd p = {fd, writing ? POLLOUT : POLLIN, 0};
    return poll(&p, 1, (int)ms) > 0 && (p.revents & (writing ? POLLOUT : POLLIN));
}
static int bio_send(void *ctx, const unsigned char *buf, size_t n) {
    OrbitLink *l = ctx;
    if (orbit_millis() > l->deadline || !ready(l->fd, 1, 3000))
        return MBEDTLS_ERR_SSL_TIMEOUT;
    int k = (int)send(l->fd, buf, n, MSG_NOSIGNAL);
    return k > 0 ? k : MBEDTLS_ERR_NET_SEND_FAILED;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t n) {
    OrbitLink *l = ctx;
    if (orbit_millis() > l->deadline || !ready(l->fd, 0, 3000))
        return MBEDTLS_ERR_SSL_TIMEOUT;
    int k = (int)recv(l->fd, buf, n, 0);
    return k >= 0 ? k : MBEDTLS_ERR_NET_RECV_FAILED;
}
/* Server side: the client names either the legacy server key or a group invitation tag. */
static int psk_lookup(void *ctx, mbedtls_ssl_context *ssl, const unsigned char *id, size_t n) {
    OrbitServer *s = ctx;
    if (n == strlen(LEGACY_IDENTITY) && !memcmp(id, LEGACY_IDENTITY, n))
        return mbedtls_ssl_set_hs_psk(ssl, s->key, 32);
    if (n != 19 || memcmp(id, "g7:", 3))
        return -1;
    char tag[17], code[9];
    memcpy(tag, id + 3, 16);
    tag[16] = 0;
    if (orbit_store_code_for_tag(s->store, tag, code))
        return -1;
    uint8_t psk[32];
    int cached = 0;
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < ORBIT_GROUPS; i++)
        if (!strcmp(s->cache[i].tag, tag) && !strcmp(s->cache[i].code, code)) {
            memcpy(psk, s->cache[i].psk, 32);
            cached = 1;
        }
    pthread_mutex_unlock(&s->lock);
    if (!cached) {
        if (orbit_group_psk(psk, code))
            return -1;
        pthread_mutex_lock(&s->lock);
        int slot = 0;
        for (int i = 0; i < ORBIT_GROUPS; i++)
            if (!s->cache[i].tag[0] || !strcmp(s->cache[i].tag, tag)) {
                slot = i;
                break;
            }
        memcpy(s->cache[slot].tag, tag, 17);
        memcpy(s->cache[slot].code, code, 9);
        memcpy(s->cache[slot].psk, psk, 32);
        pthread_mutex_unlock(&s->lock);
    }
    int rc = mbedtls_ssl_set_hs_psk(ssl, psk, 32);
    crypto_wipe(psk, 32);
    crypto_wipe(code, sizeof(code));
    return rc;
}
static OrbitLink *wrap_any(int fd, const uint8_t key[32], const char *identity, OrbitServer *srv) {
    int server = srv != NULL;
    OrbitLink *l = calloc(1, sizeof(*l));
    if (!l) {
        CLOSE(fd);
        return NULL;
    }
    l->fd = fd;
    mbedtls_ssl_init(&l->ssl);
    mbedtls_ssl_config_init(&l->conf);
    static const int suites[] = {MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256, 0};
    if (mbedtls_ssl_config_defaults(&l->conf,
                                    server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
        goto fail;
    mbedtls_ssl_conf_rng(&l->conf, random_bytes, NULL);
    mbedtls_ssl_conf_min_version(&l->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&l->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_ciphersuites(&l->conf, suites);
    if (server)
        mbedtls_ssl_conf_psk_cb(&l->conf, psk_lookup, srv);
    else if (mbedtls_ssl_conf_psk(&l->conf, key, 32, (const unsigned char *)identity,
                                  strlen(identity)))
        goto fail;
    if (mbedtls_ssl_setup(&l->ssl, &l->conf))
        goto fail;
    mbedtls_ssl_set_bio(&l->ssl, l, bio_send, bio_recv, NULL);
    l->deadline = orbit_millis() + 7000;
    if (mbedtls_ssl_handshake(&l->ssl))
        goto fail;
    return l;
fail:
    orbit_disconnect(l);
    return NULL;
}
void orbit_disconnect(OrbitLink *l) {
    if (!l)
        return;
    shutdown(l->fd, SHUT_RDWR);
    CLOSE(l->fd);
    mbedtls_ssl_free(&l->ssl);
    mbedtls_ssl_config_free(&l->conf);
    crypto_wipe(l, sizeof(*l));
    free(l);
}
static int transfer(OrbitLink *l, unsigned char *buf, size_t n, int write) {
    while (n) {
        int k = write ? mbedtls_ssl_write(&l->ssl, buf, n) : mbedtls_ssl_read(&l->ssl, buf, n);
        if (k <= 0)
            return -1;
        buf += k;
        n -= (size_t)k;
    }
    return 0;
}
static int send_json(OrbitLink *l, const cJSON *j) {
    char *s = cJSON_PrintUnformatted(j);
    if (!s)
        return -1;
    size_t n = strlen(s);
    unsigned char h[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16),
                          (unsigned char)(n >> 8), (unsigned char)n};
    int bad =
        n < 2 || n > ORBIT_FRAME || transfer(l, h, 4, 1) || transfer(l, (unsigned char *)s, n, 1);
    crypto_wipe(s, n);
    free(s);
    return bad ? -1 : 0;
}
static cJSON *recv_json(OrbitLink *l) {
    unsigned char h[4];
    if (transfer(l, h, 4, 0))
        return NULL;
    size_t n = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    if (n < 2 || n > ORBIT_FRAME)
        return NULL;
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    s[n] = 0;
    cJSON *j = NULL;
    if (!transfer(l, (unsigned char *)s, n, 0) && !memchr(s, 0, n))
        j = cJSON_ParseWithOpts(s, NULL, 1);
    crypto_wipe(s, n);
    free(s);
    return j;
}
cJSON *orbit_exchange(OrbitLink *l, const cJSON *req) {
    if (!l)
        return NULL;
    l->deadline = orbit_millis() + 7000;
    if (send_json(l, req))
        return NULL;
    return recv_json(l);
}
OrbitLink *orbit_connect(const char *ip, int port, const uint8_t key[32]) {
    return orbit_connect_detailed(ip, port, key, NULL, 0);
}
static OrbitLink *connect_as(const char *ip, int port, const uint8_t key[32], const char *identity,
                             char *reason, size_t capacity);
OrbitLink *orbit_connect_detailed(const char *ip, int port, const uint8_t key[32], char *reason,
                                  size_t capacity) {
    return connect_as(ip, port, key, LEGACY_IDENTITY, reason, capacity);
}
OrbitLink *orbit_connect_group(const char *ip, int port, const char *code, char *reason,
                               size_t capacity) {
    char norm[33], identity[20] = "g7:";
    uint8_t psk[32];
    if (orbit_group_code(norm, code) || strlen(norm) != 8 || orbit_group_psk(psk, norm)) {
        if (reason && capacity)
            snprintf(reason, capacity, "Einladung ungültig.");
        return NULL;
    }
    orbit_group_tag(identity + 3, norm);
    OrbitLink *l = connect_as(ip, port, psk, identity, reason, capacity);
    crypto_wipe(psk, 32);
    return l;
}
int orbit_tcp_connect(const char *ip, int port, char *reason, size_t capacity) {
#define WHY(message)                                                                               \
    do {                                                                                           \
        if (reason && capacity)                                                                    \
            snprintf(reason, capacity, "%s", message);                                             \
    } while (0)
    WHY("Netzwerk konnte nicht geöffnet werden.");
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        static pthread_mutex_t dns_lock = PTHREAD_MUTEX_INITIALIZER;
        char checked[ORBIT_HOST];
        int ignored_port;
        if (orbit_parse_endpoint(ip, checked, &ignored_port) || strchr(ip, ':'))
            return -1;
        pthread_mutex_lock(&dns_lock);
        struct hostent *entry = gethostbyname(ip);
        int valid =
            entry && entry->h_addrtype == AF_INET && entry->h_length == 4 && entry->h_addr_list[0];
        if (valid)
            memcpy(&addr.sin_addr, entry->h_addr_list[0], 4);
        pthread_mutex_unlock(&dns_lock);
        if (!valid) {
            WHY("DNS-Name konnte nicht aufgeloest werden. Gastgeberadresse prüfen.");
            return -1;
        }
    }
    uint32_t ipv4 = ntohl(addr.sin_addr.s_addr);
    int private_ip = (ipv4 >> 24) == 10 || (ipv4 >> 20) == 0xac1 || (ipv4 >> 16) == 0xc0a8 ||
                     (ipv4 >> 24) == 127;
    WHY(private_ip ? "Nicht erreichbar. Die Gastgeber-PS3 ist aus oder in einem anderen Heimnetz."
                   : "Nicht erreichbar. Die Gastgeber-PS3 ist aus, oder ihr Router hat die "
                     "automatische Freigabe nicht erlaubt.");
#ifdef __PPU__
    addr.sin_len = sizeof(addr);
#endif
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    /* Explicit nonblocking connect bounds shutdown even when the host is offline. */
#ifdef __PPU__
    int nonblock = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NBIO, &nonblock, sizeof(nonblock))) {
        CLOSE(fd);
        return -1;
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        CLOSE(fd);
        return -1;
    }
#endif
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        CLOSE(fd);
        return -1;
    }
    if (rc) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (!ready(fd, 1, 3000) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) || error) {
            CLOSE(fd);
            return -1;
        }
    }
#ifdef __PPU__
    nonblock = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_NBIO, &nonblock, sizeof(nonblock))) {
        CLOSE(fd);
        return -1;
    }
#else
    if (fcntl(fd, F_SETFL, flags)) {
        CLOSE(fd);
        return -1;
    }
#endif
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    WHY("");
#undef WHY
    return fd;
}
static OrbitLink *wrap_reason(int fd, const uint8_t key[32], const char *identity, char *reason,
                              size_t capacity) {
    OrbitLink *link = wrap_any(fd, key, identity, NULL);
    if (!link && reason && capacity)
        snprintf(reason, capacity, "%s",
                 "Gastgeber erreicht, aber die Einladung gilt nicht mehr (Gruppe gelöscht?) oder die "
                 "andere PS3 hat noch eine alte Orbit-Version.");
    return link;
}
static OrbitLink *connect_as(const char *ip, int port, const uint8_t key[32], const char *identity,
                             char *reason, size_t capacity) {
    int fd = orbit_tcp_connect(ip, port, reason, capacity);
    return fd < 0 ? NULL : wrap_reason(fd, key, identity, reason, capacity);
}
OrbitLink *orbit_wrap_group(int fd, const char *code, char *reason, size_t capacity) {
    char norm[33], identity[20] = "g7:";
    uint8_t psk[32];
    if (orbit_group_code(norm, code) || strlen(norm) != 8 || orbit_group_psk(psk, norm)) {
        CLOSE(fd);
        return NULL;
    }
    orbit_group_tag(identity + 3, norm);
    OrbitLink *l = wrap_reason(fd, psk, identity, reason, capacity);
    crypto_wipe(psk, 32);
    return l;
}
uint32_t orbit_resolve(const char *host) {
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1)
        return ntohl(a.s_addr);
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t ip = 0;
    pthread_mutex_lock(&lock);
    struct hostent *e = gethostbyname(host);
    if (e && e->h_addrtype == AF_INET && e->h_length == 4 && e->h_addr_list[0]) {
        memcpy(&a, e->h_addr_list[0], 4);
        ip = ntohl(a.s_addr);
    }
    pthread_mutex_unlock(&lock);
    return ip;
}
void orbit_socket_close(int fd) {
    if (fd >= 0)
        CLOSE(fd);
}
/* Binary frames for voice: 4-byte length header like JSON frames, payload up to 1 KiB. */
int orbit_frame_send(OrbitLink *l, const uint8_t *buf, size_t n) {
    if (!l || !n || n > 1024)
        return -1;
    l->deadline = orbit_millis() + 3000;
    unsigned char h[4] = {0, 0, (unsigned char)(n >> 8), (unsigned char)n};
    return transfer(l, h, 4, 1) || transfer(l, (unsigned char *)buf, n, 1) ? -1 : 0;
}
int orbit_frame_recv(OrbitLink *l, uint8_t *buf, size_t cap, unsigned wait_ms) {
    if (!l)
        return -1;
    if (!mbedtls_ssl_get_bytes_avail(&l->ssl) && !ready(l->fd, 0, wait_ms))
        return 0;
    l->deadline = orbit_millis() + 3000;
    unsigned char h[4];
    if (transfer(l, h, 4, 0))
        return -1;
    size_t n = (size_t)h[2] << 8 | h[3];
    if (h[0] || h[1] || !n || n > cap)
        return -1;
    return transfer(l, buf, n, 0) ? -1 : (int)n;
}
/* Voice relay: every packet from one call member is queued for all other members of the call.
 * Each connection is written only by its own thread (TLS contexts are not shared). */
static void voice_relay(Peer *p, OrbitLink *l) {
    OrbitServer *s = p->server;
    uint8_t buf[VOICE_PACKET + 8], out[VOICE_PACKET];
    uint64_t last_rx = orbit_millis();
    for (;;) {
        pthread_mutex_lock(&s->lock);
        int stop = s->stop;
        pthread_mutex_unlock(&s->lock);
        if (stop)
            return;
        for (;;) {
            int have = 0;
            pthread_mutex_lock(&p->qlock);
            if (p->qcount) {
                memcpy(out, p->queue[p->qhead], VOICE_PACKET);
                p->qhead = (p->qhead + 1) % VOICE_QUEUE;
                p->qcount--;
                have = 1;
            }
            pthread_mutex_unlock(&p->qlock);
            if (!have)
                break;
            if (orbit_frame_send(l, out, VOICE_PACKET))
                return;
        }
        int n = orbit_frame_recv(l, buf, sizeof(buf), 8);
        if (n < 0)
            return;
        if (!n) {
            if (orbit_millis() - last_rx > 10000)
                return; /* client vanished */
            continue;
        }
        last_rx = orbit_millis();
        if (n != VOICE_PACKET || buf[0] != VF_AUDIO)
            continue;
        buf[1] = (uint8_t)p->vuser;
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < PEERS; i++) {
            Peer *q = &s->peers[i];
            if (q == p || q->state != 1 || !q->voice || q->vgroup != p->vgroup)
                continue;
            pthread_mutex_lock(&q->qlock);
            if (q->qcount == VOICE_QUEUE) {
                q->qhead = (q->qhead + 1) % VOICE_QUEUE;
                q->qcount--;
            }
            memcpy(q->queue[(q->qhead + q->qcount++) % VOICE_QUEUE], buf, VOICE_PACKET);
            pthread_mutex_unlock(&q->qlock);
        }
        pthread_mutex_unlock(&s->lock);
    }
}
static int voice_enter(OrbitServer *s, Peer *p, const OrbitSession *session, const cJSON *req) {
    cJSON *g = cJSON_GetObjectItemCaseSensitive(req, "group");
    int gid = cJSON_IsNumber(g) ? g->valueint : -1, uid = session->user, ok = 0;
    pthread_mutex_lock(&s->store->mutex);
    OrbitData *d = &s->store->data;
    if (uid >= 0 && uid < d->users && gid >= 0 && gid < ORBIT_GROUPS && d->group[gid].used &&
        (d->group[gid].members & (UINT32_C(1) << uid))) {
        s->store->call[gid] |= UINT32_C(1) << uid;
        ok = 1;
    }
    pthread_mutex_unlock(&s->store->mutex);
    if (ok) {
        pthread_mutex_lock(&s->lock);
        pthread_mutex_lock(&p->qlock);
        p->qhead = p->qcount = 0;
        pthread_mutex_unlock(&p->qlock);
        p->voice = 1;
        p->vgroup = gid;
        p->vuser = uid;
        pthread_mutex_unlock(&s->lock);
    }
    return ok;
}
static void voice_leave(OrbitServer *s, Peer *p) {
    pthread_mutex_lock(&s->lock);
    int gid = p->vgroup, uid = p->vuser, others = 0;
    p->voice = 0;
    for (int i = 0; i < PEERS; i++)
        if (&s->peers[i] != p && s->peers[i].state == 1 && s->peers[i].voice &&
            s->peers[i].vgroup == gid && s->peers[i].vuser == uid)
            others = 1;
    pthread_mutex_unlock(&s->lock);
    if (!others) {
        pthread_mutex_lock(&s->store->mutex);
        s->store->call[gid] &= ~(UINT32_C(1) << uid);
        pthread_mutex_unlock(&s->store->mutex);
    }
}
static void *peer_main(void *arg) {
    Peer *p = arg;
    OrbitServer *s = p->server;
    int fd = p->fd;
    OrbitLink *l = wrap_any(fd, s->key, NULL, s);
    OrbitSession session = {-1, 0};
    if (l) {
        for (;;) {
            pthread_mutex_lock(&s->lock);
            int stop = s->stop;
            pthread_mutex_unlock(&s->lock);
            if (stop)
                break;
            l->deadline = orbit_millis() + 7000;
            cJSON *req = recv_json(l);
            if (!req)
                break;
            if (!strcmp(orbit_string(req, "op"), "voice")) {
                int ok = voice_enter(s, p, &session, req);
                cJSON_Delete(req);
                cJSON *reply = ok ? cJSON_CreateObject()
                                  : orbit_error("Kein Zugriff auf diesen Sprachchat.");
                if (ok)
                    cJSON_AddBoolToObject(reply, "ok", 1);
                int bad = send_json(l, reply);
                cJSON_Delete(reply);
                if (ok) {
                    if (!bad)
                        voice_relay(p, l);
                    voice_leave(s, p);
                    break;
                }
                if (bad)
                    break;
                continue;
            }
            cJSON *reply = orbit_request(s->store, &session,
                                         req); /* passwords remain only until request disposal */
            cJSON *pass = cJSON_GetObjectItemCaseSensitive(req, "password");
            if (cJSON_IsString(pass))
                crypto_wipe(pass->valuestring, strlen(pass->valuestring));
            cJSON *token = cJSON_GetObjectItem(req, "token");
            if (cJSON_IsString(token))
                crypto_wipe(token->valuestring, strlen(token->valuestring));
            cJSON_Delete(req);
            int bad = !reply || send_json(l, reply);
            token = cJSON_GetObjectItem(reply, "token");
            if (cJSON_IsString(token))
                crypto_wipe(token->valuestring, strlen(token->valuestring));
            cJSON_Delete(reply);
            if (bad)
                break;
        }
        orbit_disconnect(l);
    }
    pthread_mutex_lock(&s->lock);
    p->fd = -1;
    p->state = 2;
    pthread_mutex_unlock(&s->lock);
    return NULL;
}
static void *accept_main(void *arg) {
    OrbitServer *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->lock);
        int stop = s->stop;
        pthread_mutex_unlock(&s->lock);
        if (stop)
            break;
        for (int i = 0; i < PEERS; i++) {
            pthread_mutex_lock(&s->lock);
            int done = s->peers[i].state == 2;
            pthread_mutex_unlock(&s->lock);
            if (done) {
                pthread_join(s->peers[i].thread, NULL);
                pthread_mutex_lock(&s->lock);
                s->peers[i].state = 0;
                pthread_mutex_unlock(&s->lock);
            }
        }
        if (!ready(s->fd, 0, 250))
            continue;
        int fd = accept(s->fd, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&s->lock);
        int slot = -1;
        for (int i = 0; i < PEERS; i++)
            if (s->peers[i].state == 0) {
                slot = i;
                break;
            }
        if (slot >= 0) {
            Peer *p = &s->peers[slot];
            p->server = s;
            p->fd = fd;
            p->state = 1;
            if (pthread_create(&p->thread, NULL, peer_main, p)) {
                p->state = 0;
                CLOSE(fd);
            }
        } else
            CLOSE(fd);
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}
/* LAN discovery: "ORBIT7?<tag>" broadcast, answered with "ORBIT7!<tag>" by the host that has it. */
static void *discovery_main(void *arg) {
    OrbitServer *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->lock);
        int stop = s->stop;
        pthread_mutex_unlock(&s->lock);
        if (stop)
            break;
        if (!ready(s->udp, 0, 250))
            continue;
        char buf[64], code[9];
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int n = (int)recvfrom(s->udp, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &len);
        if (n != 23 || memcmp(buf, "ORBIT7?", 7))
            continue;
        buf[n] = 0;
        if (orbit_store_code_for_tag(s->store, buf + 7, code))
            continue;
        crypto_wipe(code, sizeof(code));
        buf[6] = '!';
        sendto(s->udp, buf, 23, 0, (struct sockaddr *)&from, len);
    }
    return NULL;
}
static int udp_socket(int port, int broadcast) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (broadcast)
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
#ifdef __PPU__
    addr.sin_len = sizeof(addr);
#endif
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        CLOSE(fd);
        return -1;
    }
    return fd;
}
int orbit_discover(const char tag[17], int port, const char *broadcast, char ip[16],
                   unsigned timeout_ms) {
    int fd = udp_socket(0, 1);
    if (fd < 0)
        return -1;
    char query[24];
    snprintf(query, sizeof(query), "ORBIT7?%s", tag);
    const char *targets[] = {"255.255.255.255", broadcast, "127.0.0.1"};
    int found = -1;
    uint64_t end = orbit_millis() + timeout_ms, next = 0;
    while (found && orbit_millis() < end) {
        if (orbit_millis() >= next) {
            for (int i = 0; i < 3; i++) {
                struct sockaddr_in to;
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_port = htons((uint16_t)port);
#ifdef __PPU__
                to.sin_len = sizeof(to);
#endif
                if (targets[i] && targets[i][0] &&
                    inet_pton(AF_INET, targets[i], &to.sin_addr) == 1)
                    sendto(fd, query, 23, 0, (struct sockaddr *)&to, sizeof(to));
            }
            next = orbit_millis() + 300;
        }
        if (!ready(fd, 0, 100))
            continue;
        char buf[64];
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int n = (int)recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len);
        if (n == 23 && !memcmp(buf, "ORBIT7!", 7) && !memcmp(buf + 7, tag, 16)) {
            uint32_t a = ntohl(from.sin_addr.s_addr);
            snprintf(ip, 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
            found = 0;
        }
    }
    CLOSE(fd);
    return found;
}
OrbitServer *orbit_server_start(OrbitStore *store, const uint8_t key[32], int port) {
    OrbitServer *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->store = store;
    memcpy(s->key, key, 32);
    pthread_mutex_init(&s->lock, NULL);
    for (int i = 0; i < PEERS; i++)
        pthread_mutex_init(&s->peers[i].qlock, NULL);
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0)
        goto fail;
    int yes = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
#ifdef __PPU__
    addr.sin_len = sizeof(addr);
#endif
    if (bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(s->fd, PEERS) ||
        pthread_create(&s->acceptor, NULL, accept_main, s))
        goto fail;
    /* Discovery is optional: without it the invitation address is still used. */
    s->udp = udp_socket(port, 1);
    if (s->udp >= 0 && !pthread_create(&s->discovery, NULL, discovery_main, s))
        s->udp_running = 1;
    else if (s->udp >= 0) {
        CLOSE(s->udp);
        s->udp = -1;
    }
    return s;
fail:
    if (s->fd >= 0)
        CLOSE(s->fd);
    pthread_mutex_destroy(&s->lock);
    crypto_wipe(s, sizeof(*s));
    free(s);
    return NULL;
}
void orbit_server_stop(OrbitServer *s) {
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->stop = 1;
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->acceptor, NULL);
    CLOSE(s->fd);
    if (s->udp_running) {
        pthread_join(s->discovery, NULL);
        CLOSE(s->udp);
    }
    for (int i = 0; i < PEERS; i++) {
        pthread_mutex_lock(&s->lock);
        int live = s->peers[i].state !=
                   0; /* Closing here would race descriptor reuse; timeout wakes each worker. */
        pthread_mutex_unlock(&s->lock);
        if (live)
            pthread_join(s->peers[i].thread, NULL);
    }
    pthread_mutex_destroy(&s->lock);
    for (int i = 0; i < PEERS; i++)
        pthread_mutex_destroy(&s->peers[i].qlock);
    crypto_wipe(s, sizeof(*s));
    free(s);
}

#ifndef ORBIT_H
#define ORBIT_H
#include "cJSON.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#define ORBIT_VERSION "0.9.5"
#define ORBIT_PROTOCOL 9
#define ORBIT_USERS 32
#define ORBIT_GROUPS 24
#define ORBIT_MESSAGES 256
#define ORBIT_TEXT 769
#define ORBIT_PORT 31413
#define ORBIT_FRAME 65536
#define ORBIT_HOST 96
#define ORBIT_PENDING_EMAILS 64
/* Invite: XXXXX-XXXXX-XXXXX-XXXXX (IPv4, port, group code, checksum). */
#define ORBIT_INVITE_TEXT 24
#define ORBIT_INVITE_RELAY 0x10000
int orbit_parse_endpoint(const char *text, char host[ORBIT_HOST], int *port);
int orbit_parse_invite(const char *text, char host[ORBIT_HOST], int *port, uint8_t key[32]);
int orbit_group_code(char out[33], const char *text);
int orbit_parse_join(const char *text, char host[ORBIT_HOST], int *port, uint8_t key[32],
                     char group[33]);
int orbit_invite_encode(char out[ORBIT_INVITE_TEXT], uint32_t ipv4, int port, const char *code);
int orbit_invite_decode(const char *text, uint32_t *ipv4, int *port, char code[9]);
void orbit_group_tag(char out[17], const char *code);
int orbit_group_psk(uint8_t psk[32], const char *code);
int orbit_ipv4_private(uint32_t ipv4);
typedef struct {
    char email[97], name[49];
    uint8_t salt[16], hash[32];
    uint8_t devices[4][32];
} OrbitUser;
typedef struct {
    int used, group;
    char email[97];
} OrbitPendingEmail;
typedef struct {
    int used, owner;
    uint32_t members, pending;
    char name[65], invite[33];
} OrbitGroup;
typedef struct {
    uint32_t id, time;
    int group, user;
    char text[ORBIT_TEXT];
} OrbitMessage;
typedef struct {
    int version, users;
    uint32_t next_id;
    OrbitUser user[ORBIT_USERS];
    OrbitGroup group[ORBIT_GROUPS];
    OrbitMessage message[ORBIT_MESSAGES];
    OrbitPendingEmail pending_email[ORBIT_PENDING_EMAILS];
} OrbitData;
typedef struct {
    OrbitData data;
    char path[512], server_id[17];
    pthread_mutex_t mutex;
    uint64_t auth_window;
    int auth_count;
    uint64_t join_window[ORBIT_USERS];
    unsigned join_count[ORBIT_USERS];
    uint32_t call[ORBIT_GROUPS]; /* members currently in the voice chat, not saved */
} OrbitStore;
typedef struct {
    int user;
    uint64_t last_write;
} OrbitSession;
int orbit_random(void *buf, size_t n);
uint64_t orbit_millis(void);
uint32_t orbit_unix_time(void);
void orbit_hex(char *out, const uint8_t *in, size_t n);
int orbit_unhex(uint8_t *out, const char *s, size_t n);
int orbit_text_valid(const char *s, size_t min, size_t max);
int orbit_store_open(OrbitStore *s, const char *path);
void orbit_store_close(OrbitStore *s);
/* Returns the group code for a PSK tag, or -1. Thread-safe. */
int orbit_store_code_for_tag(OrbitStore *s, const char tag[17], char code[9]);
cJSON *orbit_request(OrbitStore *s, OrbitSession *session, const cJSON *req);
const char *orbit_string(const cJSON *o, const char *key);
cJSON *orbit_error(const char *text);
int orbit_keyfile(const char *path, uint8_t key[32]);
typedef struct OrbitServer OrbitServer;
typedef struct OrbitLink OrbitLink;
OrbitServer *orbit_server_start(OrbitStore *store, const uint8_t key[32], int port);
void orbit_server_stop(OrbitServer *server);
OrbitLink *orbit_connect(const char *ip, int port, const uint8_t key[32]);
OrbitLink *orbit_connect_detailed(const char *ip, int port, const uint8_t key[32], char *reason,
                                  size_t capacity);
/* Connects with a group invitation: PSK derived from the group code. */
OrbitLink *orbit_connect_group(const char *ip, int port, const char *code, char *reason,
                               size_t capacity);
/* LAN discovery: broadcasts the group tag, returns the answering host IPv4. */
int orbit_discover(const char tag[17], int port, const char *broadcast, char ip[16],
                   unsigned timeout_ms);
cJSON *orbit_exchange(OrbitLink *link, const cJSON *req);
/* Plain TCP with timeouts (DNS names allowed). Returns a blocking socket or -1. */
int orbit_tcp_connect(const char *host, int port, char *reason, size_t capacity);
void orbit_socket_close(int fd);
/* IPv4 of a dotted address or DNS name, 0 if unknown. */
uint32_t orbit_resolve(const char *host);
/* TLS with the group-code PSK on an already connected socket (e.g. through the relay). */
OrbitLink *orbit_wrap_group(int fd, const char *code, char *reason, size_t capacity);
/* Relay ("Vermittler"): a small server both consoles reach from inside their home network.
 * The relay only splices TCP streams; TLS stays end to end between the consoles. */
#define ORBIT_RELAY_PORT 31400
typedef struct OrbitRelayHost OrbitRelayHost;
OrbitRelayHost *orbit_relay_host_start(const char *relay, const char server_id[17],
                                       const uint8_t key[32], OrbitStore *store, int local_port);
/* 1 = registered at the relay, 0 = not (reason filled) */
int orbit_relay_host_state(OrbitRelayHost *h, char *reason, size_t capacity);
void orbit_relay_host_stop(OrbitRelayHost *h);
int orbit_relay_dial(const char *relay, const char tag[17], char *reason, size_t capacity);
int orbit_relay_discover(int port, char ip[16], unsigned timeout_ms);
/* Built-in tunnel (open bore protocol, e.g. bore.pub): makes this console's meeting point
 * reachable from the internet without router settings and without a PC. */
typedef struct OrbitBore OrbitBore;
OrbitBore *orbit_bore_start(const char *server, int want_port, int local_port);
int orbit_bore_state(OrbitBore *b, uint32_t *ip, int *port, char *host, size_t host_cap,
                     char *reason, size_t cap);
void orbit_bore_stop(OrbitBore *b);
int orbit_frame_send(OrbitLink *link, const uint8_t *buf, size_t n);
/* Returns frame length, 0 when nothing arrived within wait_ms, -1 on a broken link. */
int orbit_frame_recv(OrbitLink *link, uint8_t *buf, size_t cap, unsigned wait_ms);
void orbit_disconnect(OrbitLink *link);
/* Automatic router port opening: UPnP IGD, then PCP / NAT-PMP. No manual router setup. */
typedef struct {
    int ok, port, cgnat, fritz;
    char external[16], method[16], reason[300], router[64];
    char control[256], service[96]; /* UPnP mapping, kept for removal */
} OrbitNat;
int orbit_nat_open(const char *local_ip, const char *gateway, int internal_port, OrbitNat *out);
void orbit_nat_close(OrbitNat *nat, const char *gateway);
#endif

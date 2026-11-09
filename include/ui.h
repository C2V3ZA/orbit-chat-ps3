#ifndef ORBIT_UI_H
#define ORBIT_UI_H
#include "orbit.h"
#define UI_GROUPS 48
#define UI_MESSAGES 80
#define APP_LINKS 8
#define CALL_PEOPLE 12
enum { NET_CHECKING, NET_INTERNET, NET_HOME, NET_OFFLINE };
enum { LINK_OFF, LINK_CONNECTING, LINK_ONLINE, LINK_FAILED };
enum { CALL_OFF, CALL_CONNECTING, CALL_ONLINE, CALL_RECONNECTING };
typedef struct {
    int link, id, members, unread, state, own, call;
    char name[65], host[49], people[200];
} UiGroup;
typedef struct {
    int id, mine;
    uint32_t time;
    char name[49], text[ORBIT_TEXT];
} UiMessage;
typedef struct {
    int speaking, self;
    char name[49];
} UiCallPerson;
typedef struct {
    int profile, busy, group_count, selected, message_count, sent_count, net, invite_state,
        loading, revision;
    uint32_t status_id, speak_mask;
    char status[200], username[49], invite_code[ORBIT_INVITE_TEXT], invite_group[65],
        invite_scope[300], net_detail[300], local_ip[16], public_ip[16], server_id[17];
    char link_error[200], relay[104], tunnel[128];
    int relay_online, tunnel_online;
    /* voice chat */
    int call_state, call_muted, call_count, call_here, call_mic;
    char call_name[65], call_note[200];
    UiCallPerson call_people[CALL_PEOPLE];
    UiGroup groups[UI_GROUPS];
    UiMessage messages[UI_MESSAGES];
} UiState;
struct OrbitApp;
typedef struct {
    struct OrbitApp *owner;
    int used, own, state, running, user, history_group, message_count, group_count, port;
    int reauth, remove, code_group, cached, thread_joinable;
    char join_code[9];
    uint64_t retry_at, backoff;
    char host[16], code[9], server[17], error[200], relay[24];
    pthread_t thread;
    cJSON *job;
    struct {
        int id, members, call;
        uint32_t last;
        char name[65], host[49], people[200];
    } groups[ORBIT_GROUPS];
    UiMessage messages[UI_MESSAGES];
} AppLink;
typedef int (*AppNetInfo)(char ip[16], char mask[16], char gateway[16]);
typedef struct OrbitApp {
    pthread_mutex_t lock;
    pthread_t manager;
    int stop, dirty, changed, busy, sent_count, profile, net, invite_state, port;
    int selected_link, selected_group, invite_wait, invite_wait_link, invite_wait_group, nat_now;
    uint64_t busy_since;
    uint32_t status_id;
    char data_dir[384], ip[16], netmask[16], gateway[16], broadcast[16], name[49];
    char status[200], server_id[17], invite_code[ORBIT_INVITE_TEXT], invite_group[65],
        invite_scope[300], net_detail[300];
    uint8_t device[32], server_key[32];
    uint32_t seen[APP_LINKS][ORBIT_GROUPS];
    OrbitStore *store;
    OrbitServer *server;
    OrbitNat nat;
    AppNetInfo netinfo;
    /* relay ("Vermittler") */
    char relay[104], relay_reason[200];
    int relay_user, relay_online, relay_port;
    uint32_t relay_ip;
    OrbitRelayHost *relay_host;
    /* built-in internet tunnel */
    OrbitBore *bore;
    int bore_online, bore_port, invite_dispatch;
    uint32_t bore_ip;
    uint64_t invite_wait_since;
    char bore_host[ORBIT_HOST], bore_reason[200];
    /* voice chat: one call at a time */
    pthread_t call_thread;
    int call_running, call_joinable, call_leave, call_state, call_link, call_group, call_count;
    char call_name[65], call_note[200];
    struct {
        int user;
        char name[49];
    } call_people[CALL_PEOPLE];
    AppLink links[APP_LINKS];
} OrbitApp;
#define KB_TEXT 769
typedef struct {
    int page, focus, history_offset, menu_focus;
    uint64_t now;
    char draft[ORBIT_TEXT];
    /* in-app keyboard (replaces the system dialog: no dimmed/black background) */
    int kb_active, kb_row, kb_col, kb_shift, kb_mode, kb_max;
    char kb_title[96], kb_text[KB_TEXT];
} UiLocal;
/* 0.6 remembered sign-in, read once for migration. */
typedef struct {
    int valid, hosting;
    char invite[240], email[97], token[65];
} OrbitRemember;
int orbit_remember_read(const char *dir, OrbitRemember *record);
int orbit_remember_write(const char *dir, const OrbitRemember *record);
void orbit_remember_clear(const char *dir, OrbitRemember *record);
enum { PAGE_CHAT, PAGE_MENU, PAGE_INVITE, PAGE_LEAVE, PAGE_NETWORK, PAGE_WELCOME, PAGE_HELP };
int app_start(OrbitApp *app, const char *data_dir, const char *ip, const char *netmask,
              const char *gateway, const char *suggested_name, int port);
void app_set_netinfo(OrbitApp *app, AppNetInfo fn);
void app_stop(OrbitApp *app);
void app_snapshot(OrbitApp *app, UiState *state);
void app_select(OrbitApp *app, int direction);
void app_notice(OrbitApp *app, const char *text);
int app_set_name(OrbitApp *app, const char *name);
int app_create_group(OrbitApp *app, const char *name);
int app_join(OrbitApp *app, const char *invite);
int app_send(OrbitApp *app, const char *text);
int app_invite(OrbitApp *app);
int app_leave(OrbitApp *app);
void app_retry(OrbitApp *app);
/* user = 1: chosen in the app (saved); 0: default from the package (relay.txt) */
void app_set_relay(OrbitApp *app, const char *relay, int user);
/* Internet tunnel server (e.g. "bore.pub"); empty = off */
void app_set_tunnel(OrbitApp *app, const char *server);
/* voice chat */
int app_call_toggle(OrbitApp *app);
void app_call_mute(OrbitApp *app);
int ui_init(const char *font_path);
void ui_close(void);
void ui_set_timezone(int minutes);
void ui_render(uint32_t *pixels, int width, int height, const UiState *state, const UiLocal *local);
void ui_performance(int fps, int frame_us, int render_us);
void ui_icon(uint32_t *pixels, int width, int height);
/* Animated intro; t in seconds. Returns 1 when finished. */
int ui_intro(uint32_t *pixels, int width, int height, float t, const char *version);
void ui_intro_done(void);
void ui_social(uint32_t *pixels, int width, int height, const char *version);
int ui_toast_visible(const UiState *s, uint64_t now);
/* keyboard layout: returns the UTF-8 text of a key, or NULL for special keys */
const char *ui_kb_key(int row, int col, int shift);
int ui_kb_cols(int row);
#define KB_ROWS 5
enum { KB_SHIFT = 100, KB_SPACE, KB_BACK, KB_DONE };
int ui_kb_special(int row, int col);
void ui_audio_probe(uint32_t *pixels, int width, int height, const char *status,
                    const char rows[][100], int count, int player, int remaining, int level);
#endif

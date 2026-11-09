#include "monocypher.h"
#include "rsxutil.h"
#include "ui.h"
#include <io/pad.h>
#include <lv2/systime.h>
#include <malloc.h>
#include <net/net.h>
#include <net/netctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sysutil/sysutil.h>
#include <unistd.h>
#define APPDIR "/dev_hdd0/game/ORB000001/USRDIR"
enum { EDIT_NONE, EDIT_NAME, EDIT_MESSAGE, EDIT_GROUP, EDIT_INVITE, EDIT_RELAY };
static int running = 1, kb_edit, system_menu;
static OrbitApp application;
static UiState state;
static UiLocal local;
static void notice(const char *msg) {
    app_notice(&application, msg);
}
static void callback(u64 status, u64 param, void *user) {
    (void)param;
    (void)user;
    if (status == SYSUTIL_EXIT_GAME)
        running = 0;
    /* PS button: the XMB opens on top of Orbit. Chat and voice keep running underneath. */
    else if (status == SYSUTIL_MENU_OPEN)
        system_menu = 1;
    else if (status == SYSUTIL_MENU_CLOSE)
        system_menu = 0;
}
/* Network may come up after Orbit started (or change): the app asks again periodically. */
static int netinfo(char ip[16], char mask[16], char gw[16]) {
    union net_ctl_info info;
    memset(&info, 0, sizeof(info));
    if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info))
        return -1;
    snprintf(ip, 16, "%s", info.ip_address);
    memset(&info, 0, sizeof(info));
    if (!netCtlGetInfo(NET_CTL_INFO_NETMASK, &info))
        snprintf(mask, 16, "%s", info.netmask);
    memset(&info, 0, sizeof(info));
    if (!netCtlGetInfo(NET_CTL_INFO_DEFAULT_ROUTE, &info))
        snprintf(gw, 16, "%s", info.default_route);
    return 0;
}
/* ---------- in-app keyboard ---------- */
static void keyboard(int edit, const char *title, const char *initial, int max, int mode) {
    local.kb_active = 1;
    local.kb_row = 1;
    local.kb_col = 0;
    local.kb_mode = mode;
    local.kb_shift = mode == 1 || (edit == EDIT_NAME && !initial[0]);
    local.kb_max = max < KB_TEXT - 1 ? max : KB_TEXT - 1;
    kb_edit = edit;
    snprintf(local.kb_title, sizeof(local.kb_title), "%s", title);
    snprintf(local.kb_text, sizeof(local.kb_text), "%s", initial);
}
static void kb_append(const char *t) {
    size_t n = strlen(local.kb_text), k = strlen(t);
    if (n + k > (size_t)local.kb_max)
        return;
    memcpy(local.kb_text + n, t, k + 1);
    /* Invitation codes: insert the dashes automatically after every five characters. */
    if (local.kb_mode == 1) {
        int symbols = 0;
        for (const char *p = local.kb_text; *p; p++)
            if (*p != '-')
                symbols++;
        n = strlen(local.kb_text);
        if (symbols % 5 == 0 && symbols < 20 && n < (size_t)local.kb_max)
            strcat(local.kb_text, "-");
    } else if (local.kb_shift == 1 && kb_edit != EDIT_INVITE)
        local.kb_shift = 0; /* one capital letter, then lower case again */
}
static void kb_backspace(void) {
    size_t n = strlen(local.kb_text);
    while (n) {
        unsigned char c = (unsigned char)local.kb_text[--n];
        local.kb_text[n] = 0;
        if ((c & 0xc0) != 0x80)
            break;
    }
    if (local.kb_mode == 1 && n && local.kb_text[n - 1] == '-')
        local.kb_text[--n] = 0;
}
static void kb_finish(int accept) {
    char value[KB_TEXT];
    snprintf(value, sizeof(value), "%s", local.kb_text);
    local.kb_active = 0;
    int edit = kb_edit;
    kb_edit = EDIT_NONE;
    char *v = value;
    while (*v == ' ')
        v++;
    size_t n = strlen(v);
    while (n && v[n - 1] == ' ')
        v[--n] = 0;
    if (edit == EDIT_MESSAGE) {
        snprintf(local.draft, sizeof(local.draft), "%s", value); /* never lose a draft */
        if (accept && n && app_send(&application, v))
            notice("Nicht gesendet. Dein Text bleibt erhalten.");
        return;
    }
    if (edit == EDIT_RELAY && accept && !n) {
        app_set_relay(&application, "", 1); /* empty: no relay */
        return;
    }
    if (!accept || !n)
        return;
    if (edit == EDIT_NAME && app_set_name(&application, v))
        notice("Name: 1 bis 48 Zeichen.");
    if (edit == EDIT_GROUP)
        app_create_group(&application, v);
    if (edit == EDIT_INVITE && !app_join(&application, v))
        local.page = PAGE_CHAT;
    if (edit == EDIT_RELAY)
        app_set_relay(&application, v, 1);
}
enum {
    B_UP = 1,
    B_DOWN = 2,
    B_LEFT = 4,
    B_RIGHT = 8,
    B_CROSS = 16,
    B_CIRCLE = 32,
    B_SQUARE = 64,
    B_TRIANGLE = 128,
    B_SELECT = 256,
    B_START = 512,
    B_L1 = 1024,
    B_R1 = 2048,
    B_R3 = 4096,
    B_R2 = 8192,
    B_L2 = 16384
};
static void kb_input(unsigned b) {
    if (b & B_UP) {
        if (local.kb_row == 4)
            local.kb_col = (const int[]){0, 4, 7, 9}[local.kb_col];
        local.kb_row = (local.kb_row + KB_ROWS - 1) % KB_ROWS;
        if (local.kb_row == 4)
            local.kb_col = local.kb_col < 2 ? 0 : local.kb_col < 7 ? 1 : local.kb_col < 9 ? 2 : 3;
    }
    if (b & B_DOWN) {
        if (local.kb_row == 4)
            local.kb_col = (const int[]){0, 4, 7, 9}[local.kb_col];
        local.kb_row = (local.kb_row + 1) % KB_ROWS;
        if (local.kb_row == 4)
            local.kb_col = local.kb_col < 2 ? 0 : local.kb_col < 7 ? 1 : local.kb_col < 9 ? 2 : 3;
    }
    int cols = ui_kb_cols(local.kb_row);
    if (b & B_LEFT)
        local.kb_col = (local.kb_col + cols - 1) % cols;
    if (b & B_RIGHT)
        local.kb_col = (local.kb_col + 1) % cols;
    if (b & B_SQUARE)
        kb_backspace();
    if (b & B_TRIANGLE)
        kb_append(" ");
    if (b & (B_L1 | B_L2))
        local.kb_shift = !local.kb_shift;
    if (b & B_CROSS) {
        int special = ui_kb_special(local.kb_row, local.kb_col);
        if (special == KB_SHIFT)
            local.kb_shift = !local.kb_shift;
        else if (special == KB_SPACE)
            kb_append(" ");
        else if (special == KB_BACK)
            kb_backspace();
        else if (special == KB_DONE) {
            kb_finish(1);
            return;
        } else {
            const char *k = ui_kb_key(local.kb_row, local.kb_col, local.kb_shift);
            if (k)
                kb_append(k);
        }
    }
    if (b & B_START) {
        kb_finish(1);
        return;
    }
    if (b & B_CIRCLE)
        kb_finish(0);
}
static unsigned buttons(void) {
    padInfo info;
    padData pad;
    memset(&info, 0, sizeof(info));
    if (ioPadGetInfo(&info))
        return 0;
    static unsigned held[7];
    unsigned all = 0;
    /* Every connected controller can steer: no "wrong port" after reconnecting a pad. */
    for (int i = 0; i < 7; i++) {
        if (!info.status[i]) {
            held[i] = 0;
            continue;
        }
        memset(&pad, 0, sizeof(pad));
        if (!ioPadGetData(i, &pad) && pad.len > 0)
            held[i] = (pad.BTN_UP ? B_UP : 0) | (pad.BTN_DOWN ? B_DOWN : 0) |
                      (pad.BTN_LEFT ? B_LEFT : 0) | (pad.BTN_RIGHT ? B_RIGHT : 0) |
                      (pad.BTN_CROSS ? B_CROSS : 0) | (pad.BTN_CIRCLE ? B_CIRCLE : 0) |
                      (pad.BTN_SQUARE ? B_SQUARE : 0) | (pad.BTN_TRIANGLE ? B_TRIANGLE : 0) |
                      (pad.BTN_SELECT ? B_SELECT : 0) | (pad.BTN_START ? B_START : 0) |
                      (pad.BTN_L1 ? B_L1 : 0) | (pad.BTN_R1 ? B_R1 : 0) |
                      (pad.BTN_R3 ? B_R3 : 0) | (pad.BTN_R2 ? B_R2 : 0) | (pad.BTN_L2 ? B_L2 : 0);
        all |= held[i];
    }
    return all;
}
static int has_group(void) {
    return state.group_count > 0 && state.selected >= 0;
}
static void open_invite(void) {
    if (!has_group()) {
        notice("Erstelle zuerst eine Gruppe.");
        return;
    }
    app_invite(&application);
    local.page = PAGE_INVITE;
}
static void open_code(void) {
    keyboard(EDIT_INVITE, "Einladungscode deines Freundes (20 Zeichen)", "", 23, 1);
}
static void action(unsigned b) {
    if (local.kb_active) {
        kb_input(b);
        return;
    }
    /* available everywhere: voice chat */
    if (b & B_R2)
        app_call_mute(&application);
    switch (local.page) {
    case PAGE_WELCOME:
        if (b & (B_UP | B_DOWN))
            local.focus = !local.focus;
        if (b & B_START)
            running = 0;
        if (b & B_CROSS) {
            if (local.focus == 0)
                keyboard(EDIT_NAME, "Dein Name (sehen deine Freunde)", state.username, 48, 0);
            else {
                app_set_name(&application, state.username);
                local.page = PAGE_CHAT;
            }
        }
        return;
    case PAGE_MENU:
        if (b & B_UP)
            local.menu_focus = (local.menu_focus + 7) % 8;
        if (b & B_DOWN)
            local.menu_focus = (local.menu_focus + 1) % 8;
        if (b & B_CIRCLE)
            local.page = PAGE_CHAT;
        if (b & B_CROSS) {
            switch (local.menu_focus) {
            case 0:
                local.page = PAGE_CHAT;
                keyboard(EDIT_GROUP, "Name der neuen Gruppe", "", 64, 0);
                break;
            case 1:
                local.page = PAGE_CHAT;
                open_code();
                break;
            case 2:
                open_invite();
                break;
            case 3:
                local.page = PAGE_CHAT;
                app_call_toggle(&application);
                break;
            case 4:
                keyboard(EDIT_NAME, "Dein Name", state.username, 48, 0);
                break;
            case 5:
                local.page = PAGE_NETWORK;
                break;
            case 6:
                if (has_group())
                    local.page = PAGE_LEAVE;
                break;
            case 7:
                running = 0;
                break;
            }
        }
        return;
    case PAGE_INVITE:
        if (b & (B_CIRCLE | B_TRIANGLE | B_CROSS))
            local.page = PAGE_CHAT;
        return;
    case PAGE_LEAVE:
        if (b & B_CIRCLE)
            local.page = PAGE_MENU;
        if (b & B_CROSS) {
            app_leave(&application);
            local.page = PAGE_CHAT;
            local.history_offset = 0;
        }
        return;
    case PAGE_NETWORK:
        if (b & B_CIRCLE)
            local.page = PAGE_MENU;
        if (b & B_SQUARE) {
            app_retry(&application);
            notice("Verbindungen werden neu aufgebaut ...");
        }
        if (b & B_TRIANGLE)
            keyboard(EDIT_RELAY, "Adresse des Orbit-Vermittlers (z. B. 203.0.113.5)", state.relay,
                     100, 0);
        return;
    default:
        break;
    }
    /* chat */
    if (b & B_UP) {
        app_select(&application, -1);
        local.history_offset = 0;
    }
    if (b & B_DOWN) {
        app_select(&application, 1);
        local.history_offset = 0;
    }
    if (b & B_L1 && local.history_offset < state.message_count - 1)
        local.history_offset++;
    if (b & B_R1 && local.history_offset > 0)
        local.history_offset--;
    if (b & B_CROSS) {
        if (!has_group())
            keyboard(EDIT_GROUP, "Name der neuen Gruppe", "", 64, 0);
        else
            keyboard(EDIT_MESSAGE, state.groups[state.selected].name, local.draft, 768, 2);
    }
    if (b & B_SELECT)
        app_call_toggle(&application);
    if (b & B_TRIANGLE)
        open_invite();
    if (b & B_SQUARE)
        open_code();
    if (b & B_CIRCLE) {
        local.page = PAGE_MENU;
        local.menu_focus = has_group() ? 2 : 0;
    }
    if (b & B_START)
        running = 0;
}
static uint64_t micros(void) {
    return (uint64_t)sysGetSystemTime();
}
int main(void) {
    mkdir(APPDIR "/data", 0700);
    netInitialize();
    int netctl = netCtlInit();
    char ip[16] = "127.0.0.1", mask[16] = "", gateway[16] = "", user[64] = "";
    netinfo(ip, mask, gateway);
    if (sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME, user, sizeof(user)))
        user[0] = 0;
    user[48] = 0;
    s32 tz = 0, summer = 0;
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &tz);
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &summer);
    ui_set_timezone(tz + (summer ? 60 : 0));
    if (app_start(&application, APPDIR "/data", ip, mask, gateway, user, ORBIT_PORT))
        return 1;
    app_set_netinfo(&application, netinfo);
    /* Internet without router settings and without a PC: built-in tunnel (open bore protocol). */
    app_set_tunnel(&application, "bore.pub");
    /* Optional own relay (USRDIR/relay.txt). The old PC relay default is replaced by the tunnel. */
    FILE *rf = fopen(APPDIR "/relay.txt", "r");
    if (rf) {
        char relay[104] = "";
        if (fgets(relay, sizeof(relay), rf)) {
            relay[strcspn(relay, " \r\n")] = 0;
            if (strncmp(relay, "bore.pub", 8))
                app_set_relay(&application, relay, 0);
        }
        fclose(rf);
    }
    if (ui_init(APPDIR "/OrbitSans.ttf")) {
        app_stop(&application);
        return 2;
    }
    void *host = memalign(1024 * 1024, HOST_SIZE);
    if (!host) {
        ui_close();
        app_stop(&application);
        return 3;
    }
    gcmContextData *context = initScreen(host, HOST_SIZE);
    if (!context) {
        ui_close();
        app_stop(&application);
        return 4;
    }
    u16 width, height;
    rsxBuffer buffers[2];
    memset(buffers, 0, sizeof(buffers));
    uint32_t *canvas = NULL;
    int ok = getResolution(&width, &height);
    for (int i = 0; ok && i < 2; i++) {
        ok = makeBuffer(&buffers[i], width, height, i);
        if (ok)
            memset(buffers[i].ptr, 0, (size_t)width * height * 4);
    }
    if (ok)
        canvas = memalign(128, (size_t)width * height * 4);
    if (!canvas) {
        app_stop(&application);
        rsxFinish(context, 1);
        ui_close();
        return 5;
    }
    ioPadInit(7);
    sysUtilRegisterCallback(0, callback, NULL);
    app_snapshot(&application, &state);
    local.page = state.profile ? PAGE_CHAT : PAGE_WELCOME;
    int index = 0, rendered = -1, generation = 0, sent_count = state.sent_count,
        buffer_generation[2] = {-1, -1}, toast_on = 0, selected_group = -1;
    unsigned previous = 0, repeat_direction = 0;
    uint64_t repeat_at = 0, stats_at = micros(), last_flip = micros();
    int show_stats = 0, frames = 0, fps = 0, render_us = 0, frame_us = 0, worst_render = 0;
    uint64_t frame_sum = 0;
    flip(context, 1);
    /* Animated intro while the network starts in the background. Any button skips it. */
    {
        uint64_t intro_start = micros();
        unsigned before = buttons();
        int skipped = 0;
        while (running) {
            sysUtilCheckCallback();
            unsigned b = system_menu ? 0 : buttons();
            if ((b & ~before) && !skipped) {
                skipped = 1; /* jump to the final fade instead of cutting hard */
                uint64_t fade = (uint64_t)((3.2f - 0.45f) * 1e6f);
                if (micros() - intro_start < fade)
                    intro_start = micros() - fade;
            }
            before = b;
            float t = (float)(micros() - intro_start) / 1e6f;
            int done = ui_intro(canvas, width, height, t, ORBIT_VERSION);
            waitFlip();
            memcpy(buffers[index].ptr, canvas, (size_t)width * height * 4);
            if (!flip(context, index))
                break;
            index ^= 1;
            if (done)
                break;
        }
        ui_intro_done();
        buffer_generation[0] = buffer_generation[1] = -1;
        previous = buttons(); /* the skip press must not trigger an action in the app */
    }
    while (running) {
        sysUtilCheckCallback();
        app_snapshot(&application, &state);
        unsigned held = system_menu ? 0 : buttons(), pressed = held & ~previous;
        previous = held;
        unsigned direction = held & (B_UP | B_DOWN | B_LEFT | B_RIGHT | B_L1 | B_R1 | B_SQUARE);
        if (!local.kb_active)
            direction &= ~B_SQUARE; /* hold SQUARE to delete quickly on the keyboard */
        uint64_t now = micros();
        local.now = orbit_millis();
        if (direction != repeat_direction) {
            repeat_direction = direction;
            repeat_at = now + 380000;
        } else if (direction && now >= repeat_at) {
            pressed |= direction;
            repeat_at = now + (local.kb_active ? 90000 : 110000);
        }
        int changed = 0;
        if (now - stats_at >= 1000000) {
            fps = (int)(((uint64_t)frames * 1000000 + (now - stats_at) / 2) / (now - stats_at));
            frame_us = frames ? (int)(frame_sum / (uint64_t)frames) : 0;
            frames = 0;
            frame_sum = 0;
            stats_at = now;
            if (show_stats)
                changed = 1;
        }
        if ((pressed & B_R3) && !local.kb_active) {
            show_stats = !show_stats;
            changed = 1;
        }
        if (state.sent_count != sent_count) {
            sent_count = state.sent_count;
            local.draft[0] = 0;
            local.history_offset = 0;
            changed = 1;
        }
        int current = has_group() ? state.groups[state.selected].link * 64 +
                                        state.groups[state.selected].id
                                  : -1;
        if (current != selected_group) {
            selected_group = current;
            local.history_offset = 0;
        }
        if (pressed) {
            action(pressed);
            changed = 1;
        }
        int t = ui_toast_visible(&state, local.now);
        if (t != toast_on) {
            toast_on = t;
            changed = 1;
        }
        /* Only redraw when something visible changed; an idle screen costs almost nothing. */
        if (changed || state.revision != rendered) {
            uint64_t start = micros();
            ui_render(canvas, width, height, &state, &local);
            if (show_stats)
                ui_performance(fps, frame_us, render_us);
            render_us = (int)(micros() - start);
            if (render_us > worst_render)
                worst_render = render_us;
            rendered = state.revision;
            generation++;
        }
        waitFlip();
        uint64_t flipped = micros();
        frame_sum += flipped - last_flip;
        last_flip = flipped;
        if (buffer_generation[index] != generation) {
            memcpy(buffers[index].ptr, canvas, (size_t)width * height * 4);
            buffer_generation[index] = generation;
        }
        if (!flip(context, index))
            break;
        index ^= 1;
        frames++;
    }
    FILE *perf = fopen(APPDIR "/performance.txt", "w");
    if (perf) {
        fprintf(perf, "Orbit %s; %ux%u; fps=%d; frame=%dus; last_render=%dus; worst_render=%dus\n",
                ORBIT_VERSION, width, height, fps, frame_us, render_us, worst_render);
        fclose(perf);
    }
    app_stop(&application);
    sysUtilUnregisterCallback(0);
    ioPadEnd();
    rsxFinish(context, 1);
    free(canvas);
    ui_close();
    if (!netctl)
        netCtlTerm();
    netDeinitialize();
    crypto_wipe(&local, sizeof(local));
    return 0;
}

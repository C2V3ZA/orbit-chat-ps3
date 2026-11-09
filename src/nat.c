/* Automatic reachability without router configuration by the user:
 * 1. UPnP IGD (SSDP discovery + SOAP AddPortMapping), used by most home routers,
 * 2. PCP / NAT-PMP on the default gateway (UDP 5351).
 * Both only work when the router allows automatic port sharing (factory default on most
 * models). CGNAT / DS-Lite cannot be opened from inside the home network at all. */
#include "orbit.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __PPU__
#include <net/net.h>
#include <net/poll.h>
#define CLOSE(fd) netClose((fd) & ~SOCKET_FD_MASK)
#else
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#define CLOSE close
#endif
#define FRITZ_HINT                                                                                 \
    "FRITZ!Box: einmal erlauben unter fritz.box > Internet > Freigaben > Gerät für Freigaben "     \
    "hinzufügen > PS3 auswählen > Haken bei \"Selbstständige Portfreigaben für dieses Gerät "      \
    "erlauben\". Danach in Orbit: Menü > Verbindung > QUADRAT."
#define NO_UPNP_HINT                                                                               \
    "Dein Router erlaubt keine automatische Freigabe (UPnP aus). Im selben WLAN klappt alles. "    \
    "Übers Internet: Lass deinen Freund die Gruppe erstellen und dich einladen."
static int wait_fd(int fd, int writing, int ms) {
    struct pollfd p = {fd, writing ? POLLOUT : POLLIN, 0};
    return poll(&p, 1, ms) > 0 && (p.revents & (writing ? POLLOUT : POLLIN));
}
static void addr_of(struct sockaddr_in *a, uint32_t ip, int port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    a->sin_addr.s_addr = htonl(ip);
#ifdef __PPU__
    a->sin_len = sizeof(*a);
#endif
}
static uint32_t parse_ip(const char *s) {
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1 ? ntohl(a.s_addr) : 0;
}
static void ip_text(char out[16], uint32_t ip) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}
static int set_nonblock(int fd, int on) {
#ifdef __PPU__
    return setsockopt(fd, SOL_SOCKET, SO_NBIO, &on, sizeof(on));
#else
    int f = fcntl(fd, F_GETFL, 0);
    return f < 0 ? -1 : fcntl(fd, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
#endif
}
/* One HTTP/1.1 request with Connection: close; returns body length or -1. */
static int http(uint32_t ip, int port, const char *request, char *out, int cap, int *status) {
    struct sockaddr_in a;
    addr_of(&a, ip, port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    set_nonblock(fd, 1);
    int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (rc && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        CLOSE(fd);
        return -1;
    }
    if (rc) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (!wait_fd(fd, 1, 2000) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
            CLOSE(fd);
            return -1;
        }
    }
    set_nonblock(fd, 0);
    size_t n = strlen(request), sent = 0;
    while (sent < n) {
        if (!wait_fd(fd, 1, 2000)) {
            CLOSE(fd);
            return -1;
        }
        int k = (int)send(fd, request + sent, n - sent, 0);
        if (k <= 0) {
            CLOSE(fd);
            return -1;
        }
        sent += (size_t)k;
    }
    int got = 0;
    uint64_t end = orbit_millis() + 3000;
    while (got < cap - 1 && orbit_millis() < end) {
        if (!wait_fd(fd, 0, 500))
            continue;
        int k = (int)recv(fd, out + got, (size_t)(cap - 1 - got), 0);
        if (k <= 0)
            break;
        got += k;
    }
    CLOSE(fd);
    out[got] = 0;
    *status = 0;
    if (strncmp(out, "HTTP/1.", 7) || got < 12)
        return -1;
    *status = atoi(out + 9);
    char *body = strstr(out, "\r\n\r\n");
    if (!body)
        return -1;
    body += 4;
    int len = got - (int)(body - out);
    memmove(out, body, (size_t)len + 1);
    return len;
}
static const char *find_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n))
            return hay;
    return NULL;
}
/* Copies the text of <tag>...</tag> following position start (namespace prefixes allowed). */
static int xml_value(const char *start, const char *tag, char *out, size_t cap) {
    const char *p = start;
    size_t n = strlen(tag);
    while ((p = strchr(p, '<'))) {
        const char *name = p + 1;
        if (*name == '/') {
            p++;
            continue;
        }
        const char *colon = strpbrk(name, ":> ");
        if (colon && *colon == ':')
            name = colon + 1;
        if (!strncmp(name, tag, n) && (name[n] == '>' || name[n] == ' ')) {
            const char *v = strchr(name, '>');
            if (!v)
                return -1;
            v++;
            const char *e = strchr(v, '<');
            if (!e || (size_t)(e - v) >= cap)
                return -1;
            while (v < e && isspace((unsigned char)*v))
                v++;
            size_t k = (size_t)(e - v);
            while (k && isspace((unsigned char)v[k - 1]))
                k--;
            memcpy(out, v, k);
            out[k] = 0;
            return 0;
        }
        p++;
    }
    return -1;
}
/* http://a.b.c.d:port/path */
static int split_url(const char *url, uint32_t *ip, int *port, char *path, size_t cap) {
    if (strncasecmp(url, "http://", 7))
        return -1;
    const char *h = url + 7, *slash = strchr(h, '/');
    char host[64];
    size_t n = slash ? (size_t)(slash - h) : strlen(h);
    if (!n || n >= sizeof(host))
        return -1;
    memcpy(host, h, n);
    host[n] = 0;
    char *colon = strchr(host, ':');
    *port = 80;
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
    }
    *ip = parse_ip(host);
    snprintf(path, cap, "%s", slash ? slash : "/");
    return *ip && *port > 0 && *port < 65536 ? 0 : -1;
}
static int soap(const OrbitNat *n, const char *action, const char *args, char *out, int cap,
                int *status) {
    uint32_t ip;
    int port;
    char path[256];
    if (split_url(n->control, &ip, &port, path, sizeof(path)))
        return -1;
    char body[1400], request[2200];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\"?>\r\n<s:Envelope "
             "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s "
             "xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
             action, n->service, args, action);
    char host[16];
    ip_text(host, ip);
    snprintf(request, sizeof(request),
             "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "SOAPAction: \"%s#%s\"\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
             path, host, port, n->service, action, (unsigned)strlen(body), body);
    return http(ip, port, request, out, cap, status);
}
static int upnp_find(OrbitNat *n) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in to;
    addr_of(&to, 0xeffffffa, 1900); /* 239.255.255.250 */
    const char *targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
    for (int i = 0; i < 3; i++) {
        char q[256];
        int len = snprintf(q, sizeof(q),
                           "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "
                           "\"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n",
                           targets[i]);
        sendto(fd, q, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to));
    }
    char locations[4][200];
    int count = 0;
    uint64_t end = orbit_millis() + 2500;
    while (orbit_millis() < end && count < 4) {
        if (!wait_fd(fd, 0, 200))
            continue;
        char buf[1500];
        int k = (int)recv(fd, buf, sizeof(buf) - 1, 0);
        if (k <= 0)
            continue;
        buf[k] = 0;
        const char *loc = find_ci(buf, "\nLOCATION:");
        if (!loc)
            continue;
        loc += 10;
        while (*loc == ' ')
            loc++;
        size_t m = strcspn(loc, "\r\n");
        if (!m || m >= sizeof(locations[0]))
            continue;
        char url[200];
        memcpy(url, loc, m);
        url[m] = 0;
        int dup = 0;
        for (int i = 0; i < count; i++)
            dup |= !strcmp(locations[i], url);
        if (!dup)
            strcpy(locations[count++], url);
    }
    CLOSE(fd);
    static char xml[65536];
    for (int i = 0; i < count; i++) {
        uint32_t ip;
        int port, status;
        char path[200], host[16], request[512];
        if (split_url(locations[i], &ip, &port, path, sizeof(path)))
            continue;
        ip_text(host, ip);
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
        if (http(ip, port, request, xml, sizeof(xml), &status) <= 0 || status != 200)
            continue;
        char model[64] = {0};
        if (xml_value(xml, "modelName", model, sizeof(model)))
            xml_value(xml, "friendlyName", model, sizeof(model));
        snprintf(n->router, sizeof(n->router), "%s", model);
        if (strstr(xml, "FRITZ!Box") || strstr(xml, "AVM"))
            n->fritz = 1;
        const char *types[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                               "urn:schemas-upnp-org:service:WANIPConnection:1",
                               "urn:schemas-upnp-org:service:WANPPPConnection:1"};
        for (int t = 0; t < 3; t++) {
            const char *at = strstr(xml, types[t]);
            char control[200];
            if (!at || xml_value(at, "controlURL", control, sizeof(control)))
                continue;
            snprintf(n->service, sizeof(n->service), "%s", types[t]);
            if (!strncasecmp(control, "http://", 7))
                snprintf(n->control, sizeof(n->control), "%s", control);
            else
                snprintf(n->control, sizeof(n->control), "http://%s:%d%s%s", host, port,
                         control[0] == '/' ? "" : "/", control);
            return 0;
        }
    }
    return -1;
}
static int upnp_map(OrbitNat *n, const char *local_ip, int internal) {
    static char reply[16384];
    int status;
    if (soap(n, "GetExternalIPAddress", "", reply, sizeof(reply), &status) < 0 || status != 200 ||
        xml_value(reply, "NewExternalIPAddress", n->external, sizeof(n->external)))
        return -1;
    for (int attempt = 0; attempt < 6; attempt++) {
        int ext = internal + attempt;
        char args[900], query[200];
        /* Reuse an existing mapping for this console (for example after a crash). */
        snprintf(query, sizeof(query),
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort>"
                 "<NewProtocol>TCP</NewProtocol>",
                 ext);
        char client[32] = {0};
        if (soap(n, "GetSpecificPortMappingEntry", query, reply, sizeof(reply), &status) >= 0 &&
            status == 200 && !xml_value(reply, "NewInternalClient", client, sizeof(client))) {
            if (strcmp(client, local_ip))
                continue; /* used by another device */
        }
        static const char *leases[] = {"0", "86400"};
        for (int l = 0; l < 2; l++) {
            snprintf(args, sizeof(args),
                     "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort>"
                     "<NewProtocol>TCP</NewProtocol><NewInternalPort>%d</NewInternalPort>"
                     "<NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                     "<NewPortMappingDescription>Orbit PS3</NewPortMappingDescription>"
                     "<NewLeaseDuration>%s</NewLeaseDuration>",
                     ext, internal, local_ip, leases[l]);
            if (soap(n, "AddPortMapping", args, reply, sizeof(reply), &status) >= 0 &&
                status == 200) {
                n->port = ext;
                return 0;
            }
            char code[16] = {0};
            xml_value(reply, "errorCode", code, sizeof(code));
            if (!strcmp(code, "718"))
                break; /* conflict: next port */
            if (!strcmp(code, "606") || !strcmp(code, "401") || !strcmp(code, "501") ||
                n->fritz) {
                snprintf(n->reason, sizeof(n->reason), "%s",
                         n->fritz ? FRITZ_HINT
                                  : "Der Router erlaubt der PS3 keine automatische Freigabe. In "
                                    "den Router-Einstellungen UPnP für die PS3 erlauben - oder "
                                    "dein Freund lädt dich ein.");
                return -1;
            }
        }
    }
    return -1;
}
static int udp_ask(uint32_t gateway, const uint8_t *q, int qn, uint8_t *r, int rn) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in to;
    addr_of(&to, gateway, 5351);
    int got = -1;
    for (int attempt = 0, wait = 250; attempt < 3 && got < 0; attempt++, wait *= 2) {
        sendto(fd, q, (size_t)qn, 0, (struct sockaddr *)&to, sizeof(to));
        if (wait_fd(fd, 0, wait))
            got = (int)recv(fd, r, (size_t)rn, 0);
    }
    CLOSE(fd);
    return got;
}
static int pcp_map(OrbitNat *n, uint32_t gateway, uint32_t local, int internal) {
    uint8_t q[60] = {2, 1}, r[1100];
    uint32_t life = 7200;
    q[4] = life >> 24, q[5] = life >> 16, q[6] = life >> 8, q[7] = (uint8_t)life;
    q[18] = q[19] = 0xff;
    q[20] = local >> 24, q[21] = local >> 16, q[22] = local >> 8, q[23] = (uint8_t)local;
    if (orbit_random(q + 24, 12))
        return -1;
    q[36] = 6; /* TCP */
    q[40] = (uint8_t)(internal >> 8), q[41] = (uint8_t)internal;
    q[42] = (uint8_t)(internal >> 8), q[43] = (uint8_t)internal;
    q[54] = q[55] = 0xff;
    int k = udp_ask(gateway, q, 60, r, sizeof(r));
    if (k >= 4 && r[0] == 2 && r[1] == 0x81 && r[3] == 2) {
        /* NOT_AUTHORIZED: typical FRITZ!Box answer before the device was allowed */
        n->fritz = 1;
        snprintf(n->reason, sizeof(n->reason), "%s", FRITZ_HINT);
        return -1;
    }
    if (k >= 60 && r[0] == 2 && r[1] == 0x81 && r[3] == 0 && !memcmp(r + 24, q + 24, 12)) {
        n->port = r[42] << 8 | r[43];
        ip_text(n->external, (uint32_t)r[56] << 24 | (uint32_t)r[57] << 16 |
                                 (uint32_t)r[58] << 8 | r[59]);
        return n->port ? 0 : -1;
    }
    if (k >= 8 && r[0] == 0) { /* NAT-PMP only gateway */
        uint8_t e[2] = {0, 0}, er[16];
        if (udp_ask(gateway, e, 2, er, sizeof(er)) < 12 || er[1] != 128 || er[2] || er[3])
            return -1;
        ip_text(n->external, (uint32_t)er[8] << 24 | (uint32_t)er[9] << 16 |
                                 (uint32_t)er[10] << 8 | er[11]);
        uint8_t m[12] = {0, 2, 0, 0, (uint8_t)(internal >> 8), (uint8_t)internal,
                         (uint8_t)(internal >> 8), (uint8_t)internal, 0, 0, 0x1c, 0x20};
        uint8_t mr[16];
        if (udp_ask(gateway, m, 12, mr, sizeof(mr)) < 16 || mr[1] != 130 || mr[2] || mr[3])
            return -1;
        n->port = mr[10] << 8 | mr[11];
        return n->port ? 0 : -1;
    }
    return -1;
}
int orbit_nat_open(const char *local_ip, const char *gateway, int internal, OrbitNat *n) {
    memset(n, 0, sizeof(*n));
    uint32_t local = parse_ip(local_ip), gw = parse_ip(gateway);
    snprintf(n->reason, sizeof(n->reason), "%s", NO_UPNP_HINT);
    if (!local || (local >> 24) == 127) {
        snprintf(n->reason, sizeof(n->reason), "Keine Netzwerkverbindung.");
        return -1;
    }
    if (!orbit_ipv4_private(local)) {
        /* Directly on the internet (rare): nothing to open. */
        snprintf(n->external, sizeof(n->external), "%s", local_ip);
        n->port = internal;
        n->ok = 1;
        snprintf(n->method, sizeof(n->method), "direkt");
        return 0;
    }
    int mapped = -1;
    if (!upnp_find(n) && !upnp_map(n, local_ip, internal)) {
        mapped = 0;
        snprintf(n->method, sizeof(n->method), "UPnP");
    } else {
        char why[300];
        int fritz = n->fritz;
        snprintf(why, sizeof(why), "%s", n->reason);
        n->control[0] = 0;
        if (gw && !pcp_map(n, gw, local, internal)) {
            mapped = 0;
            snprintf(n->method, sizeof(n->method), "PCP");
        } else if (!n->fritz || fritz)
            snprintf(n->reason, sizeof(n->reason), "%s", why);
    }
    if (mapped)
        return -1;
    uint32_t ext = parse_ip(n->external);
    if (!ext || orbit_ipv4_private(ext)) {
        n->cgnat = 1;
        snprintf(n->reason, sizeof(n->reason),
                 "Dein Anschluss hat keine eigene IPv4-Adresse (CGNAT/DS-Lite). Lass deinen "
                 "Freund einladen - oder trefft euch im selben Heimnetz.");
        orbit_nat_close(n, gateway);
        return -1;
    }
    n->ok = 1;
    n->reason[0] = 0;
    return 0;
}
void orbit_nat_close(OrbitNat *n, const char *gateway) {
    (void)gateway;
    if (n->control[0] && n->port) {
        static char reply[4096];
        char args[200];
        int status;
        snprintf(args, sizeof(args),
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort>"
                 "<NewProtocol>TCP</NewProtocol>",
                 n->port);
        soap(n, "DeletePortMapping", args, reply, sizeof(reply), &status);
    }
    n->ok = 0;
}

#include "orbit.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __PPU__
#include <lv2/system.h>
#else
#include <errno.h>
#include <sys/random.h>
#endif
int orbit_group_code(char out[33], const char *text) {
    if (!text || strlen(text) > 48)
        return -1;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == ' ' || *p == '-')
            continue;
        if (*p > 127 || n == 32)
            return -1;
        out[n++] = (char)toupper(*p);
    }
    out[n] = 0;
    if (n == 8)
        return strspn(out, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789") == 8 ? 0 : -1;
    /* Keep existing 0.4 group invitations valid. */
    if (n == 32) {
        for (size_t i = 0; i < n; i++)
            out[i] = (char)tolower((unsigned char)out[i]);
        return strspn(out, "0123456789abcdef") == 32 ? 0 : -1;
    }
    return -1;
}
int orbit_parse_endpoint(const char *text, char host[ORBIT_HOST], int *port) {
    if (!text || !*text || strlen(text) > 102)
        return -1;
    const char *colon = strchr(text, ':');
    size_t n = colon ? (size_t)(colon - text) : strlen(text);
    if (!n || n >= ORBIT_HOST)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c > 127 || !(isalnum(c) || c == '-' || c == '.'))
            return -1;
        if ((c == '.' || c == '-') && (!i || i == n - 1))
            return -1;
        if (c == '.' && i && text[i - 1] == '.')
            return -1;
    }
    *port = ORBIT_PORT;
    if (colon) {
        if (!colon[1] || strlen(colon + 1) > 5)
            return -1;
        for (const char *p = colon + 1; *p; p++)
            if (!isdigit((unsigned char)*p))
                return -1;
        long value = strtol(colon + 1, NULL, 10);
        if (value < 1 || value > 65535)
            return -1;
        *port = (int)value;
    }
    memcpy(host, text, n);
    host[n] = 0;
    return 0;
}
int orbit_parse_join(const char *text, char host[ORBIT_HOST], int *port, uint8_t key[32],
                     char group[33]) {
    group[0] = 0;
    if (!text || strlen(text) > 239 || strncmp(text, "orbit://", 8))
        return -1;
    const char *slash = strchr(text + 8, '/');
    if (!slash || slash - text - 8 > 102)
        return -1;
    char endpoint[104], secret[65];
    size_t n = (size_t)(slash - text - 8);
    memcpy(endpoint, text + 8, n);
    endpoint[n] = 0;
    const char *query = strchr(slash + 1, '?');
    size_t keylen = query ? (size_t)(query - slash - 1) : strlen(slash + 1);
    if (keylen != 64)
        return -1;
    memcpy(secret, slash + 1, 64);
    secret[64] = 0;
    if (orbit_parse_endpoint(endpoint, host, port) || orbit_unhex(key, secret, 32))
        return -1;
    if (query && (strncmp(query, "?g=", 3) || orbit_group_code(group, query + 3)))
        return -1;
    return 0;
}
int orbit_parse_invite(const char *text, char host[ORBIT_HOST], int *port, uint8_t key[32]) {
    char group[33];
    return orbit_parse_join(text, host, port, key, group);
}
int orbit_random(void *buf, size_t n) {
#ifdef __PPU__
    unsigned char *p = buf;
    while (n) {
        size_t k = n > 256 ? 256 : n;
        if (sysGetRandomNumber(p, k))
            return -1;
        p += k;
        n -= k;
    }
    return 0;
#else
    unsigned char *p = buf;
    while (n) {
        ssize_t k = getrandom(p, n, 0);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
#endif
}
uint64_t orbit_millis(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000 + t.tv_usec / 1000;
}
void orbit_hex(char *out, const uint8_t *in, size_t n) {
    const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 15];
    }
    out[n * 2] = 0;
}
int orbit_unhex(uint8_t *out, const char *s, size_t n) {
    if (!s || strlen(s) != n * 2)
        return -1;
    for (size_t i = 0; i < n; i++) {
        int a = s[2 * i], b = s[2 * i + 1];
        a = a >= '0' && a <= '9'   ? a - '0'
            : a >= 'a' && a <= 'f' ? a - 'a' + 10
            : a >= 'A' && a <= 'F' ? a - 'A' + 10
                                   : -1;
        b = b >= '0' && b <= '9'   ? b - '0'
            : b >= 'a' && b <= 'f' ? b - 'a' + 10
            : b >= 'A' && b <= 'F' ? b - 'A' + 10
                                   : -1;
        if (a < 0 || b < 0)
            return -1;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 0;
}
/* Strict UTF-8: reject controls, overlong encodings and surrogate code points. */
int orbit_text_valid(const char *s, size_t min, size_t max) {
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (n < min || n > max)
        return 0;
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    int visible = 0;
    while (p < end) {
        uint32_t c = *p++;
        int k = 0;
        uint32_t lo = 0;
        if (c < 128) {
            if (c < 32 || c == 127)
                return 0;
            if (c != ' ')
                visible = 1;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            k = 1;
            c &= 31;
            lo = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            k = 2;
            c &= 15;
            lo = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            k = 3;
            c &= 7;
            lo = 65536;
        } else
            return 0;
        while (k--) {
            if (p >= end || (*p & 192) != 128)
                return 0;
            c = (c << 6) | (*p++ & 63);
        }
        if (c < lo || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return 0;
        visible = 1;
    }
    return visible || min == 0;
}
int orbit_keyfile(const char *path, uint8_t key[32]) {
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(key, 1, 32, f);
        int end = fgetc(f);
        fclose(f);
        return n == 32 && end == EOF ? 0 : -1;
    }
    if (orbit_random(key, 32))
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(key, 1, 32, f) == 32;
    if (fflush(f))
        ok = 0;
    if (fclose(f))
        ok = 0;
    chmod(path, 0600);
    return ok ? 0 : -1;
}
#include "monocypher.h"
static const char invite_alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
/* Fixed whitening so codes look random even for the relay address 0.0.0.0. */
static const uint8_t invite_mask[11] = {0x5a, 0xc3, 0x91, 0x3e, 0x7b, 0x24,
                                        0xe8, 0x0f, 0xb6, 0x4d, 0x92};
uint32_t orbit_unix_time(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint32_t)t.tv_sec;
}
int orbit_ipv4_private(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8 || (ip >> 24) == 127 ||
           (ip >> 22) == (100u << 2 | 1) || (ip >> 16) == 0xa9fe || ip == 0;
}
static void invite_check(uint8_t out[2], const uint8_t data[11], int relay) {
    uint8_t h[32], in[12];
    memcpy(in, data, 11);
    in[11] = (uint8_t)relay;
    crypto_blake2b_keyed(h, 32, (const uint8_t *)"orbit-invite-v7", 15, in, 12);
    out[0] = h[0];
    out[1] = h[1] & 0xe0;
}
/* 100 bits: IPv4 (32) port (16) code (40) relay flag (1) check (11), 20 base32 symbols.
 * ORBIT_INVITE_RELAY in the port argument marks "address is a relay" (any port 1..65535). */
int orbit_invite_encode(char out[ORBIT_INVITE_TEXT], uint32_t ip, int port, const char *code) {
    char norm[33];
    int relay = (port & ORBIT_INVITE_RELAY) != 0;
    port &= 0xffff;
    if (port < 1 || orbit_group_code(norm, code) || strlen(norm) != 8)
        return -1;
    uint8_t bits[13] = {0};
    bits[0] = ip >> 24, bits[1] = ip >> 16, bits[2] = ip >> 8, bits[3] = ip;
    bits[4] = (uint8_t)(port >> 8), bits[5] = (uint8_t)port;
    uint64_t c = 0;
    for (int i = 0; i < 8; i++)
        c = c << 5 | (uint64_t)(strchr(invite_alphabet, norm[i]) - invite_alphabet);
    for (int i = 0; i < 5; i++)
        bits[6 + i] = (uint8_t)(c >> (32 - 8 * i));
    invite_check(bits + 11, bits, relay);
    if (relay)
        bits[12] |= 0x10;
    for (int i = 0; i < 11; i++)
        bits[i] ^= invite_mask[i];
    int n = 0;
    for (int i = 0; i < 20; i++) {
        int bit = i * 5, v = 0;
        for (int k = 0; k < 5; k++, bit++)
            v = v << 1 | ((bits[bit >> 3] >> (7 - (bit & 7))) & 1);
        if (i && i % 5 == 0)
            out[n++] = '-';
        out[n++] = invite_alphabet[v];
    }
    out[n] = 0;
    return 0;
}
int orbit_invite_decode(const char *text, uint32_t *ip, int *port, char code[9]) {
    if (!text || strlen(text) > 64)
        return -1;
    int symbols[20], n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int c = toupper(*p);
        if (c == ' ' || c == '-' || c == '.' || c == '_')
            continue;
        if (c == '0')
            c = 'O';
        if (c == '1')
            c = 'I';
        if (c == 'O' || c == 'I')
            return -1; /* not in the alphabet: typing mistake */
        const char *at = c ? strchr(invite_alphabet, c) : NULL;
        if (!at || n == 20)
            return -1;
        symbols[n++] = (int)(at - invite_alphabet);
    }
    if (n != 20)
        return -1;
    uint8_t bits[13] = {0};
    for (int i = 0, bit = 0; i < 20; i++)
        for (int k = 4; k >= 0; k--, bit++)
            if (symbols[i] >> k & 1)
                bits[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
    uint8_t check[2];
    for (int i = 0; i < 11; i++)
        bits[i] ^= invite_mask[i];
    int relay = (bits[12] & 0x10) != 0;
    invite_check(check, bits, relay);
    if (check[0] != bits[11] || check[1] != (bits[12] & 0xe0) || (bits[12] & 0x0f))
        return -1;
    *ip = (uint32_t)bits[0] << 24 | (uint32_t)bits[1] << 16 | (uint32_t)bits[2] << 8 | bits[3];
    *port = bits[4] << 8 | bits[5];
    uint64_t c = 0;
    for (int i = 0; i < 5; i++)
        c = c << 8 | bits[6 + i];
    for (int i = 7; i >= 0; i--, c >>= 5)
        code[i] = invite_alphabet[c & 31];
    code[8] = 0;
    if (!*port)
        return -1;
    if (relay)
        *port |= ORBIT_INVITE_RELAY;
    return 0;
}
void orbit_group_tag(char out[17], const char *code) {
    uint8_t h[8];
    crypto_blake2b_keyed(h, 8, (const uint8_t *)"orbit-tag-v7", 12, (const uint8_t *)code,
                         strlen(code));
    orbit_hex(out, h, 8);
}
int orbit_group_psk(uint8_t psk[32], const char *code) {
    /* Small Argon2id stretch: cheap once per connection, costly for code guessing. */
    void *work = malloc(256 * 1024);
    if (!work)
        return -1;
    static const uint8_t salt[17] = "orbit-psk-v7-ps3";
    crypto_argon2_config c = {CRYPTO_ARGON2_ID, 256, 2, 1};
    crypto_argon2_inputs in = {(const uint8_t *)code, salt, (uint32_t)strlen(code), 16};
    crypto_argon2(psk, 32, work, c, in, crypto_argon2_no_extras);
    crypto_wipe(work, 256 * 1024);
    free(work);
    return 0;
}

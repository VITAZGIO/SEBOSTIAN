#include "seb_proto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int seb_csv_ints(const char *s, long *out, int max)
{
    int n = 0;
    while (s && *s && n < max) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s) break;                 // не число — дальше не разбираем
        out[n++] = v;
        s = end;
        while (*s == ' ') s++;
        if (*s != ',') break;
        s++;
    }
    return n;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src[i] && src[i] != '\r' && src[i] != '\n' && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

bool seb_proto_parse_cp(const char *s, seb_cp_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->mux = -1;
    if (!s || !*s) return false;
    long v[12];
    int k;
    if (s[1] == ',') {
        const char *p = s + 2;
        switch (s[0]) {
        case 'S':
            k = seb_csv_ints(p, v, 9);
            if (k < 5) break;
            m->type = CP_STATUS;
            m->t1 = v[0]; m->t2 = v[1]; m->t3 = v[2]; m->mic = v[3]; m->bat_mv = v[4];
            m->uptime = k > 5 ? (uint32_t)v[5] : 0;
            m->bt = k > 6 ? v[6] : 0;
            m->music = k > 7 ? v[7] : 0;
            m->mux = k > 8 ? v[8] : -1;
            return true;
        case 'T':
            if (seb_csv_ints(p, v, 1) < 1) break;
            m->type = CP_TOUCH; m->n = v[0];
            return true;
        case 'P':
            m->type = CP_PONG; m->code = strtoul(p, NULL, 10); m->n = (int)m->code;
            return true;
        case 'R': case 'L': case 'X':
            k = seb_csv_ints(p, v, 2);
            if (k < 1) break;
            m->type = s[0] == 'R' ? CP_RF_TRIG : s[0] == 'L' ? CP_RF_LIGHT : CP_RF_SENT;
            m->n = v[0];
            m->code = k > 1 ? (uint32_t)strtoul(strchr(p, ',') + 1, NULL, 10) : 0;
            return true;
        case 'U':
            m->type = CP_RF_UNK; m->code = strtoul(p, NULL, 10);
            return true;
        case 'W': {
            k = seb_csv_ints(p, v, 2);
            if (k < 1) break;
            m->type = CP_OTA;
            m->ota_state = v[0];
            m->ota_pct = k > 1 ? v[1] : 0;
            const char *c = strchr(p, ',');
            if (c) c = strchr(c + 1, ',');
            if (c) copy_str(m->text, sizeof(m->text), c + 1);
            return true;
        }
        case 'B':
            k = seb_csv_ints(p, v, 2);
            if (k < 1) break;
            m->type = CP_BAT; m->bat_level = v[0]; m->bat_mv = k > 1 ? v[1] : 0;
            return true;
        case 'G':
            m->type = CP_GRACE; m->bat_mv = atoi(p);
            return true;
        case 'V':
            m->type = CP_VERSION; copy_str(m->text, sizeof(m->text), p);
            return true;
        case 'A':
            k = seb_csv_ints(p, v, 3);
            if (k < 3) break;
            m->type = CP_ALINK; m->a_ok = (uint32_t)v[0]; m->a_crc = (uint32_t)v[1]; m->a_lost = (uint32_t)v[2];
            return true;
        default:
            break;
        }
        m->type = CP_UNKNOWN;
        return false;
    }
    if (!strcmp(s, "F")) { m->type = CP_FORCE_BTN; return true; }
    if (!strncmp(s, "HELLO", 5)) {
        m->type = CP_HELLO;
        copy_str(m->text, sizeof(m->text), s[5] == ' ' ? s + 6 : s + 5);
        return true;
    }
    if (!strncmp(s, "CFG?", 4)) { m->type = CP_CFG_REQ; return true; }
    m->type = CP_UNKNOWN;
    return false;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// hex-слово до пробела/конца -> строка. "-" = пустая строка.
static const char *unhex_word(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    while (*p == ' ') p++;
    if (*p == '-') { p++; out[0] = 0; return p; }
    while (*p && *p != ' ') {
        int a = hexval(p[0]), b = p[1] ? hexval(p[1]) : -1;
        if (a < 0 || b < 0) break;
        if (n < cap - 1) out[n++] = (char)(a * 16 + b);
        p += 2;
    }
    out[n] = 0;
    return p;
}

bool seb_proto_parse_hc(const char *s, seb_hc_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    if (!s || !*s) return false;
    if (!strncmp(s, "PING", 4)) { m->type = HC_PING; m->n = s[4] ? strtoul(s + 5, NULL, 10) : 0; return true; }
    if (!strncmp(s, "S3B", 3)) { m->type = HC_S3B; m->n = s[3] ? strtoul(s + 4, NULL, 10) : 0; return true; }
    if (!strcmp(s, "BEEP")) { m->type = HC_BEEP; return true; }
    if (!strcmp(s, "MUSIC")) { m->type = HC_MUSIC; return true; }
    if (!strcmp(s, "LED")) { m->type = HC_LED; return true; }
    if (!strcmp(s, "RFHOLD")) { m->type = HC_RFHOLD; return true; }
    if (!strncmp(s, "RF ", 3)) { m->type = HC_RF; m->n = strtoul(s + 3, NULL, 10); return true; }
    if (!strcmp(s, "OTA")) { m->type = HC_OTA; return true; }
    if (!strncmp(s, "SLEEP", 5)) { m->type = HC_SLEEP; return true; }
    if (!strncmp(s, "FORCE", 5)) { m->type = HC_FORCE; return true; }
    if (!strncmp(s, "DUCK", 4)) { m->type = HC_DUCK; m->n = s[4] ? strtoul(s + 5, NULL, 10) : 0; return true; }
    if (!strncmp(s, "AROUTE", 6)) {
        long v[3] = {0, 1, 0};
        if (seb_csv_ints(s[6] == ' ' ? s + 7 : s + 6, v, 3) < 0) return false;
        m->type = HC_AROUTE;
        m->bt_spk = v[0] != 0;
        m->bt_aux = v[1] != 0;
        m->head_aux = v[2] != 0;
        return true;
    }
    if (!strncmp(s, "CFG ", 4)) {
        const char *p = s + 4;
        p = unhex_word(p, m->ssid, sizeof(m->ssid));
        p = unhex_word(p, m->pass, sizeof(m->pass));
        unhex_word(p, m->ota, sizeof(m->ota));
        m->type = HC_CFG;
        return true;
    }
    m->type = HC_UNKNOWN;
    return false;
}

static int put_hex(char *out, size_t cap, size_t pos, const char *s)
{
    static const char H[] = "0123456789abcdef";
    if (!s || !*s) {
        if (pos + 1 >= cap) return -1;
        out[pos++] = '-';
        return (int)pos;
    }
    for (; *s; s++) {
        if (pos + 2 >= cap) return -1;
        out[pos++] = H[((unsigned char)*s) >> 4];
        out[pos++] = H[((unsigned char)*s) & 15];
    }
    return (int)pos;
}

int seb_proto_fmt_cfg(char *out, size_t cap, const char *ssid, const char *pass, const char *ota)
{
    if (cap < 8) return -1;
    int p = 0;
    memcpy(out, "CFG ", 4); p = 4;
    if ((p = put_hex(out, cap, p, ssid)) < 0) return -1;
    if ((size_t)p + 1 >= cap) return -1;
    out[p++] = ' ';
    if ((p = put_hex(out, cap, p, pass)) < 0) return -1;
    if ((size_t)p + 1 >= cap) return -1;
    out[p++] = ' ';
    if ((p = put_hex(out, cap, p, ota)) < 0) return -1;
    if ((size_t)p >= cap) return -1;
    out[p] = 0;
    return p;
}

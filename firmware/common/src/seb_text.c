// Текстовые утилиты: base64, мини-JSON, построчный приёмник, транслит, слово «Себастьян».
#include "seb_util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// ---------------- base64 ----------------
static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t seb_b64_decode_inplace(char *s, size_t n)
{
    uint8_t *o = (uint8_t *)s;
    size_t w = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64v(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; o[w++] = (uint8_t)((acc >> bits) & 0xFF); }
    }
    return w;
}

// ---------------- мини-JSON ----------------
const char *seb_json_val(const char *s, const char *key)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pl = strlen(pat);
    const char *p = strstr(s, pat);
    while (p) {
        const char *q = p + pl;
        while (*q == ' ') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ') q++;
            return q;
        }
        p = strstr(q, pat);
    }
    return NULL;
}

bool seb_json_num(const char *s, const char *key, float *v)
{
    const char *q = seb_json_val(s, key);
    if (!q) return false;
    char *end;
    float f = strtof(q, &end);
    if (end == q) return false;
    *v = f;
    return true;
}

static size_t put_utf8(char *o, size_t pos, size_t cap, uint32_t cp)
{
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    if (pos + n >= cap) return pos;
    memcpy(o + pos, b, n);
    return pos + n;
}

bool seb_json_str(const char *s, const char *key, char *o, size_t cap)
{
    const char *q = seb_json_val(s, key);
    if (!q || *q != '"' || cap == 0) return false;
    q++;
    size_t n = 0;
    while (*q && *q != '"') {
        if (*q != '\\') {
            if (n + 1 < cap) o[n++] = *q;
            q++;
            continue;
        }
        q++;
        switch (*q) {
        case 'n': case 'r': case 't':
            if (n + 1 < cap) o[n++] = ' ';
            q++;
            break;
        case 'u': {
            char h[5] = {0};
            for (int i = 0; i < 4 && q[1 + i]; i++) h[i] = q[1 + i];
            uint32_t cp = strtoul(h, NULL, 16);
            q += 1 + strlen(h);
            if (cp >= 0xD800 && cp <= 0xDFFF) {          // эмодзи (суррогатная пара) -> '?'
                if (n + 1 < cap) o[n++] = '?';
                if (q[0] == '\\' && q[1] == 'u') q += 6;
            } else {
                n = put_utf8(o, n, cap, cp);
            }
        } break;
        case 0:
            break;
        default:
            if (n + 1 < cap) o[n++] = *q;
            q++;
            break;
        }
    }
    o[n] = 0;
    return true;
}

size_t seb_json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            if (n + 2 >= cap) break;
            out[n++] = '\\';
            out[n++] = (char)c;
        } else {
            if (n + 1 >= cap) break;
            out[n++] = c < 0x20 ? ' ' : (char)c;
        }
    }
    out[n] = 0;
    return n;
}

// ---------------- построчный приёмник ----------------
void seb_linebuf_init(seb_linebuf_t *lb, char *buf, size_t cap)
{
    lb->buf = buf;
    lb->cap = cap;
    lb->len = 0;
    lb->overflow = false;
    if (cap) buf[0] = 0;
}

void seb_linebuf_reset(seb_linebuf_t *lb)
{
    lb->len = 0;
    lb->overflow = false;
    lb->buf[0] = 0;
}

bool seb_linebuf_put(seb_linebuf_t *lb, char c)
{
    if (c == '\r') return false;
    if (c == '\n') {
        lb->buf[lb->len] = 0;
        return true;
    }
    if (lb->len + 1 < lb->cap) lb->buf[lb->len++] = c;
    else lb->overflow = true;
    return false;
}

// ---------------- транслит ----------------
static const char *LAT[32] = {"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
                              "r", "s", "t", "u", "f", "h", "ts", "ch", "sh", "sch", "", "y", "'", "e", "yu", "ya"};

static size_t put_str(char *out, size_t pos, size_t cap, const char *s)
{
    while (*s && pos + 1 < cap) out[pos++] = *s++;
    return pos;
}

size_t seb_translit(const char *u, char *out, size_t cap)
{
    const uint8_t *p = (const uint8_t *)u;
    size_t n = 0;
    if (!cap) return 0;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { p += 4; n = put_str(out, n, cap, "?"); continue; }
        else { p++; continue; }
        if (cp < 0x80) { if (n + 1 < cap) out[n++] = (char)cp; continue; }
        if (cp == 0x401) { n = put_str(out, n, cap, "Yo"); continue; }
        if (cp == 0x451) { n = put_str(out, n, cap, "yo"); continue; }
        bool up = false;
        if (cp >= 0x410 && cp <= 0x42F) { up = true; cp += 0x20; }
        if (cp >= 0x430 && cp <= 0x44F) {
            const char *t = LAT[cp - 0x430];
            if (up && *t) {
                if (n + 1 < cap) out[n++] = (char)toupper((unsigned char)t[0]);
                t++;
            }
            n = put_str(out, n, cap, t);
        } else if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D || cp == 0x201E) n = put_str(out, n, cap, "\"");
        else if (cp == 0x2013 || cp == 0x2014) n = put_str(out, n, cap, "-");
        else if (cp == 0x2026) n = put_str(out, n, cap, "...");
        else if (cp == 0xA0) n = put_str(out, n, cap, " ");
        else n = put_str(out, n, cap, "?");
    }
    out[n] = 0;
    return n;
}

// ---------------- слово «Себастьян» ----------------
static const char *WAKE_KEYS[] = {"sebas", "sebos", "sevas", "sebus", "sabas", "sebes"};

static bool is_wake_word(const char *w, size_t len)
{
    char tmp[64], tr[128], letters[64];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, w, len);
    tmp[len] = 0;
    seb_translit(tmp, tr, sizeof(tr));
    size_t k = 0;
    for (size_t i = 0; tr[i] && k + 1 < sizeof(letters); i++)
        if (isalpha((unsigned char)tr[i])) letters[k++] = (char)tolower((unsigned char)tr[i]);
    letters[k] = 0;
    for (size_t i = 0; i < sizeof(WAKE_KEYS) / sizeof(WAKE_KEYS[0]); i++)
        if (!strncmp(letters, WAKE_KEYS[i], strlen(WAKE_KEYS[i]))) return true;
    return false;
}

bool seb_wake_split(const char *text, char *cmd, size_t cap)
{
    bool found = false;
    size_t n = 0;
    if (!cap) return false;
    cmd[0] = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *st = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - st);
        if (!found && is_wake_word(st, len)) { found = true; continue; }
        if (n && n + 1 < cap) cmd[n++] = ' ';
        for (size_t i = 0; i < len && n + 1 < cap; i++) cmd[n++] = st[i];
    }
    cmd[n] = 0;
    // знаки по краям: «Себастьян, включи свет.» -> «включи свет»
    static const char *PUNCT = ",.!?-:; ";
    size_t a = 0;
    while (cmd[a] && strchr(PUNCT, cmd[a])) a++;
    size_t b = strlen(cmd);
    while (b > a && strchr(PUNCT, cmd[b - 1])) b--;
    memmove(cmd, cmd + a, b - a);
    cmd[b - a] = 0;
    int letters = 0;
    for (size_t i = 0; cmd[i]; i++)
        if (isalpha((unsigned char)cmd[i]) || (unsigned char)cmd[i] >= 0x80) letters++;
    if (letters < 2) cmd[0] = 0;
    return found;
}

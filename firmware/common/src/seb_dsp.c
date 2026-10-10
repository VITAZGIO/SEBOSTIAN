// Звуковые утилиты: кольцевой буфер, ресемплер, лимитер, детектор речи.
#include "seb_util.h"
#include <string.h>
#include <math.h>

// ---------------- кольцевой буфер ----------------
void seb_ring_init(seb_ring_t *r, int16_t *buf, uint32_t size)
{
    r->buf = buf;
    r->size = size;
    r->wr = r->rd = 0;
}

uint32_t seb_ring_fill(const seb_ring_t *r) { return r->wr - r->rd; }
uint32_t seb_ring_space(const seb_ring_t *r) { return r->size - (r->wr - r->rd); }

uint32_t seb_ring_write(seb_ring_t *r, const int16_t *x, uint32_t n)
{
    uint32_t sp = seb_ring_space(r);
    if (n > sp) n = sp;
    uint32_t w = r->wr % r->size;
    uint32_t a = r->size - w < n ? r->size - w : n;
    memcpy(r->buf + w, x, a * 2);
    if (n > a) memcpy(r->buf, x + a, (n - a) * 2);
    __sync_synchronize();
    r->wr += n;
    return n;
}

uint32_t seb_ring_read(seb_ring_t *r, int16_t *x, uint32_t n)
{
    uint32_t f = seb_ring_fill(r);
    if (n > f) n = f;
    uint32_t rp = r->rd % r->size;
    uint32_t a = r->size - rp < n ? r->size - rp : n;
    memcpy(x, r->buf + rp, a * 2);
    if (n > a) memcpy(x + a, r->buf, (n - a) * 2);
    __sync_synchronize();
    r->rd += n;
    return n;
}

uint32_t seb_ring_peek(const seb_ring_t *r, const int16_t **p)
{
    uint32_t f = seb_ring_fill(r);
    uint32_t rp = r->rd % r->size;
    uint32_t a = r->size - rp;
    *p = r->buf + rp;
    return f < a ? f : a;
}

void seb_ring_consume(seb_ring_t *r, uint32_t n)
{
    uint32_t f = seb_ring_fill(r);
    if (n > f) n = f;
    __sync_synchronize();
    r->rd += n;
}

void seb_ring_clear(seb_ring_t *r) { r->rd = r->wr; }

// ---------------- ресемплер ----------------
void seb_rs_init(seb_rs_t *rs, int ch, uint32_t in_rate, uint32_t out_rate)
{
    memset(rs, 0, sizeof(*rs));
    rs->ch = ch;
    rs->in_rate = in_rate;
    rs->out_rate = out_rate;
    rs->step = (double)in_rate / (double)out_rate;
    rs->frac = 3.0;          // сначала набрать 3 сэмпла истории
}

static inline float hermite(float xm1, float x0, float x1, float x2, float t)
{
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

int seb_rs_process(seb_rs_t *rs, const int16_t *in, int in_frames, int *in_used, float *out, int out_frames)
{
    int used = 0, made = 0;
    const int ch = rs->ch;
    while (made < out_frames) {
        while (rs->frac >= 1.0) {
            if (used >= in_frames) goto done;
            for (int c = 0; c < 2; c++) {
                rs->h[0][c] = rs->h[1][c];
                rs->h[1][c] = rs->h[2][c];
                rs->h[2][c] = rs->h[3][c];
            }
            rs->h[3][0] = in[used * ch];
            rs->h[3][1] = ch == 2 ? in[used * ch + 1] : rs->h[3][0];
            used++;
            rs->frac -= 1.0;
        }
        float t = (float)rs->frac;
        if (rs->step == 1.0) {               // одна частота — без интерполяции
            out[2 * made] = rs->h[1][0];
            out[2 * made + 1] = rs->h[1][1];
        } else {
            out[2 * made] = hermite(rs->h[0][0], rs->h[1][0], rs->h[2][0], rs->h[3][0], t);
            out[2 * made + 1] = hermite(rs->h[0][1], rs->h[1][1], rs->h[2][1], rs->h[3][1], t);
        }
        made++;
        rs->frac += rs->step;
    }
done:
    *in_used = used;
    return made;
}

// ---------------- лимитер ----------------
void seb_lim_init(seb_lim_t *l, float thr, int rate)
{
    l->thr = thr;
    l->env = 0;
    l->att = expf(-1.0f / (0.001f * rate));     // атака ~1 мс
    l->rel = expf(-1.0f / (0.150f * rate));     // отпускание ~150 мс
    l->gain = 1.0f;
}

void seb_lim_process(seb_lim_t *l, float *lr, int frames)
{
    for (int i = 0; i < frames; i++) {
        float a = fabsf(lr[2 * i]), b = fabsf(lr[2 * i + 1]);
        float p = a > b ? a : b;
        if (p > l->env) l->env = l->att * l->env + (1.0f - l->att) * p;
        else l->env = l->rel * l->env + (1.0f - l->rel) * p;
        float g = l->env > l->thr ? l->thr / l->env : 1.0f;
        l->gain = g;
        for (int c = 0; c < 2; c++) {
            float y = lr[2 * i + c] * g;
            float ay = fabsf(y);
            if (ay > 0.9f) {                       // мягкий потолок на пиках, которые атака не поймала
                float s = 0.9f + 0.099f * tanhf((ay - 0.9f) / 0.099f);
                y = y > 0 ? s : -s;
            }
            lr[2 * i + c] = y;
        }
    }
}

// ---------------- VAD ----------------
void seb_vad_default_cfg(seb_vad_cfg_t *c)
{
    c->vad_min = 40.0f;
    c->start_k = 3.0f;
    c->start_frames = 3;
    c->end_k = 2.0f;
    c->sil_ms = 700;
    c->min_speech_ms = 400;
}

void seb_vad_init(seb_vad_t *v, const seb_vad_cfg_t *cfg)
{
    memset(v, 0, sizeof(*v));
    v->cfg = *cfg;
    v->nf = cfg->vad_min;
}

float seb_vad_threshold(const seb_vad_t *v)
{
    float t = v->nf * v->cfg.start_k;
    return t > v->cfg.vad_min ? t : v->cfg.vad_min;
}

seb_vad_ev_t seb_vad_frame(seb_vad_t *v, const int16_t *x, int n, bool phrase_full)
{
    if (n <= 0) return SEB_VAD_NONE;
    double sq = 0;
    for (int i = 0; i < n; i++) {
        float s = x[i];
        v->hp = 0.954f * (v->hp + s - v->prev);
        v->prev = s;
        sq += (double)v->hp * v->hp;
    }
    int fms = n * 1000 / 16000;
    float rms = sqrtf((float)(sq / n));
    float nf = v->nf;
    v->lvl = rms;
    if (v->state == 0) {
        if (rms > seb_vad_threshold(v)) {
            if (++v->above >= v->cfg.start_frames) {
                v->state = 1;
                v->sil_ms = 0;
                v->sp_ms = v->above * fms;
                return SEB_VAD_START;
            }
        } else {
            v->above = 0;
            float nn = nf * 0.97f + rms * 0.03f;     // шум подстраивается только в тишине
            v->nf = nn > 5.0f ? nn : 5.0f;
        }
        return SEB_VAD_NONE;
    }
    v->sp_ms += fms;
    float endthr = nf * v->cfg.end_k;
    if (endthr < v->cfg.vad_min * 0.7f) endthr = v->cfg.vad_min * 0.7f;
    if (rms < endthr) v->sil_ms += fms;
    else v->sil_ms = 0;
    if (phrase_full) {                               // 8 с без паузы — это шум, поднимаем порог
        v->nf = (nf > rms ? nf : rms) * 1.3f;
        v->state = 0;
        v->above = 0;
        return SEB_VAD_OVERFLOW;
    }
    if (v->sil_ms >= v->cfg.sil_ms) {
        v->state = 0;
        v->above = 0;
        if (v->sp_ms - v->sil_ms >= v->cfg.min_speech_ms) {
            v->tail_ms = v->sil_ms;
            return SEB_VAD_DONE;
        }
        return SEB_VAD_SHORT;
    }
    return SEB_VAD_NONE;
}

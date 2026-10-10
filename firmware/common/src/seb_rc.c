// Радио 433: протокол 1 библиотеки RCSwitch (sui77/rc-switch 2.6.4), перенесён 1-в-1.
// Протокол 1: синхро {1,31}, ноль {1,3}, единица {3,1}, сигнал не инвертирован.
#include "seb_util.h"
#include <string.h>

#define SEP_LIMIT_US   4300     // пауза длиннее = промежуток между посылками
#define RX_TOLERANCE   60       // допуск, %

void seb_rc_rx_init(seb_rc_rx_t *rx) { memset(rx, 0, sizeof(*rx)); }

static inline uint32_t udiff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

static bool decode_p1(const seb_rc_rx_t *rx, unsigned change_count, seb_rc_code_t *out)
{
    const uint32_t delay = rx->timings[0] / 31;
    const uint32_t tol = delay * RX_TOLERANCE / 100;
    uint32_t code = 0;
    for (unsigned i = 1; i < change_count - 1; i += 2) {
        code <<= 1;
        if (udiff(rx->timings[i], delay * 1) < tol && udiff(rx->timings[i + 1], delay * 3) < tol) {
            // ноль
        } else if (udiff(rx->timings[i], delay * 3) < tol && udiff(rx->timings[i + 1], delay * 1) < tol) {
            code |= 1;
        } else {
            return false;
        }
    }
    if (change_count > 7) {      // слишком короткие посылки — шум
        out->code = code;
        out->bits = (int)(change_count - 1) / 2;
        out->delay_us = (int)delay;
        return true;
    }
    return false;
}

bool seb_rc_rx_edge(seb_rc_rx_t *rx, uint32_t duration, seb_rc_code_t *out)
{
    bool got = false;
    if (duration > SEP_LIMIT_US) {
        if (rx->repeat_count == 0 || udiff(duration, rx->timings[0]) < 200) {
            rx->repeat_count++;
            if (rx->repeat_count == 2) {
                got = decode_p1(rx, rx->change_count, out);
                rx->repeat_count = 0;
            }
        }
        rx->change_count = 0;
    }
    if (rx->change_count >= SEB_RC_MAX_CHANGES) {
        rx->change_count = 0;
        rx->repeat_count = 0;
    }
    rx->timings[rx->change_count++] = duration;
    return got;
}

int seb_rc_tx_encode(uint32_t code, int bits, int pulse, uint16_t hi[], uint16_t lo[], int max_pairs)
{
    if (bits + 1 > max_pairs) return 0;
    int k = 0;
    for (int i = bits - 1; i >= 0; i--) {
        bool one = (code >> i) & 1;
        hi[k] = (uint16_t)(pulse * (one ? 3 : 1));
        lo[k] = (uint16_t)(pulse * (one ? 1 : 3));
        k++;
    }
    hi[k] = (uint16_t)pulse;           // синхро после данных (как в RCSwitch)
    lo[k] = (uint16_t)(pulse * 31);
    return k + 1;
}

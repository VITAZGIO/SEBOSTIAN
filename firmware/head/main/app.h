// Центральная логика головы: команды от экрана, сенсоров корпуса и со-процессора.
// Быстрые команды выполняет задача app, долгие (запись, ответ, тесты, TRAIN) — задача голоса (worker).
#pragma once
#include <stdbool.h>

typedef enum {
    CMD_NONE = 0,
    CMD_PLAY, CMD_VOLM, CMD_VOLP,
    CMD_SPK_MUTE, CMD_AUX_MUTE, CMD_AUX_MUSIC,
    CMD_LR_TEST, CMD_MIC_TEST,
    CMD_LED, CMD_BEEP,
    CMD_CPOTA, CMD_REBOOT,
    CMD_ASK, CMD_WAKE, CMD_PING, CMD_AEC,
    CMD_KPOS, CMD_KNEG, CMD_KBG, CMD_KPLAY, CMD_KDEL, CMD_KTIP,
    CMD_OUT,            // arg = OUT_SPK/OUT_AUX/OUT_BOTH
    CMD_RF,             // arg = 1..21, 22 = HOLD
    CMD_HW_TOUCH,       // arg = сенсор 1..3
    CMD_PTT,            // нажали кнопку мика на со-процессоре
    CMD_GRACE,          // arg = мВ: включили на севшем аккуме
    CMD_BAT_DOWN,       // arg = мВ: аккум сел, спать
} app_cmd_t;

extern const char *RF_NAMES[21];

void app_start(void);
void app_post(app_cmd_t c, int arg);

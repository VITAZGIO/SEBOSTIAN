// UART-связь с со-процессором (протокол — firmware/common/include/seb_proto.h, таблица в README).
#pragma once
#include <stdbool.h>

void link_uart_setup(void);         // драйвер UART (можно звать рано — в boot gate)
void link_start(void);              // задача приёма строк
void link_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void link_send_cfg(void);           // Wi-Fi и пароль OTA новому IDF-со-процессору (для его OTA)
// boot gate: ждать признак жизни со-процессора ("S,..." или "HELLO") до ms миллисекунд
bool link_wait_alive(int ms);

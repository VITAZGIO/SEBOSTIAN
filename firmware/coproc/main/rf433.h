// Радио 433 МГц, совместимо с RCSwitch: протокол 1, импульс 389 мкс, 24 бита, повтор 8.
// Приём — прерывания по фронтам на GPIO27 + декодер RCSwitch (common/seb_rc.c).
// Передача — RMT на GPIO13. Свои передачи не ловим (приём выкл на время TX + 300 мс).
#pragma once
#include <stdbool.h>

void rf433_init(void);
void rf433_send_light(int n);      // код подсветки #n = 9348096 + n (n = 1..21)
void rf433_hold(void);             // HOLD = код #1 три раза подряд с паузой 100 мс
void rf433_rx_enable(bool on);

// Лог аккумулятора на SD (Arduino v20): раз в секунду строка time,mV,pct,mp3,bt,voice,vol,
// только при 100-70% и 30-0%; файл на каждый час /batlog/ГГГГММДД_ЧЧ.csv
// (без времени — notime_b<boot>_<час>.csv). Сброс на SD пачкой раз в 10 с и перед сном.
#pragma once

void batlog_tick(void);     // раз в секунду
void batlog_flush(void);

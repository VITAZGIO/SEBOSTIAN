// Веб-сервер головы (порт 80):
//   /            сводка и ссылки
//   /status      JSON для отладки (версия, память, задачи, звук, AEC, связь)
//   /setup       настройка Wi-Fi, пароля OTA, сервера, потолка громкости
//   /ota         POST .bin — прошивка по Wi-Fi (пароль OTA)
//   /log         последние ~32 КБ лога
//   /screenshot  снимок экрана (BMP)
//   /coredump    дамп последнего падения (для idf.py coredump-info)
//   /kws.tar /bat.tar /bat_clear?yes=1   сэмплы TRAIN и лог батареи с SD
#pragma once
#include <stdbool.h>

void web_start(void);
bool web_running(void);

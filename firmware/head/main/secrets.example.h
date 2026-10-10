// Необязательно. Скопируй в secrets.h (он в .gitignore), если собираешь сам и хочешь,
// чтобы плата сразу знала Wi-Fi. Без него Wi-Fi вводится на http://192.168.4.1/setup
// (точка доступа «Sebastian-setup», пароль sebastian123) и хранится в NVS.
#pragma once
#define WIFI_SSID  "ИМЯ_WIFI"
#define WIFI_PASS  "ПАРОЛЬ_WIFI"
#define OTA_PASS   "ПАРОЛЬ_OTA"
#define SEB_HOST   "192.168.1.201"
#define SEB_PORT   9010

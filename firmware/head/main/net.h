// Wi-Fi (STA с автопереподключением), точка доступа для настройки, часы (NTP), mDNS.
// Нет Wi-Fi в настройках или не подключились за 3 мин -> включается точка доступа
// «Sebastian-setup» (пароль sebastian123), настройка на http://192.168.4.1/setup
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define NET_HOSTNAME   "sebastian-head"
#define NET_AP_SSID    "Sebastian-setup"
#define NET_AP_PASS    "sebastian123"

void net_start(void);
bool net_connected(void);
bool net_ap_active(void);
void net_ip_str(char *out, size_t cap);
int net_rssi(void);
bool net_time_valid(void);
void net_stop(void);            // перед глубоким сном

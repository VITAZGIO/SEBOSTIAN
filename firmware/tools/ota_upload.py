#!/usr/bin/env python3
"""Прошивка «Себастьяна» по Wi-Fi (OTA): один скрипт для головы и со-процессора.

    python ota_upload.py <IP или имя> <файл.bin> --password ПАРОЛЬ_OTA

Примеры:
    python ota_upload.py sebastian-head.local ..\\bin\\sebastian_head.bin --password 1234
    python ota_upload.py 192.168.1.182 ..\\bin\\sebastian_coproc.bin --password 1234

Со-процессор принимает прошивку только в режиме OTA: на экране головы СИСТЕМА -> «OTA СОПРОЦ.»,
дождаться «OTA сопроц.: ЖДУ <IP>» (лента фиолетовая), 5 минут на заливку.
Только стандартная библиотека Python — ничего ставить не надо.
Windows спросит разрешение брандмауэра для Python — нажми «Разрешить».
"""
import argparse
import base64
import http.client
import os
import sys
import time


def main():
    ap = argparse.ArgumentParser(description="OTA-прошивка Себастьяна (голова/со-процессор)")
    ap.add_argument("host", help="IP или имя: sebastian-head.local / sebastian-coproc.local")
    ap.add_argument("bin", help="файл прошивки .bin (НЕ *_full.bin — тот только для кабеля)")
    ap.add_argument("--password", "-p", required=True, help="пароль OTA (задаётся на http://<голова>/setup)")
    ap.add_argument("--port", type=int, default=80)
    a = ap.parse_args()

    data = open(a.bin, "rb").read()
    if not data or data[0] != 0xE9:
        sys.exit("Это не прошивка приложения ESP (нет 0xE9 в начале). Для OTA нужен sebastian_*.bin без _full.")
    if "_full" in os.path.basename(a.bin):
        sys.exit("*_full.bin — образ для кабеля (с загрузчиком). По Wi-Fi шли sebastian_head.bin / sebastian_coproc.bin.")
    auth = base64.b64encode(("ota:" + a.password).encode()).decode()
    print(f"-> {a.host}: {os.path.basename(a.bin)}, {len(data) // 1024} КБ")
    # сначала проверить пароль пустым запросом (плата с неверным паролем сразу рвёт соединение)
    try:
        c0 = http.client.HTTPConnection(a.host, a.port, timeout=10)
        c0.request("POST", "/ota", body=b"", headers={"Authorization": "Basic " + auth, "Content-Length": "0"})
        if c0.getresponse().status == 401:
            sys.exit("Неверный пароль OTA (или он не задан — задай на http://<голова>/setup).")
        c0.close()
    except OSError as e:
        sys.exit(f"Нет связи с {a.host}: {e}\n(IP верный? Плата в сети? Для со-процессора — режим OTA включён?)")
    conn = http.client.HTTPConnection(a.host, a.port, timeout=60)
    t0 = time.time()
    try:
        conn.putrequest("POST", "/ota")
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(data)))
        conn.putheader("Authorization", "Basic " + auth)
        conn.endheaders()
        step = 16 * 1024
        for off in range(0, len(data), step):
            conn.send(data[off:off + step])
            pct = min(100, (off + step) * 100 // len(data))
            print(f"\r   {pct:3d}%", end="", flush=True)
        print()
        r = conn.getresponse()
        body = r.read().decode("utf-8", "replace").strip()
    except OSError as e:
        sys.exit(f"Нет связи с {a.host}: {e}\n(IP верный? Плата в сети? Для со-процессора — режим OTA включён?)")
    dt = time.time() - t0
    if r.status == 200:
        print(f"ГОТОВО за {dt:.0f} с: {body}")
        print("Плата перезагрузится. Новая прошивка ~30 с проверяет себя (Wi-Fi + веб) — если не поднимется, вернётся старая.")
    elif r.status == 401:
        sys.exit("Неверный пароль OTA (или он не задан — задай на http://<голова>/setup).")
    else:
        sys.exit(f"Ошибка HTTP {r.status}: {body}")


if __name__ == "__main__":
    main()

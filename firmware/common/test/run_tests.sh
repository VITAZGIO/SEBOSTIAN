#!/bin/sh
# Все тесты общих модулей на ПК (Linux/WSL). Нужны: cmake, gcc, python3, IDF_PATH (Unity из ESP-IDF).
set -e
cd "$(dirname "$0")"
cmake -S . -B build >/dev/null && cmake --build build >/dev/null
./build/test_common
python3 mock_server.py 18099 > /dev/null & SRV=$!
sleep 1
./build/test_hc 18099 || { kill $SRV; exit 1; }
kill $SRV

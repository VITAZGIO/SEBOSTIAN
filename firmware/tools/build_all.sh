#!/bin/sh
# Собрать обе прошивки + тестовую «битую» для проверки отката и разложить готовые файлы в firmware/bin.
# Нужен ESP-IDF v5.5.5 (. $IDF_PATH/export.sh). Linux/WSL/macOS.
set -e
F="$(cd "$(dirname "$0")/.." && pwd)"
B="$F/bin"
mkdir -p "$B"
cd "$F/head"
idf.py build
idf.py -B build_broken -DSEB_BROKEN_TEST=1 build
cd "$F/coproc"
idf.py build
cd "$F/head/build" && python -m esptool --chip esp32s3 merge-bin -o "$B/sebastian_head_full.bin" @flash_args
cp sebastian_head.bin "$B/"
cp "$F/head/build_broken/sebastian_head.bin" "$B/test_rollback_head.bin"
cd "$F/coproc/build" && python -m esptool --chip esp32 merge-bin -o "$B/sebastian_coproc_full.bin" @flash_args
cp sebastian_coproc.bin "$B/"
cd "$B" && sha256sum *.bin > SHA256SUMS.txt && cat SHA256SUMS.txt

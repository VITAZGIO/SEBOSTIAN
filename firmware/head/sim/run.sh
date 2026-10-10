#!/bin/sh
# Симулятор экрана головы на ПК (Linux/WSL): LVGL + наш ui.c + шрифты -> картинки всех страниц в sim/out/*.png
# Нужно: gcc, python3 с Pillow, исходники LVGL (появляются в managed_components после idf.py build).
set -e
cd "$(dirname "$0")"
LV=../managed_components/lvgl
mkdir -p out build
if [ ! -f build/liblvgl.a ]; then
  for f in $(find $LV/src -name '*.c'); do
    o=build/$(echo $f | md5sum | cut -c1-12).o
    gcc -O1 -c -DLV_CONF_INCLUDE_SIMPLE -I. -I$LV $f -o $o
  done
  ar rcs build/liblvgl.a build/*.o
fi
gcc -O1 -g -DLV_CONF_INCLUDE_SIMPLE -I. -Iinc -I../main -I../../common/include -I$LV \
  sim_main.c sim_stubs.c ../main/ui.c ../main/fonts/*.c ../../common/src/seb_media.c \
  build/liblvgl.a -lm -o build/sim
./build/sim
python3 -c "
import glob
from PIL import Image
for f in glob.glob('out/*.ppm'):
    Image.open(f).resize((480, 640), Image.NEAREST).save(f[:-4] + '.png')
"
rm -f out/*.ppm
ls out

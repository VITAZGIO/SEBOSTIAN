# components/ — локальные компоненты головы

- `sr_lite/` — минимум из ESP-SR: эхоподавление (AEC) и шумодав (NS). Готовые библиотеки `.a` из
  github.com/espressif/esp-sr (коммит в `sr_lite/ESP_SR_COMMIT`), лицензия ESPRESSIF MIT (только для чипов Espressif).
  Целиком esp-sr не подключаем: он тянет из реестра esp-dl/cjson/esp-dsp и модели на сотни МБ.
- `dl_fft/` — быстрое преобразование Фурье для AEC, из github.com/espressif/esp-dl (tools/dl_fft v0.8.0), MIT.

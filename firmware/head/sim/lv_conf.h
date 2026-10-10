// lv_conf для симулятора экрана на ПК (на железе LVGL настраивается через sdkconfig)
#pragma once
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_USE_SNAPSHOT 1
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_USE_LOG 0
#define LV_DEF_REFR_PERIOD 33
#define LV_DRAW_SW_SUPPORT_RGB565 1

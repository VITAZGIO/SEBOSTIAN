# Себастьян — со-процессор (ESP32 D0WD-V3, 4 МБ). ESP-IDF v5.5.5.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../common)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sebastian_coproc)

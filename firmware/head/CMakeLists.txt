# Себастьян — голова (ESP32-S3-N16R8). ESP-IDF v5.5.5.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../common)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sebastian_head)

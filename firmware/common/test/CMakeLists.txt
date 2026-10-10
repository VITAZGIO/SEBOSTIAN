# Юнит-тесты общих модулей на ПК (Linux/WSL/MSYS2): Unity берётся из ESP-IDF ($IDF_PATH).
#   cmake -S . -B build && cmake --build build && ./build/test_common
cmake_minimum_required(VERSION 3.16)
project(seb_common_test C)
if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "Нужен IDF_PATH (Unity берётся из ESP-IDF)")
endif()
set(UNITY $ENV{IDF_PATH}/components/unity/unity/src)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_executable(test_common test_common.c
    ${SRC}/seb_proto.c ${SRC}/seb_text.c ${SRC}/seb_media.c ${SRC}/seb_dsp.c ${SRC}/seb_rc.c
    ${UNITY}/unity.c)
target_include_directories(test_common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include ${UNITY})
target_compile_definitions(test_common PRIVATE UNITY_INCLUDE_DOUBLE)
target_compile_options(test_common PRIVATE -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -g)
target_link_options(test_common PRIVATE -fsanitize=address,undefined)
target_link_libraries(test_common m)

# HTTP-клиент против макета сервера Себастьяна: python3 mock_server.py 18099 & ./build/test_hc 18099
add_executable(test_hc test_hc.c
    ${SRC}/seb_hc.c ${SRC}/seb_text.c ${SRC}/seb_media.c
    ${UNITY}/unity.c)
target_include_directories(test_hc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include ${UNITY})
target_compile_options(test_hc PRIVATE -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -g)
target_link_options(test_hc PRIVATE -fsanitize=address,undefined)
target_link_libraries(test_hc m pthread)

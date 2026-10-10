# Минимум из ESP-SR (github.com/espressif/esp-sr, коммит в ESP_SR_COMMIT, лицензия ESPRESSIF MIT):
# только эхоподавление (AEC), шумодав (NS) и WebRTC VAD. Полный esp-sr тянет из реестра
# esp-dl/cjson/esp-dsp и модели на сотни МБ — нам это не нужно.
idf_component_register(INCLUDE_DIRS "include" REQUIRES dl_fft)
add_prebuilt_library(sr_audio_processor "${CMAKE_CURRENT_SOURCE_DIR}/lib/libesp_audio_processor.a" REQUIRES dl_fft)
add_prebuilt_library(sr_dl_lib "${CMAKE_CURRENT_SOURCE_DIR}/lib/libdl_lib.a")
add_prebuilt_library(sr_speech_features "${CMAKE_CURRENT_SOURCE_DIR}/lib/libc_speech_features.a")
idf_component_get_property(fft_lib dl_fft COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--start-group" sr_audio_processor sr_dl_lib sr_speech_features
    $<TARGET_FILE:${fft_lib}> "-Wl,--end-group")

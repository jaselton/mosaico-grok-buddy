set(FRONTEND "frontend/tensorflow/lite/experimental/microfrontend/lib")
idf_component_register(
    SRCS "app_wake.cc"
        "${FRONTEND}/kiss_fft_int16.cc" "${FRONTEND}/fft.cc" "${FRONTEND}/fft_util.cc"
        "${FRONTEND}/filterbank.c" "${FRONTEND}/filterbank_util.c"
        "${FRONTEND}/frontend.c" "${FRONTEND}/frontend_util.c"
        "${FRONTEND}/log_lut.c" "${FRONTEND}/log_scale.c" "${FRONTEND}/log_scale_util.c"
        "${FRONTEND}/noise_reduction.c" "${FRONTEND}/noise_reduction_util.c"
        "${FRONTEND}/pcan_gain_control.c" "${FRONTEND}/pcan_gain_control_util.c"
        "${FRONTEND}/window.c" "${FRONTEND}/window_util.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "frontend" "frontend/kissfft"
    REQUIRES esp_common
    PRIV_REQUIRES log espressif__esp-tflite-micro)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-variable -Wno-sign-compare)

# Optional model: projects/grok_desk_buddy/wakeword/hi_grok.tflite (git-ignored;
# trained locally, see wakeword/README.md). Missing -> empty placeholder, wake word off.
set(MODEL_SRC "${CMAKE_CURRENT_LIST_DIR}/../../wakeword/hi_grok.tflite")
set(MODEL_DST "${CMAKE_CURRENT_BINARY_DIR}/hi_grok.tflite")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../../wakeword")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../wakeword")
if(EXISTS "${MODEL_SRC}")
    configure_file("${MODEL_SRC}" "${MODEL_DST}" COPYONLY)
else()
    file(WRITE "${MODEL_DST}" "")
endif()
target_add_binary_data(${COMPONENT_LIB} "${MODEL_DST}" BINARY)

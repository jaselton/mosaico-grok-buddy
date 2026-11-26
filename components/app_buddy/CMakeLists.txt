idf_component_register(SRCS "app_buddy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp
    PRIV_REQUIRES log)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/main.json;ui/clock.json;ui/timer.json;ui/usage.json")

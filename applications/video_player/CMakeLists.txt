set(APPLICATION_NAME video_player)

add_executable(${APPLICATION_NAME}
    src/main.c
    src/utils/misc.c
    src/vdec/bitreader.c
    src/vdec/nalu.c
    src/vdec/mp4_demux.c
    src/vdec/h264_parser.c
    src/vdec/h264_dpb.c
    src/vdec/vdec_v4l2.c
    src/vdec/rotate_v4l2.c
    src/player/mediaplayer.c
    src/port/platform.c
    src/ui/ui.c
)

target_include_directories(${APPLICATION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBDRM_INCLUDE_DIRS}
)

# 移植的 driver/vdec 代码过不了 -Wpedantic，与上游保持 -Wall -Wextra
target_compile_options(${APPLICATION_NAME} PRIVATE
    -Wall -Wextra
    ${LIBDRM_CFLAGS_OTHER}
)

target_link_libraries(${APPLICATION_NAME} PRIVATE
    hal_display
    hal_input
    lvgl
    epass_fonts
    ${LIBDRM_LIBRARIES}
    pthread
    m
)

install(TARGETS ${APPLICATION_NAME}
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)
install(FILES appconfig.json icon.png
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)

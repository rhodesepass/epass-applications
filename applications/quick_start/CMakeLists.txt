set(APPLICATION_NAME quick_start)

# 纯逻辑部分独立成库, 供单测复用 (device-tree 解析 / VE 判定不碰硬件)
add_library(quick_start_core STATIC
    src/system/sysinfo.c
    src/system/storage.c
    src/system/ve_check.c
)
target_compile_features(quick_start_core PUBLIC c_std_11)
target_include_directories(quick_start_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/system)
target_compile_options(quick_start_core PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(quick_start_core PUBLIC pthread)

add_executable(${APPLICATION_NAME}
    src/main.c
    src/port/drm_warpper.c
    src/port/platform.c
    src/ui/ui.c
)
target_compile_features(${APPLICATION_NAME} PRIVATE c_std_11)
target_include_directories(${APPLICATION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${LIBDRM_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
)
target_compile_options(${APPLICATION_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
    ${LIBDRM_CFLAGS_OTHER}
    ${FREETYPE_CFLAGS_OTHER}
)
target_link_libraries(${APPLICATION_NAME} PRIVATE
    quick_start_core
    epass_input
    lvgl
    epass_fonts
    PNG::PNG
    ${LIBDRM_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    pthread
    m
)

# dev 工具: 离屏渲染各页到 PNG, 不依赖设备 (host 跑, 字体指向共享母本)
option(QUICK_START_PREVIEW "Build quick_start offscreen page preview tool" OFF)
if(QUICK_START_PREVIEW)
    # platform/drm 只为满足 ui.c 的符号引用而链入; preview 不调 platform_init,
    # 所以 has_overlay 恒为 0, overlay 相关调用全是空操作, 碰不到 /dev/dri
    add_executable(quick_start_preview
        tools/preview.c
        src/ui/ui.c
        src/port/platform.c
        src/port/drm_warpper.c
    )
    target_compile_features(quick_start_preview PRIVATE c_std_11)
    target_include_directories(quick_start_preview PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/port
        ${LIBDRM_INCLUDE_DIRS}
        ${FREETYPE_INCLUDE_DIRS}
    )
    target_link_libraries(quick_start_preview PRIVATE
        quick_start_core epass_input lvgl epass_fonts PNG::PNG
        ${LIBDRM_LIBRARIES} ${FREETYPE_LIBRARIES} pthread m)
endif()

if(BUILD_TESTING)
    add_executable(quick_start_tests tests/test_logic.c)
    target_compile_features(quick_start_tests PRIVATE c_std_11)
    target_compile_options(quick_start_tests PRIVATE
        -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(quick_start_tests PRIVATE quick_start_core)
    add_test(NAME quick_start_logic COMMAND quick_start_tests)
endif()

install(TARGETS ${APPLICATION_NAME}
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)
install(FILES appconfig.json icon.png
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)
install(DIRECTORY assets
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)

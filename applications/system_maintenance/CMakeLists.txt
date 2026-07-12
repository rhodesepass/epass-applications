set(APPLICATION_NAME system_maintenance)

add_library(system_maintenance_core STATIC
    src/system/boot_config.c
    src/system/boot_source.c
    src/system/fido_store.c
    src/system/overlay_registry.c
    src/system/sd_format.c
    src/system/system_info.c
)
target_compile_features(system_maintenance_core PUBLIC c_std_11)
target_include_directories(system_maintenance_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/system)
target_compile_options(system_maintenance_core PRIVATE
    -Wall -Wextra -Wpedantic)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/system
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
    system_maintenance_core
    lvgl
    epass_fonts
    PNG::PNG
    ${LIBDRM_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    pthread
    m
)

if(BUILD_TESTING)
    add_executable(system_maintenance_tests tests/test_system.c)
    target_compile_features(system_maintenance_tests PRIVATE c_std_11)
    target_compile_options(system_maintenance_tests PRIVATE
        -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(system_maintenance_tests PRIVATE
        system_maintenance_core)
    add_test(NAME system_maintenance_backend
        COMMAND system_maintenance_tests)
endif()

install(TARGETS ${APPLICATION_NAME}
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)
install(FILES appconfig.json
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)

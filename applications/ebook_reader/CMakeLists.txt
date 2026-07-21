set(APPLICATION_NAME ebook_reader)

add_executable(${APPLICATION_NAME}
    src/main.c
    src/port/platform.c
    src/reader/reader.c
    src/reader/gbk_table.c
    src/ui/ui.c
)

target_include_directories(${APPLICATION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port
    ${CMAKE_CURRENT_SOURCE_DIR}/src/reader
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
    hal_display
    hal_input
    lvgl
    epass_fonts
    PNG::PNG
    ${LIBDRM_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    pthread
    m
)

install(TARGETS ${APPLICATION_NAME}
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)
install(FILES appconfig.json icon.png
    DESTINATION ${APPLICATION_NAME}
    COMPONENT applications)

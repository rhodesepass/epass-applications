# HAL 中间件 —— epass 后端(sunxi DRM / evdev)。
# 收编原先散落在各应用 src/port 下的拷贝为单一实现。
add_library(hal_display STATIC
  src/hal_display.c
  src/hal_run.c
  src/spsc_queue.c
  src/log.c
)

target_compile_features(hal_display PUBLIC c_std_11)
target_include_directories(hal_display
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LIBDRM_INCLUDE_DIRS}
)
target_compile_options(hal_display PRIVATE ${LIBDRM_CFLAGS_OTHER})
target_link_libraries(hal_display PUBLIC ${LIBDRM_LIBRARIES} pthread)

# 4 键导航输入(evdev 扫描 + 键码翻译), 原 epass_input + 各 app 重复的读键循环
add_library(hal_input STATIC
  src/hal_input.c
)
target_compile_features(hal_input PUBLIC c_std_11)
target_include_directories(hal_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hal_input PRIVATE -Wall -Wextra -Wpedantic)

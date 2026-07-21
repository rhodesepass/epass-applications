# HAL 中间件 —— 显示/图层后端(epass=sunxi DRM)。
# 收编原先散落在各应用 src/port 下的 6 份 hal_display 拷贝为单一实现。
add_library(hal_display STATIC
  src/hal_display.c
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

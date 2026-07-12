cmake_minimum_required(VERSION 3.10)
project(epass_applications C)
include(CTest)

# ============================================================================
# 编译选项
# ============================================================================
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/dist CACHE PATH "Install prefix" FORCE)

# ============================================================================
# 依赖库查找
# ============================================================================
find_package(PNG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(FREETYPE REQUIRED freetype2)

# 系统共享字体 (buildroot 的 epass-fonts 包)。找到就用它导出的目录, 编译期把
# FONT_REGISTRY_DIR 定成该路径, 应用不再自带字体。找不到 (纯本地 dev 构建,
# 无 staging) 则仅打日志, 由各应用自行决定是否捆绑 fallback 字体。
pkg_check_modules(EPASS_FONTS QUIET epass-fonts)
add_library(epass_fonts INTERFACE)
if(EPASS_FONTS_FOUND)
  pkg_get_variable(EPASS_FONTS_DIR epass-fonts fontsdir)
  message(STATUS "epass-fonts found, shared font dir: ${EPASS_FONTS_DIR}")
  target_compile_definitions(epass_fonts INTERFACE
    FONT_REGISTRY_DIR="${EPASS_FONTS_DIR}"
  )
else()
  message(STATUS "epass-fonts not found (dev fallback: apps may bundle fonts)")
endif()

# ============================================================================
# LVGL 配置
# ============================================================================
# 设置为 CACHE 变量以覆盖 LVGL 的默认值
set(LV_BUILD_CONF_PATH ${CMAKE_SOURCE_DIR}/lv_conf.h CACHE PATH "lv_conf.h path" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "Build LVGL examples" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "Build LVGL demos" FORCE)
# lv_conf.h 里 LV_USE_THORVG_INTERNAL 已是 0；这里同步关掉 CMake 侧同名开关，
# 否则 ThorVG 的 .cpp 会被无条件编译，逼着交叉工具链必须带 C++ 前端。
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "Use internal ThorVG (needs CXX)" FORCE)
# lvgl/CMakeLists.txt 顶层无条件 enable_language(CXX ASM)，但工程里唯一用到
# C++ 的地方就是上面关掉的 ThorVG。为了不强依赖交叉工具链自带 C++ 前端
# (cc1plus)，把 CXX“编译器”指向 C 编译器本身，并跳过 CMake 的 try_compile
# 探测——反正不会有真的 .cpp 落到它头上。
set(CMAKE_CXX_COMPILER ${CMAKE_C_COMPILER} CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_FORCED TRUE)
add_subdirectory(lvgl)

# LVGL 自身的 os_desktop.cmake 把 src/ 下所有 .cpp（glTF viewer、TFT_eSPI、
# LovyanGFX、vg_lite_tvg 等可选桥接）无条件塞进主 lvgl 库，逼着上面那个假
# CXX 编译器去真编译，必炸。add_subdirectory 之后从 target 的 SOURCES 属性里
# 把 .cpp 全部摘掉。
get_target_property(LVGL_SRCS lvgl SOURCES)
list(FILTER LVGL_SRCS EXCLUDE REGEX "\\.cpp$")
set_target_properties(lvgl PROPERTIES SOURCES "${LVGL_SRCS}")
# LVGL 开启 LV_USE_FREETYPE 后，其适配源需要 freetype 头
target_include_directories(lvgl PRIVATE ${FREETYPE_INCLUDE_DIRS})

# ============================================================================
# 应用
# ============================================================================
# 各应用通过 target_link_libraries(... lvgl epass_fonts ${FREETYPE_LIBRARIES} ...)
# 接入共享库与字体路径宏。
add_subdirectory(applications)

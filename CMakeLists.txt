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
  # 角色 -> 路径是 epass-fonts 的对外契约 (roles.conf 单一真值源, 经 .pc 透传);
  # 应用应当用这些宏, 不要写死字体文件名 —— 换字体只动 roles.conf。
  foreach(role body title display icon)
    unset(EPASS_FONTS_ROLE_PATH)
    pkg_get_variable(EPASS_FONTS_ROLE_PATH epass-fonts font_${role})
    if(EPASS_FONTS_ROLE_PATH)
      string(TOUPPER ${role} EPASS_FONTS_ROLE_UPPER)
      target_compile_definitions(epass_fonts INTERFACE
        FONT_${EPASS_FONTS_ROLE_UPPER}_PATH="${EPASS_FONTS_ROLE_PATH}"
      )
    endif()
  endforeach()
else()
  message(STATUS "epass-fonts not found (dev fallback: apps may bundle fonts)")
endif()

# ============================================================================
# LVGL —— 优先用系统共享库, 否则下载 release 归档就地静态编 (无 submodule)
# ============================================================================
# 设备/buildroot 构建: staging 里有 pkg-config 的 lvgl (共享库 + 权威 lv_conf.h)。
# 找到就建一个同名 INTERFACE 目标 lvgl, 下游各应用的 target_link_libraries(... lvgl)
# 一行不用改 —— 等价于把静态 .a 换成 -llvgl, 符号名不变。
# 找不到 (纯本地 PC 开发, 无 sysroot) 则退回 FetchContent 下载 v9.5.0 release 归档
# 就地静态编, 用本仓库自带 lv_conf.h 作配置源。
find_package(PkgConfig REQUIRED)
pkg_check_modules(LVGL IMPORTED_TARGET lvgl)
if(LVGL_FOUND)
  message(STATUS "LVGL: using system shared lib ${LVGL_VERSION} (${LVGL_LIBDIR})")
  # 同名 INTERFACE 目标 lvgl 转发系统库 (include/-L/-llvgl 都由 PkgConfig::LVGL 带);
  # 不能直接 target_link_libraries(lvgl INTERFACE ${LVGL_LIBRARIES}), 因为它值就是
  # 字符串 "lvgl" —— 会被当成对本目标自身的引用, -llvgl 反而丢掉。
  add_library(lvgl INTERFACE)
  target_link_libraries(lvgl INTERFACE PkgConfig::LVGL)
else()
  message(STATUS "LVGL: system lib not found, fetching v9.5.0 release archive (static)")
  include(FetchContent)
  set(LV_BUILD_CONF_PATH ${CMAKE_SOURCE_DIR}/lv_conf.h CACHE PATH "lv_conf.h path" FORCE)
  set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "Build LVGL examples" FORCE)
  set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "Build LVGL demos" FORCE)
  set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "Use internal ThorVG (needs CXX)" FORCE)
  # 独立工具链可能没 C++ 前端: 把 CXX 指向 C 编译器并跳过探测 (反正没真 .cpp 要编)
  set(CMAKE_CXX_COMPILER ${CMAKE_C_COMPILER} CACHE FILEPATH "" FORCE)
  set(CMAKE_CXX_COMPILER_FORCED TRUE)
  # 解到名为 lvgl 的目录 (而非默认 _deps/lvgl-src), 好让下面把父目录加进 include 后
  # 工程里的 #include <lvgl/lvgl.h> 照旧能解析。
  FetchContent_Declare(lvgl
    URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.5.0.tar.gz
    SOURCE_DIR ${CMAKE_BINARY_DIR}/_lvgl/lvgl)
  FetchContent_MakeAvailable(lvgl)
  get_target_property(LVGL_SRCS lvgl SOURCES)
  list(FILTER LVGL_SRCS EXCLUDE REGEX "\\.cpp$")
  set_target_properties(lvgl PROPERTIES SOURCES "${LVGL_SRCS}")
  target_include_directories(lvgl PRIVATE ${FREETYPE_INCLUDE_DIRS})
  # LVGL 自身把源码根导出为 include (-> <lvgl.h>); 再把源码根的父目录加进来,
  # 让 <lvgl/lvgl.h> 这种带前缀的写法也能解析 (设备侧靠 buildroot 全局 -I/usr/include)。
  target_include_directories(lvgl INTERFACE ${CMAKE_BINARY_DIR}/_lvgl)
endif()

# ============================================================================
# 应用
# ============================================================================
# 各应用通过 target_link_libraries(... lvgl epass_fonts ${FREETYPE_LIBRARIES} ...)
# 接入共享库与字体路径宏。
add_subdirectory(applications)

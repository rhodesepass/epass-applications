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
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED libdrm)

# ============================================================================
# 应用
# ============================================================================
add_subdirectory(applications)

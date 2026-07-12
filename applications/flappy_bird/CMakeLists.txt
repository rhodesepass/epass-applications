cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(flappy_bird C)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBDRM REQUIRED libdrm)
  add_subdirectory(../common/epass_game
                   ${CMAKE_CURRENT_BINARY_DIR}/epass_game)
endif()

add_executable(flappy_bird main.c)
target_compile_features(flappy_bird PRIVATE c_std_11)
target_link_libraries(flappy_bird PRIVATE epass_game)

install(TARGETS flappy_bird
        DESTINATION flappy_bird
        COMPONENT applications)
install(FILES appconfig.json icon.png
        DESTINATION flappy_bird
        COMPONENT applications)

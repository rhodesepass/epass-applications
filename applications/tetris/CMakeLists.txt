cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(epass_tetris C)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../dist"
            CACHE PATH "Install prefix" FORCE)
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDRM REQUIRED libdrm)
endif()

if(NOT TARGET epass_game)
    add_subdirectory(../common/epass_game
                     ${CMAKE_CURRENT_BINARY_DIR}/epass_game)
endif()

add_library(tetris_logic STATIC src/logic.c)
target_compile_features(tetris_logic PUBLIC c_std_11)
target_include_directories(tetris_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(tetris_logic PRIVATE -Wall -Wextra -Wpedantic)

add_executable(tetris src/main.c)
target_compile_features(tetris PRIVATE c_std_11)
target_compile_options(tetris PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tetris PRIVATE tetris_logic epass_game)

add_executable(tetris_logic_tests tests/test_logic.c)
target_compile_features(tetris_logic_tests PRIVATE c_std_11)
target_compile_options(tetris_logic_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tetris_logic_tests PRIVATE tetris_logic)

enable_testing()
add_test(NAME tetris_rotation_and_line_clear COMMAND tetris_logic_tests)

install(TARGETS tetris
        DESTINATION tetris
        COMPONENT applications)
install(FILES appconfig.json
        DESTINATION tetris
        COMPONENT applications)

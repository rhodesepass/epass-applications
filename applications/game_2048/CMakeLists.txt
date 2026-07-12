cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(game_2048 C)
    option(GAME_2048_LOGIC_ONLY
        "Build host-side logic and tests without DRM" OFF)
    if(NOT GAME_2048_LOGIC_ONLY)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBDRM REQUIRED libdrm)
        add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/epass_game
            ${CMAKE_CURRENT_BINARY_DIR}/epass_game)
    endif()
endif()

set(APPLICATION_NAME game_2048)

add_library(game_2048_logic STATIC src/logic.c)
target_compile_features(game_2048_logic PUBLIC c_std_11)
target_include_directories(game_2048_logic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(game_2048_logic PRIVATE -Wall -Wextra -Wpedantic)

add_executable(game_2048_logic_tests tests/test_logic.c)
target_compile_features(game_2048_logic_tests PRIVATE c_std_11)
target_compile_options(game_2048_logic_tests PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(game_2048_logic_tests PRIVATE game_2048_logic)

enable_testing()
add_test(NAME game_2048_merge_rules COMMAND game_2048_logic_tests)

if(NOT GAME_2048_LOGIC_ONLY)
    add_executable(${APPLICATION_NAME} src/main.c)
    target_compile_features(${APPLICATION_NAME} PRIVATE c_std_11)
    target_compile_options(${APPLICATION_NAME} PRIVATE
        -Wall -Wextra -Wpedantic)
    target_link_libraries(${APPLICATION_NAME} PRIVATE
        game_2048_logic
        epass_game)

    install(TARGETS ${APPLICATION_NAME}
        DESTINATION ${APPLICATION_NAME}
        COMPONENT applications)
    install(FILES appconfig.json
        DESTINATION ${APPLICATION_NAME}
        COMPONENT applications)
endif()

cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(epniccc C)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBDRM REQUIRED libdrm)
  add_subdirectory(../common/epass_game
                   ${CMAKE_CURRENT_BINARY_DIR}/epass_game)
  include(CTest)
endif()

add_executable(epniccc main.c niccc.c)
target_compile_features(epniccc PRIVATE c_std_11)
target_include_directories(epniccc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(epniccc PRIVATE epass_game)

install(TARGETS epniccc
        DESTINATION epniccc
        COMPONENT applications)
install(FILES appconfig.json icon.png scene1.bin
        DESTINATION epniccc
        COMPONENT applications)

if(BUILD_TESTING)
  add_executable(epniccc_renderer_test
    tests/test_renderer.c
    niccc.c
  )
  target_compile_features(epniccc_renderer_test PRIVATE c_std_11)
  target_compile_options(epniccc_renderer_test PRIVATE
    -Wall -Wextra -Wpedantic)
  target_include_directories(epniccc_renderer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(epniccc_renderer_test PRIVATE
    NICCC_TEST_SCENE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/scene1.bin")
  target_link_libraries(epniccc_renderer_test PRIVATE epass_game)
  add_test(NAME epniccc_renderer COMMAND epniccc_renderer_test)
endif()

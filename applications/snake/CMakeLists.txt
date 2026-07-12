cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(snake C)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBDRM REQUIRED libdrm)
  add_subdirectory(../common common)
  include(CTest)
endif()

add_executable(snake
  src/main.c
  src/logic.c
)
target_compile_features(snake PRIVATE c_std_11)
target_compile_options(snake PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(snake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(snake PRIVATE epass_game)

install(TARGETS snake
  DESTINATION snake
  COMPONENT applications)
install(FILES appconfig.json
  DESTINATION snake
  COMPONENT applications)

if(BUILD_TESTING)
  add_executable(snake_logic_test
    tests/test_logic.c
    src/logic.c
  )
  target_compile_features(snake_logic_test PRIVATE c_std_11)
  target_compile_options(snake_logic_test PRIVATE -Wall -Wextra -Wpedantic)
  target_include_directories(snake_logic_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME snake_logic COMMAND snake_logic_test)
endif()

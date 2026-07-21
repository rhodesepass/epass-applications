add_library(epass_game STATIC
  src/epass_game.c
  src/log.c
)

target_compile_features(epass_game PUBLIC c_std_11)
target_include_directories(epass_game
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LIBDRM_INCLUDE_DIRS}
)
target_compile_options(epass_game PRIVATE ${LIBDRM_CFLAGS_OTHER})
target_link_libraries(epass_game PUBLIC hal_display ${LIBDRM_LIBRARIES} pthread m PRIVATE epass_input)

if(BUILD_TESTING)
  add_executable(epass_game_draw_test tests/test_draw.c)
  target_compile_features(epass_game_draw_test PRIVATE c_std_11)
  target_compile_options(epass_game_draw_test PRIVATE
    -Wall -Wextra -Wpedantic)
  target_link_libraries(epass_game_draw_test PRIVATE epass_game)
  add_test(NAME epass_game_draw COMMAND epass_game_draw_test)
endif()

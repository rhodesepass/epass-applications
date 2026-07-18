add_library(epass_input STATIC
  src/epass_input.c
)

target_compile_features(epass_input PUBLIC c_std_11)
target_include_directories(epass_input
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(epass_input PRIVATE -Wall -Wextra -Wpedantic)

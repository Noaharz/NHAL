cmake_minimum_required(VERSION 3.21)
project(nhal_example_game_input LANGUAGES C)

set(NHAL_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/nhal.toml" CACHE FILEPATH "" FORCE)
add_subdirectory(../.. nhal)

add_executable(game_input main.c)
target_link_libraries(game_input PRIVATE nhal::nhal)

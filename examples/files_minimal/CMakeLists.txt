cmake_minimum_required(VERSION 3.21)
project(nhal_example_files_minimal LANGUAGES C)

set(NHAL_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/nhal.toml" CACHE FILEPATH "" FORCE)
add_subdirectory(../.. nhal)

add_executable(files_minimal main.c)
target_link_libraries(files_minimal PRIVATE nhal::nhal)

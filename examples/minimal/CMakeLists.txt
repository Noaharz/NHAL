cmake_minimum_required(VERSION 3.21)
project(nhal_example_minimal LANGUAGES C)

# So bindet eine App NHAL ein: Manifest setzen, dann NHAL als Unterprojekt.
set(NHAL_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/nhal.toml" CACHE FILEPATH "" FORCE)
add_subdirectory(../.. nhal)

add_executable(minimal main.c)
target_link_libraries(minimal PRIVATE nhal::nhal)

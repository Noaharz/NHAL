cmake_minimum_required(VERSION 3.21)

project(nhal VERSION 0.1.0 DESCRIPTION "Modulares Hardware Abstraction Layer" LANGUAGES C)

# ── Optionen ─────────────────────────────────────────────────────────────────
# Normalerweise ruft man tools/nhal.py (./nhal) auf, das diese Variablen
# setzt. Direkt mit CMake geht es auch:
# NHAL_MANIFEST: nhal.toml der App. Leer = alle auf der Plattform verfügbaren
# Features als optional (für Tests und zum Ausprobieren).
# NHAL_PLATFORM: macos, windows, linux, xernel, web (leer = Host bzw. Emscripten).
set(NHAL_MANIFEST "" CACHE FILEPATH "App-Manifest (nhal.toml)")
set(NHAL_PLATFORM "" CACHE STRING "Zielplattform aus catalog/platforms.toml (leer = automatisch)")
option(NHAL_BUILD_TESTS "Tests bauen" ${PROJECT_IS_TOP_LEVEL})
option(NHAL_WERROR "Warnungen als Fehler" OFF)

set(NHAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NHAL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/nhal_generated")

# ── Plattform bestimmen ──────────────────────────────────────────────────────
if(NHAL_PLATFORM STREQUAL "")
    if(EMSCRIPTEN)
        set(NHAL_PLATFORM "web")
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(NHAL_PLATFORM "macos")
    elseif(WIN32)
        set(NHAL_PLATFORM "windows")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(NHAL_PLATFORM "linux")
    else()
        message(FATAL_ERROR "NHAL: Plattform ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} "
                            "wird nicht erkannt. NHAL_PLATFORM setzen.")
    endif()
endif()

# ── Generator ────────────────────────────────────────────────────────────────
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(_nhal_gen_args generate --platform "${NHAL_PLATFORM}" --out "${NHAL_GEN_DIR}")
if(NOT NHAL_MANIFEST STREQUAL "")
    get_filename_component(_nhal_manifest "${NHAL_MANIFEST}" ABSOLUTE)
    list(APPEND _nhal_gen_args --manifest "${_nhal_manifest}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nhal_manifest}")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${NHAL_ROOT}/tools/nhalgen/nhalgen.py" ${_nhal_gen_args}
    RESULT_VARIABLE _nhal_gen_result
    OUTPUT_VARIABLE _nhal_gen_output
    ERROR_VARIABLE _nhal_gen_error
)
if(NOT _nhal_gen_result EQUAL 0)
    message(FATAL_ERROR "NHAL: nhalgen ist fehlgeschlagen:\n${_nhal_gen_error}")
endif()
string(STRIP "${_nhal_gen_output}" _nhal_gen_output)
message(STATUS "${_nhal_gen_output}")

include("${NHAL_GEN_DIR}/nhal_sources.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${NHAL_CATALOG_FILES} "${NHAL_ROOT}/tools/nhalgen/nhalgen.py")

# Backends in Objective-C (.m, macOS-Frameworks) – nur wenn gewählt, mit ARC.
set(_nhal_objc_sources ${NHAL_FEATURE_SOURCES})
list(FILTER _nhal_objc_sources INCLUDE REGEX "\\.m$")
if(_nhal_objc_sources)
    enable_language(OBJC)
    set_source_files_properties(${_nhal_objc_sources} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# ── Bibliothek ───────────────────────────────────────────────────────────────
add_library(nhal STATIC
    core/src/core.c
    "${NHAL_GEN_DIR}/src/nhal_registry.c"
    ${NHAL_FEATURE_SOURCES}
)
add_library(nhal::nhal ALIAS nhal)

target_sources(nhal PRIVATE ${NHAL_ALLOCATOR_SOURCES})

target_include_directories(nhal PUBLIC
    "$<BUILD_INTERFACE:${NHAL_ROOT}/include>"
    "$<BUILD_INTERFACE:${NHAL_GEN_DIR}/include>"
)
# Interne Hilfen der Backends (Threads, Heap) – nicht Teil der öffentlichen API.
target_include_directories(nhal PRIVATE "${NHAL_ROOT}/backends")

# pthreads für Features mit Worker-Threads (Windows nutzt Win32-Threads,
# Web und Xernel haben keine).
if(NHAL_PLATFORM STREQUAL "macos" OR NHAL_PLATFORM STREQUAL "linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(nhal PUBLIC Threads::Threads)
endif()
set_target_properties(nhal PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(nhal PUBLIC ${NHAL_FEATURE_LINK_LIBS})
# JS-Bibliotheken der Web-Backends (größerer JS-Code als in EM_JS sinnvoll).
foreach(_nhal_js IN LISTS NHAL_FEATURE_JS_LIBRARIES)
    target_link_options(nhal INTERFACE "--js-library=${_nhal_js}")
    set_property(TARGET nhal APPEND PROPERTY INTERFACE_LINK_DEPENDS "${_nhal_js}")
endforeach()

if(MSVC)
    target_compile_options(nhal PRIVATE /W4 $<$<BOOL:${NHAL_WERROR}>:/WX>)
else()
    target_compile_options(nhal PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
                                        $<$<BOOL:${NHAL_WERROR}>:-Werror>)
endif()

if(NHAL_PLATFORM_FREESTANDING)
    target_compile_options(nhal PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-ffreestanding -fno-builtin>)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(NHAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

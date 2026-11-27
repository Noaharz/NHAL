# CMake-Toolchain für Xernel (x86_64, Userland, mit xlibc).
#
# Übernimmt die Regeln des Xernel-Userland-Targets: kein SSE/MMX (nur
# Ganzzahl-Register), keine Red Zone, statisch, nicht positionsunabhängig.
# Programme werden mit rust-lld, dem Linkerskript des Xernel-Userlands und
# libxlibc.a zu ELF-Dateien gelinkt, die Xernel direkt starten kann.
#
# Gebraucht werden:
#   XERNEL_ROOT    Wurzel des Xernel-Workspace (enthält libs/xlibc), als
#                  CMake-Variable oder Umgebungsvariable
#   XERNEL_PROFILE debug oder release (Standard: debug) – welches libxlibc.a
#   clang, llvm-ar, rust-lld (llvm-ar und rust-lld kommen aus der Rust-Toolchain)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ── Xernel-Workspace ─────────────────────────────────────────────────────────
if(NOT XERNEL_ROOT AND DEFINED ENV{XERNEL_ROOT})
    set(XERNEL_ROOT "$ENV{XERNEL_ROOT}")
endif()
if(NOT XERNEL_ROOT OR NOT EXISTS "${XERNEL_ROOT}/libs/xlibc/include/stdlib.h")
    message(FATAL_ERROR
        "Xernel: XERNEL_ROOT zeigt nicht auf den Xernel-Workspace (libs/xlibc/include fehlt).\n"
        "Setzen mit: export XERNEL_ROOT=<pfad>/Xernel/Xernel")
endif()
set(XERNEL_ROOT "${XERNEL_ROOT}" CACHE PATH "Xernel-Workspace" FORCE)
if(NOT XERNEL_PROFILE)
    set(XERNEL_PROFILE "$ENV{XERNEL_PROFILE}")
endif()
if(NOT XERNEL_PROFILE)
    set(XERNEL_PROFILE debug)
endif()
set(XERNEL_LIBC "${XERNEL_ROOT}/target/x86_64-xernel-user/${XERNEL_PROFILE}/libxlibc.a")
set(XERNEL_LINKER_SCRIPT "${XERNEL_ROOT}/userland/init/linker.ld")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XERNEL_ROOT XERNEL_PROFILE)

# ── Werkzeuge ────────────────────────────────────────────────────────────────
file(GLOB _xernel_rust_bins "$ENV{HOME}/.rustup/toolchains/*/lib/rustlib/*/bin")
find_program(XERNEL_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 clang-16 REQUIRED)
find_program(XERNEL_AR NAMES llvm-ar llvm-ar-20 llvm-ar-19 llvm-ar-18 llvm-ar-17 llvm-ar-16
             HINTS ${_xernel_rust_bins} REQUIRED)
find_program(XERNEL_LLD NAMES rust-lld HINTS ${_xernel_rust_bins} REQUIRED)

set(CMAKE_C_COMPILER "${XERNEL_CLANG}")
set(CMAKE_C_COMPILER_TARGET x86_64-unknown-none-elf)
set(CMAKE_C_FLAGS_INIT
    "-ffreestanding -fno-builtin -nostdlib -mno-red-zone -mgeneral-regs-only -fno-pic -fno-stack-protector \
-isystem ${XERNEL_ROOT}/libs/xlibc/include")

set(CMAKE_AR "${XERNEL_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> qcs <TARGET> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> qs <TARGET> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "")

# Programme: statisch, Linkerskript des Xernel-Userlands, xlibc (mit crt0) zuletzt.
set(CMAKE_C_LINK_EXECUTABLE
    "${XERNEL_LLD} -flavor gnu -static -nostdlib --gc-sections -T ${XERNEL_LINKER_SCRIPT} \
<OBJECTS> <LINK_LIBRARIES> ${XERNEL_LIBC} -o <TARGET>")
# Der Compiler-Test von CMake linkt kein Programm (crt0 und xlibc fehlen dort noch).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

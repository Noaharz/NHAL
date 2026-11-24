# Kern-Test (braucht libc für die Ausgabe, daher nicht im Freestanding-Build).
if(NOT NHAL_PLATFORM_FREESTANDING)
    add_executable(nhal_core_test core/core_test.c)
    target_link_libraries(nhal_core_test PRIVATE nhal::nhal)
    set_target_properties(nhal_core_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    add_test(NAME core COMMAND nhal_core_test)
endif()

add_test(NAME nhalgen_unit
         COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${NHAL_ROOT}/tests/tools" -p "test_*.py")
add_test(NAME catalog_check
         COMMAND "${Python3_EXECUTABLE}" "${NHAL_ROOT}/tools/nhalgen/nhalgen.py" check)
add_test(NAME platform_matrix_current
         COMMAND "${Python3_EXECUTABLE}" "${NHAL_ROOT}/tools/nhalgen/nhalgen.py" matrix --check)
add_test(NAME guard
         COMMAND "${Python3_EXECUTABLE}" "${NHAL_ROOT}/tools/guard/guard.py" --root "${NHAL_ROOT}")

cmake_minimum_required(VERSION 3.21)
project(ReResorep LANGUAGES CXX ASM_MASM RC VERSION 2.5.3)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ReResorep builds for x64 only (configure with -A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: neither the proxy nor the manager needs a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

set(RERESOREP_BIN ${CMAKE_BINARY_DIR}/bin)
# ReResorep.exe and the proxy link here; their sealed copies go to bin (see reresorep_seal).
set(RERESOREP_UNSEALED ${CMAKE_BINARY_DIR}/unsealed)

# Single source of the version: project(... VERSION x.y.z) above.
# The proxy has its own version. Bump it only when the proxy itself changes (proxy/ or the core/
# code it links): mods that whitelist d3d11.dll by its hash need the new hash every time it does.
# A release that only changes the manager keeps it, and ships the released proxy file
# (packaging/proxy.sha256).
set(RERESOREP_PROXY_VERSION 2.5.0)
string(REPLACE "." ";" RERESOREP_PROXY_VERSION_PARTS ${RERESOREP_PROXY_VERSION})
list(GET RERESOREP_PROXY_VERSION_PARTS 0 RERESOREP_PROXY_VERSION_MAJOR)
list(GET RERESOREP_PROXY_VERSION_PARTS 1 RERESOREP_PROXY_VERSION_MINOR)
list(GET RERESOREP_PROXY_VERSION_PARTS 2 RERESOREP_PROXY_VERSION_PATCH)
# Reproducible builds: the same sources always link to the same bytes (no timestamps), so a
# rebuild of an unchanged proxy keeps its hash.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Brepro>)
add_link_options(/Brepro)
set(RERESOREP_GENERATED ${CMAKE_BINARY_DIR}/generated)
configure_file(core/version.h.in ${RERESOREP_GENERATED}/version.h @ONLY)

add_library(resorep_core STATIC
    core/blowfish.cpp
    core/dds.cpp
    core/integrity.cpp
    core/labels.cpp
    core/mod_package.cpp
    core/mod_state.cpp
    core/resorep_crypto.cpp
    core/settings.cpp
    core/texture_index.cpp
    core/zip.cpp
)
target_include_directories(resorep_core PUBLIC core ${RERESOREP_GENERATED})
target_link_libraries(resorep_core PUBLIC bcrypt)

# Seals ReResorep.exe and the proxy after each link, so they can tell if their file was changed
# (core/integrity.h). The linker's own output is left alone, or the build would see it as
# changed and relink it every time; the sealed copy goes to `sealed`. A target that can't be
# sealed fails its build.
add_executable(reresorep_seal tools/seal.cpp)
target_link_libraries(reresorep_seal PRIVATE resorep_core)
function(reresorep_seal target sealed)
    add_dependencies(${target} reresorep_seal)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND reresorep_seal $<TARGET_FILE:${target}> ${sealed}
        COMMENT "Sealing ${sealed}"
        VERBATIM)
endfunction()

add_subdirectory(proxy)
add_subdirectory(manager)

# build/testing: an everyday copy of the app outside the compiler's output folder, so its
# data survives clean rebuilds. Every build refreshes its exe and proxy (part of ALL).
add_custom_target(testing ALL
    COMMAND ${CMAKE_COMMAND} -DBIN=${RERESOREP_BIN} -DDIR=${CMAKE_BINARY_DIR}/testing
            -P ${PROJECT_SOURCE_DIR}/packaging/testing.cmake
    DEPENDS ReResorep proxy
    COMMENT "Updating build/testing"
    VERBATIM)

# Release folder + zip in build/ship: cmake --build build --config Release --target ship
add_custom_target(ship
    COMMAND ${CMAKE_COMMAND} -DBIN=${RERESOREP_BIN} -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/ship
            -DVERSION=${PROJECT_VERSION} -DPROXY_VERSION=${RERESOREP_PROXY_VERSION} -DCONFIG=$<CONFIG>
            -P ${PROJECT_SOURCE_DIR}/packaging/ship.cmake
    DEPENDS ReResorep proxy
    COMMENT "Packaging ReResorep ${PROJECT_VERSION}"
    VERBATIM)

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE resorep_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(proxy_test tests/proxy_test.cpp)
target_link_libraries(proxy_test PRIVATE resorep_core)
add_dependencies(proxy_test proxy)
add_test(NAME proxy_test COMMAND proxy_test ${RERESOREP_BIN}/proxy/d3d11.dll)
add_test(NAME proxy_tamper_test COMMAND proxy_test ${RERESOREP_BIN}/proxy/d3d11.dll tampered)

add_executable(manager_test tests/manager_test.cpp manager/library.cpp manager/games.cpp manager/win_util.cpp
    manager/theme.cpp)
target_include_directories(manager_test PRIVATE manager ${RERESOREP_GENERATED})
target_link_libraries(manager_test PRIVATE resorep_core imgui shell32 ole32 version bcrypt)
add_dependencies(manager_test proxy)
add_test(NAME manager_test COMMAND manager_test ${RERESOREP_BIN}/proxy/d3d11.dll)

add_executable(analyzer_test tests/analyzer_test.cpp manager/analyzer.cpp)
target_include_directories(analyzer_test PRIVATE manager)
target_link_libraries(analyzer_test PRIVATE resorep_core d3d11 d3dcompiler)
add_test(NAME analyzer_test COMMAND analyzer_test)

add_executable(tint_test tests/tint_test.cpp manager/tint.cpp manager/bc_encode.cpp manager/png_export.cpp
               manager/analyzer.cpp)
target_include_directories(tint_test PRIVATE manager)
target_link_libraries(tint_test PRIVATE resorep_core d3d11 d3dcompiler windowscodecs ole32)
add_test(NAME tint_test COMMAND tint_test)

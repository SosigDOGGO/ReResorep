# Assembles the release folder and zip. Run through the "ship" target:
#   cmake --build build --config Release --target ship
# Only the files listed here are copied, so a developer's own settings, caches,
# dumps or mods next to the built exe can never end up in a release.
foreach(var BIN SRC OUT VERSION PROXY_VERSION CONFIG)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "ship.cmake: ${var} is not set")
    endif()
endforeach()
if(NOT CONFIG STREQUAL "Release")
    message(FATAL_ERROR "Ship a Release build: cmake --build build --config Release --target ship")
endif()

set(NAME "ReResorep-${VERSION}")
# Build beside the current release so its pinned proxy can be reused on repeated ship runs.
set(STAGE "${OUT}/.staging")
set(DIR "${STAGE}/${NAME}")
set(ZIP "${OUT}/${NAME}-win64.zip")

# The proxy: mods whitelist d3d11.dll by its hash, so while the proxy version stays the one in
# packaging/proxy.sha256 ("<version> <sha256>" of the released proxy), that exact file ships,
# taken from an earlier package in OUT when this build's differs.
set(PROXY "${BIN}/proxy/d3d11.dll")
file(SHA256 "${PROXY}" BUILT_PROXY_HASH)
file(STRINGS "${SRC}/packaging/proxy.sha256" PIN LIMIT_COUNT 1)
string(STRIP "${PIN}" PIN)  # a CRLF checkout leaves a \r
string(REGEX MATCH "^([0-9.]+) ([0-9a-f]+)$" PIN_OK "${PIN}")
if(NOT PIN_OK)
    message(FATAL_ERROR "packaging/proxy.sha256 should hold \"<proxy version> <sha256>\"")
endif()
set(PIN_VERSION "${CMAKE_MATCH_1}")
set(PIN_HASH "${CMAKE_MATCH_2}")
if(PIN_VERSION STREQUAL PROXY_VERSION)
    if(NOT BUILT_PROXY_HASH STREQUAL PIN_HASH)
        set(PROXY "")
        file(GLOB CANDIDATES "${OUT}/ReResorep-*/proxy/d3d11.dll")
        foreach(candidate IN LISTS CANDIDATES)
            file(SHA256 "${candidate}" CANDIDATE_HASH)
            if(CANDIDATE_HASH STREQUAL PIN_HASH)
                set(PROXY "${candidate}")
                break()
            endif()
        endforeach()
        if(NOT PROXY)
            message(FATAL_ERROR "The released proxy ${PIN_VERSION} (SHA-256 ${PIN_HASH}) is not in ${OUT}. "
                                "Put the release zip's ReResorep-<version> folder there, or, if the proxy "
                                "changed, raise RERESOREP_PROXY_VERSION in CMakeLists.txt.")
        endif()
        message(STATUS "Proxy ${PIN_VERSION}: shipping the released file from ${PROXY}")
        message(STATUS "  (this build's proxy differs; if you changed the proxy, raise RERESOREP_PROXY_VERSION)")
    endif()
else()
    message(WARNING "New proxy ${PROXY_VERSION}, SHA-256 ${BUILT_PROXY_HASH}. Mods that whitelist the proxy "
                    "need this hash. Once released, put \"${PROXY_VERSION} ${BUILT_PROXY_HASH}\" in "
                    "packaging/proxy.sha256.")
endif()

file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}/proxy")

file(COPY "${BIN}/ReResorep.exe" DESTINATION "${DIR}")
file(COPY "${PROXY}" DESTINATION "${DIR}/proxy")
configure_file("${SRC}/packaging/README.txt" "${DIR}/README.txt" @ONLY NEWLINE_STYLE CRLF)
configure_file("${SRC}/CREDITS.md" "${DIR}/CREDITS.md" COPYONLY)
configure_file("${SRC}/LICENSE" "${DIR}/LICENSE.txt" COPYONLY)

file(READ "${SRC}/third_party/imgui/LICENSE.txt" IMGUI_LICENSE)
file(WRITE "${DIR}/THIRD_PARTY_NOTICES.txt"
"ReResorep includes third-party code under the following licences.

==============================================================================
Dear ImGui v1.91.8 - https://github.com/ocornut/imgui
==============================================================================

${IMGUI_LICENSE}")

file(SHA256 "${DIR}/ReResorep.exe" EXE_HASH)
file(SHA256 "${DIR}/proxy/d3d11.dll" DLL_HASH)
# Checksum files use Unix line endings, so `sha256sum -c` (Git Bash, Linux, WSL) can check them;
# file(WRITE) would turn \n into CRLF on Windows. Notepad and PowerShell read LF fine.
file(CONFIGURE OUTPUT "${DIR}/SHA256SUMS.txt" NEWLINE_STYLE UNIX
     CONTENT "${EXE_HASH}  ReResorep.exe\n${DLL_HASH}  proxy/d3d11.dll\n")

execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${STAGE}/${NAME}-win64.zip" --format=zip "${NAME}"
                WORKING_DIRECTORY "${STAGE}" RESULT_VARIABLE ZIP_RESULT)
if(NOT ZIP_RESULT EQUAL 0)
    message(FATAL_ERROR "Creating ${ZIP} failed")
endif()
file(REMOVE_RECURSE "${OUT}/${NAME}")
file(RENAME "${DIR}" "${OUT}/${NAME}")
file(RENAME "${STAGE}/${NAME}-win64.zip" "${ZIP}")
file(SHA256 "${ZIP}" ZIP_HASH)
file(CONFIGURE OUTPUT "${ZIP}.sha256.txt" NEWLINE_STYLE UNIX CONTENT "${ZIP_HASH}  ${NAME}-win64.zip\n")

message(STATUS "Shipping folder: ${OUT}/${NAME}")
message(STATUS "Release zip:     ${ZIP}")
message(STATUS "SHA-256 (zip):   ${ZIP_HASH}")
message(STATUS "SHA-256 (proxy): ${DLL_HASH}")

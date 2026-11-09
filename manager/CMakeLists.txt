set(IMGUI ${PROJECT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_compile_options(imgui PRIVATE /W0)

# Licence texts compiled into the About tab, so they always ship with the exe.
file(READ ${IMGUI}/LICENSE.txt IMGUI_LICENSE)
# ReResorep's own licence. The GPL is longer than the 16 KB MSVC allows in one string literal,
# so it goes in as several adjacent raw literals, which the compiler joins.
file(READ ${PROJECT_SOURCE_DIR}/LICENSE GPL_TEXT)
string(LENGTH "${GPL_TEXT}" GPL_LENGTH)
set(GPL_LICENSE_LITERAL "")
set(GPL_POS 0)
while(GPL_POS LESS GPL_LENGTH)
    string(SUBSTRING "${GPL_TEXT}" ${GPL_POS} 8000 GPL_CHUNK)
    string(APPEND GPL_LICENSE_LITERAL "R\"RRLICENSE(${GPL_CHUNK})RRLICENSE\"\n")
    math(EXPR GPL_POS "${GPL_POS} + 8000")
endwhile()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${IMGUI}/LICENSE.txt ${PROJECT_SOURCE_DIR}/LICENSE)
configure_file(licenses.h.in ${RERESOREP_GENERATED}/licenses.h @ONLY)

add_executable(ReResorep WIN32
    main.cpp
    app.cpp
    games.cpp
    library.cpp
    thumbnails.cpp
    analyzer.cpp
    png_export.cpp
    bc_encode.cpp
    tint.cpp
    win_util.cpp
    theme.cpp
    updater.cpp
    manager.manifest
    app.rc
)
target_link_libraries(ReResorep PRIVATE resorep_core imgui d3d11 dxgi d3dcompiler shell32 ole32 version dwmapi windowscodecs bcrypt winhttp)
set_target_properties(ReResorep PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${RERESOREP_UNSEALED}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${RERESOREP_UNSEALED}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${RERESOREP_UNSEALED}
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${RERESOREP_UNSEALED}
    # Run and debug the sealed copy: the linked one refuses to start.
    VS_DEBUGGER_COMMAND ${RERESOREP_BIN}/ReResorep.exe
)
# The manager ships the proxy next to itself (bin/proxy/d3d11.dll).
add_dependencies(ReResorep proxy)
reresorep_seal(ReResorep ${RERESOREP_BIN}/ReResorep.exe)

# Zone of the Enders, native. Build with engine/tools/port.py, which generates the
# dispatch table and drives this file. (From engine/templates/game.)

cmake_minimum_required(VERSION 3.20)
project(zoe_us_port LANGUAGES CXX RC)     # RC: the icon and version

set(PS2_GENERATED_DIR "" CACHE PATH "dispatch.cpp, from engine/tools/port.py")

# The C and C++ runtime linked in (/MT): the executable runs on a Windows
# without the Visual C++ redistributable - every library here built so too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Source files' names (__FILE__) from the repository's root, not the folder it
# was built in: a release names no one's folders.
if(MSVC)
    get_filename_component(repo_root ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
    file(TO_NATIVE_PATH "${repo_root}/" repo_root_native)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl (engine/tools/port.py's default when LLVM is installed):
        # the same paths taken off - its own flag, both ways a path is written -
        # and no multiply-add fused into one rounding (MSVC fuses none): the
        # game's floats, and the software GS's, as MSVC's build has them.
        add_compile_options("/clang:-ffile-prefix-map=${repo_root}/=" "/clang:-ffile-prefix-map=${repo_root_native}="
                            "/clang:-ffp-contract=off")
    else()
        add_compile_options("/d1trimfile:${repo_root_native}")
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../engine/host ${CMAKE_BINARY_DIR}/ps2host)

ps2_add_port(zoe_us
    GAME ${CMAKE_CURRENT_LIST_DIR}/..
    GENERATED ${PS2_GENERATED_DIR}
    TRANSLATED ${CMAKE_CURRENT_LIST_DIR}/../src/lifted
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/overrides.cpp ${CMAKE_CURRENT_LIST_DIR}/dub.cpp)

# The Japanese subtitles' letters (SUBTITLE LANGUAGE JAPANESE, overrides.cpp): Noto Sans JP Bold (SIL Open Font
# License 1.1, port/fonts/NotoSansJP-OFL.txt) cut to the subtitles' characters (tools/m10/font_subset.md), built in.
set(jp_font_file ${CMAKE_CURRENT_LIST_DIR}/fonts/NotoSansJP-Bold-zoe.otf)
set(jp_font_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/noto_sans_jp.inc)
if(NOT EXISTS ${jp_font_inc} OR ${jp_font_file} IS_NEWER_THAN ${jp_font_inc})
    file(READ ${jp_font_file} jp_font_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," jp_font_bytes "${jp_font_hex}")
    file(WRITE ${jp_font_inc} "// From port/fonts/NotoSansJP-Bold-zoe.otf (port/CMakeLists.txt): generated.\n${jp_font_bytes}\n")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${jp_font_file})
target_include_directories(zoe_us PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Zone of the Enders Recompiled for Android - the app's own native libraries (agent L, 2026-11-21).
# libvulkan_zoe.so: the game's Vulkan library - the system driver, or the player's Turnip through libadrenotools
# (vkshim/vulkan_zoe.c, from the Orta Recompiled Android app). libadrenotools (BSD-2): -DADRENOTOOLS_SRC=<source>;
# its hook libraries (main_hook, hook_impl, file_redirect_hook, gsl_alloc_hook) land in nativeLibraryDir with it.
# libzoe_app.so: the app's own JNI part (app/touchpad.c: the touch controls as a virtual SDL gamepad), linked against
# the APK's libSDL3.so: -DSDL3_SRC=<SDL3 source> (headers: ${SDL3_SRC}/include), -DSDL3_LIB=<that libSDL3.so>.
cmake_minimum_required(VERSION 3.22)
project(zoe_android_native C CXX ASM)
if(NOT ADRENOTOOLS_SRC)
    message(FATAL_ERROR "-DADRENOTOOLS_SRC=<libadrenotools source> missing")
endif()
add_subdirectory(${ADRENOTOOLS_SRC} adrenotools EXCLUDE_FROM_ALL)
add_library(vulkan_zoe SHARED vkshim/vulkan_zoe.c vkshim/vk_trampolines.S)
set_target_properties(vulkan_zoe PROPERTIES LINKER_LANGUAGE CXX)   # libadrenotools is C++ (static libc++)
target_link_libraries(vulkan_zoe PRIVATE adrenotools linkernsbypass log dl)
target_include_directories(vulkan_zoe PRIVATE ${ADRENOTOOLS_SRC}/lib/linkernsbypass)
# --exclude-libs: linkernsbypass's function-pointer variables must not leave this library (Orta, Tab S8 2026-11-11)
target_link_options(vulkan_zoe PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-soname,libvulkan_zoe.so"
                    "-Wl,--exclude-libs,ALL")

if(NOT SDL3_SRC OR NOT SDL3_LIB)
    message(FATAL_ERROR "-DSDL3_SRC=<SDL3 source> / -DSDL3_LIB=<the APK's arm64 libSDL3.so> missing")
endif()
add_library(sdl3_shared SHARED IMPORTED)
set_target_properties(sdl3_shared PROPERTIES IMPORTED_LOCATION "${SDL3_LIB}"
                      INTERFACE_INCLUDE_DIRECTORIES "${SDL3_SRC}/include")
add_library(zoe_app SHARED app/touchpad.c)
target_link_libraries(zoe_app PRIVATE sdl3_shared log)
target_compile_options(zoe_app PRIVATE -Wall -Wextra)
target_link_options(zoe_app PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-soname,libzoe_app.so")

add_custom_target(zoe_native ALL DEPENDS vulkan_zoe zoe_app main_hook hook_impl file_redirect_hook gsl_alloc_hook)

# The setup's programs (SetupActivity; an app may exec only from nativeLibraryDir, so they ship as lib*_exe.so):
# libpython_exe.so (python.org's Android CPython, embedded: the translation), libld_exe.so (clang's --ld-path: execs
# B's liblld_exe.so as "ld.lld"), libzoe_run_exe.so (the device-built game as a program: the discs' unpacking).
# -DPYTHON_PREFIX=<python.org Android package prefix> (libpython3.15.so + include/python3.15).
if(NOT PYTHON_PREFIX)
    message(FATAL_ERROR "-DPYTHON_PREFIX=<python.org Android package prefix> missing")
endif()
add_library(zoe_python SHARED IMPORTED)
set_target_properties(zoe_python PROPERTIES IMPORTED_LOCATION "${PYTHON_PREFIX}/lib/libpython3.15.so"
                      INTERFACE_INCLUDE_DIRECTORIES "${PYTHON_PREFIX}/include/python3.15")
foreach(exe python_exe ld_exe run_exe)
    add_executable(${exe} setup/${exe}.c)
    target_link_options(${exe} PRIVATE "-Wl,-z,max-page-size=16384")
endforeach()
set_target_properties(python_exe PROPERTIES OUTPUT_NAME "libpython_exe" SUFFIX ".so")
set_target_properties(ld_exe PROPERTIES OUTPUT_NAME "libld_exe" SUFFIX ".so")
set_target_properties(run_exe PROPERTIES OUTPUT_NAME "libzoe_run_exe" SUFFIX ".so")
target_link_libraries(python_exe PRIVATE zoe_python)
target_link_libraries(run_exe PRIVATE dl)
add_dependencies(zoe_native python_exe ld_exe run_exe)

# libchd2iso_exe.so (2026-11-24): a CHD disc image as the .iso the translation's tools read (the PC launcher's
# chd_job; the game reads a CHD itself). Linked against the engine's libchdr from the Android engine build:
# -DLIBCHDR_BUILD=<engine build>/libchdr (libchdr-static.a, deps/...) -DLIBCHDR_INCLUDE=<engine>/host/third_party/
# libchdr/include.
if(NOT LIBCHDR_BUILD OR NOT LIBCHDR_INCLUDE)
    message(FATAL_ERROR "-DLIBCHDR_BUILD=<engine build>/libchdr / -DLIBCHDR_INCLUDE=<libchdr include> missing")
endif()
add_executable(chd2iso_exe setup/chd2iso_exe.c)
set_target_properties(chd2iso_exe PROPERTIES OUTPUT_NAME "libchd2iso_exe" SUFFIX ".so")
target_include_directories(chd2iso_exe PRIVATE ${LIBCHDR_INCLUDE})
file(GLOB CHD2ISO_LZMA "${LIBCHDR_BUILD}/deps/lzma-*/libchdr-lzma.a")
file(GLOB CHD2ISO_MINIZ "${LIBCHDR_BUILD}/deps/miniz-*/libminiz.a")
file(GLOB CHD2ISO_ZSTD "${LIBCHDR_BUILD}/deps/zstd-*/libzstd.a")
target_link_libraries(chd2iso_exe PRIVATE "${LIBCHDR_BUILD}/libchdr-static.a" ${CHD2ISO_LZMA} ${CHD2ISO_MINIZ}
                      ${CHD2ISO_ZSTD} m)
target_link_options(chd2iso_exe PRIVATE "-Wl,-z,max-page-size=16384")
add_dependencies(zoe_native chd2iso_exe)

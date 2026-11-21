# Zone of the Enders Recompiled for Android - the app's own native libraries (agent L, 2026-11-21).
# libvulkan_zoe.so: the game's Vulkan library - the system driver, or the player's Turnip through libadrenotools
# (vkshim/vulkan_zoe.c, from the Orta Recompiled Android app). libadrenotools (BSD-2): -DADRENOTOOLS_SRC=<source>;
# its hook libraries (main_hook, hook_impl, file_redirect_hook, gsl_alloc_hook) land in nativeLibraryDir with it.
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
add_custom_target(zoe_native ALL DEPENDS vulkan_zoe main_hook hook_impl file_redirect_hook gsl_alloc_hook)

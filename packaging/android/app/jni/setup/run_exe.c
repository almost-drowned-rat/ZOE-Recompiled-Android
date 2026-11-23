/* run_exe.c - the device-built game as a program, for the setup's unpacking (agent L, 2026-11-23).
 *
 * The PC launcher unpacks the discs by starting the game: `<game> --unpack-disc <image> <folder>` (engine/host/src
 * main.cpp; progress in <folder>.progress). On Android the game is a library (<files>/bin/libZOE_game.so, its main()
 * is SDL_main) and an app may exec only from nativeLibraryDir - so SetupActivity runs this (libzoe_run_exe.so) with
 * XR_GAME_LIB = that library, PS2HOST_HOME = the app's external files dir (the game's folder without SDL's JNI) and
 * LD_LIBRARY_PATH = nativeLibraryDir (its libSDL3.so); the arguments go to SDL_main unchanged.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*MainFn)(int, char **);

int main(int argc, char **argv)
{
    const char *lib = getenv("XR_GAME_LIB");
    void *h;
    MainFn fn;
    if (!lib || !*lib) {
        fprintf(stderr, "run_exe: XR_GAME_LIB not set\n");
        return 120;
    }
    h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "run_exe: %s\n", dlerror());
        return 121;
    }
    fn = (MainFn)dlsym(h, "SDL_main");
    if (!fn) {
        fprintf(stderr, "run_exe: no SDL_main in %s\n", lib);
        return 122;
    }
    return fn(argc, argv);
}

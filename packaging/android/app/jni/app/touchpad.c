/* touchpad.c - the touch controls as one virtual SDL gamepad (libzoe_app.so). Agent L, 2026-11-21, from the Orta
 * Recompiled Android app's jni/loader/touchpad.c (O, 2026-11-11).
 *
 * TouchOverlay.java draws a DualShock 2's controls and sends their state here (JNI, the UI thread); the game
 * (engine/gpu vulkan_display.cpp: SDL_EVENT_GAMEPAD_ADDED -> SDL_OpenGamepad, ReadPad by position) sees an ordinary
 * SDL gamepad: Cross / Circle / Square / Triangle = SOUTH / EAST / WEST / NORTH, L1 / R1 = the shoulders, L2 / R2 =
 * the trigger axes, START, SELECT = BACK, L3 / R3, the D-pad, the floating stick = the left stick, the fixed one =
 * the right stick. libSDL3.so is one shared library for the Java side, this library and libmain.so, so the device
 * attached here is the game's.
 *
 * Unlike Orta there is no loader that runs before the game: the game itself brings SDL's gamepad subsystem up
 * (SDL_InitSubSystem(SDL_INIT_GAMEPAD) in its display), so the virtual pad is attached lazily - on the first touch
 * that presses something, once SDL_WasInit says the subsystem is up. A real gamepad's input detaches it
 * (ZoeActivity, mode AUTO: nativeTouchPadEnable(false)); a touch attaches it again. No game hooks (Orta's pad
 * priority / touch aiming) - the game merges every controller into player 1's input itself.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <SDL3/SDL.h>
#include <android/log.h>
#include <jni.h>
#include <pthread.h>

#include "touchpad.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "ZOE", __VA_ARGS__)

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static SDL_JoystickID s_id;
static SDL_Joystick *s_joy;
static int s_enabled = 1;                       /* nativeTouchPadEnable: off while a real gamepad is used */

/* the gamepad subsystem is up (the game's); a pad attached before it went down (SDL_Quit at the game's end) is gone */
static int ready(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        s_id = 0;
        s_joy = NULL;                           /* SDL closed and freed it with the subsystem */
        return 0;
    }
    if (s_id && !SDL_IsJoystickVirtual(s_id)) { /* the subsystem was restarted: the old id is stale */
        s_id = 0;
        s_joy = NULL;
    }
    return 1;
}

static void attach(void)
{
    SDL_VirtualJoystickDesc d;
    if (s_id)
        return;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;           /* all six: the joystick axis index = the SDL_GamepadAxis */
    d.nbuttons = 14;                            /* the buttons in button_mask (see the map in nativeTouchPad) */
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                  (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                  (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                    (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                    (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START) |
                    (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
                    (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) |
                    (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) | (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
                    (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) | (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    d.name = ZOE_TOUCHPAD_NAME;
    s_id = SDL_AttachVirtualJoystick(&d);
    s_joy = s_id ? SDL_OpenJoystick(s_id) : NULL;
    LOG("[TOUCH] virtual gamepad %s (id %u)%s%s", s_joy ? "attached" : "failed", (unsigned)s_id,
        s_joy ? "" : ": ", s_joy ? "" : SDL_GetError());
}

static void detach(void)
{
    if (!s_id)
        return;
    if (s_joy)
        SDL_CloseJoystick(s_joy);
    SDL_DetachVirtualJoystick(s_id);
    s_joy = NULL;
    s_id = 0;
    LOG("[TOUCH] virtual gamepad detached (a real gamepad is used)");
}

JNIEXPORT void JNICALL Java_io_github_almostdrownedrat_zoe_ZoeActivity_nativeTouchPadEnable(JNIEnv *env, jclass c,
                                                                                             jboolean on)
{
    (void)env; (void)c;
    pthread_mutex_lock(&s_mx);
    s_enabled = on ? 1 : 0;
    if (ready() && !on)
        detach();                               /* (on: attached at the next touch that presses something) */
    pthread_mutex_unlock(&s_mx);
}

JNIEXPORT void JNICALL Java_io_github_almostdrownedrat_zoe_TouchOverlay_nativeTouchPad(JNIEnv *env, jclass c,
    jint b, jfloat lx, jfloat ly, jfloat lt, jfloat rt, jfloat rx, jfloat ry)
{
    /* TouchOverlay's bit i -> the virtual JOYSTICK button. SDL_SetJoystickVirtualButton takes the joystick index,
     * and with a button_mask SDL numbers only the masked buttons, in SDL_GamepadButton order (SDL3
     * SDL_virtualjoystick.c VIRTUAL_JoystickGetGamepadMapping): SOUTH 0, EAST 1, WEST 2, NORTH 3, BACK 4, START 5,
     * LEFT_STICK 6, RIGHT_STICK 7, LEFT_SHOULDER 8, RIGHT_SHOULDER 9, DPAD_UP 10, DOWN 11, LEFT 12, RIGHT 13
     * (Orta 2026-11-11: the SDL_GamepadButton enums themselves pressed the wrong buttons). -1 = not a button. */
    static const signed char map[18] = {
        0 /* CROSS = SOUTH */, 1 /* CIRCLE = EAST */, 2 /* SQUARE = WEST */, 3 /* TRIANGLE = NORTH */,
        8 /* L1 = LEFT_SHOULDER */, 9 /* R1 = RIGHT_SHOULDER */, 5 /* START */, -1 /* L2 */, -1 /* R2 */,
        -1 /* STICK */, 4 /* SELECT = BACK */, 6 /* L3 */, 7 /* R3 */,
        10 /* D-pad up */, 11 /* down */, 12 /* left */, 13 /* right */, -1 /* RIGHT STICK */,
    };
    int i, any;
    (void)env; (void)c;
    pthread_mutex_lock(&s_mx);
    if (!ready() || !s_enabled) {
        pthread_mutex_unlock(&s_mx);
        return;
    }
    /* only a press attaches the pad: a release (the menu opening, the controls hiding) never adds a controller */
    any = b != 0 || lx != 0 || ly != 0 || lt != 0 || rt != 0 || rx != 0 || ry != 0;
    if (!s_joy && any)
        attach();
    if (s_joy) {
        for (i = 0; i < 18; i++)
            if (map[i] >= 0)
                SDL_SetJoystickVirtualButton(s_joy, map[i], (b >> i) & 1);
        /* all six axes are in the mask, so here the joystick axis = the SDL_GamepadAxis; a trigger rests at
         * SDL_JOYSTICK_AXIS_MIN (SDL maps the joystick's full range onto 0..32767) */
        SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)(lx * 32767));
        SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)(ly * 32767));
        SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)(rx * 32767));
        SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_RIGHTY, (Sint16)(ry * 32767));
        SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (Sint16)(-32768 + (int)(lt * 65535)));
        SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (Sint16)(-32768 + (int)(rt * 65535)));
    }
    pthread_mutex_unlock(&s_mx);
}

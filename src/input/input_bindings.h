/*
 * input_bindings.h -- host devices and per-control bindings, from a config
 * file, for all four Xbox controller ports.
 *
 * One place decides what the four guest pads read: which host device stands
 * in for each port, and which key or pad control drives each of the title's
 * 24 inputs. Both input paths use it -- the XAPI replacement in src/hle and
 * the USB gamepad in src/usb -- so a binding changed once applies to a title
 * whichever way it reads its pads.
 *
 * The file is JSON, written by `py -3 -m tools.input_ui`; see
 * docs/technical/input-binding.md for the format and the search order. With
 * no file present the built-in defaults are what the runtime always did:
 * XInput pad 0 plus the keyboard on port 1, XInput pad N on port N.
 */
#ifndef XBOXRECOMP_INPUT_BINDINGS_H
#define XBOXRECOMP_INPUT_BINDINGS_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the config, once. Safe to call from anywhere and from any thread that
 * reaches it first; later calls do nothing. Logs one line naming the file it
 * read (or the built-in defaults) and the device on each port. */
void recomp_bindings_init(void);

/* The config file actually read, or NULL when the built-in defaults are in
 * use. Valid after recomp_bindings_init(). */
const char *recomp_bindings_source_path(void);

/* A human name for what drives this port: "SDL3 pad 1 + keyboard",
 * "keyboard", "XInput pad 3", "nothing". Never NULL. */
const char *recomp_bindings_device_name(unsigned port);

/* Bitmask of ports whose device is present right now, bit 0 = port 1. A
 * keyboard port is always present; a pad port is present when that pad
 * answers, or when the port also has keyboard bindings. Re-checks the pads
 * at most a few times a second, so a pad plugged in mid-game shows up. */
unsigned recomp_bindings_present_mask(void);

/* Sample one port through its bindings. Returns 0 (and a neutral pad) when
 * the port has no device. */
int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out);

/* Rumble on the pad this port reads, 0..65535 per motor as XInputSetState
 * takes it. Nothing happens for a keyboard port or a pad without motors. */
void recomp_bindings_rumble(unsigned port, unsigned short low, unsigned short high);

/* ---- the mouse ----------------------------------------------------------
 *
 * Fed by whoever owns the game window (src/hle/hle_d3d8.c), because only
 * the window hears the mouse. Its buttons are `mouse:` sources any control
 * can be bound to. Its motion is mouse look: with the title's `mouse_look`
 * setting on (RECOMP_MOUSE_LOOK), controller 1's right stick follows the
 * mouse as well as whatever is bound to it, so the pad's own stick still
 * works. Every call is safe from any thread. */
enum {
    RECOMP_MOUSE_LEFT, RECOMP_MOUSE_RIGHT, RECOMP_MOUSE_MIDDLE,
    RECOMP_MOUSE_X1, RECOMP_MOUSE_X2,
    RECOMP_MOUSE_BUTTONS,
    /* Sources only, past the buttons: the wheel's two directions
     * (mouse:wheel_up, mouse:wheel_down), fed by recomp_mouse_wheel. */
    RECOMP_MOUSE_WHEEL_UP = RECOMP_MOUSE_BUTTONS,
    RECOMP_MOUSE_WHEEL_DOWN,
    RECOMP_MOUSE_SOURCES
};

/* The wheel turned: positive notches away from the player (up), negative
 * towards. Each is a short press of mouse:wheel_up or mouse:wheel_down. */
void recomp_mouse_wheel(int notches);

/* Mouse look is on and controller 1 reads something: the window should
 * take the pointer (hide it and keep it inside) while it has the focus. */
int  recomp_mouse_look(void);
/* Relative motion in mouse counts, x right and y down. Only counted while
 * mouse look is on. */
void recomp_mouse_move(float dx, float dy);
void recomp_mouse_button(int button, int down);
/* The window lost the focus or the pointer: every button up, and motion
 * not yet read is dropped, so nothing is left held or moving. */
void recomp_mouse_release(void);

/* Mouse look's settings changed while the title runs (the in-game menu):
 * on or off, the sensitivity (RECOMP_MOUSE_SENSITIVITY_MIN..MAX) and
 * whether forward looks down. */
void recomp_mouse_look_configure(int on, int sensitivity, int invert);
int  recomp_mouse_invert(void);

/* 1 while the in-game menu has the keyboard and mouse: every port then
 * reads as connected with nothing pressed (recomp_bindings_sample). */
void recomp_bindings_block(int on);

/* Called when a controller asks for the in-game menu -- its Guide button, or
 * Back and Start together -- from the thread reading the controllers. The
 * menu installs it (hle_menu.cpp); the menu, once open, reads the controllers
 * itself. */
void recomp_bindings_set_menu_hook(void (*open_menu)(void));

/* The menu closed from a controller: the buttons held now reach the title only
 * once let go. */
void recomp_bindings_swallow_held(void);

/* The title's own menu is up, not play (1), or play again (0): the left
 * button is A, the right B and the wheel up and down while it is, and the
 * buttons' bindings rest. The window's side is hle_window_title_menu, which
 * calls this. */
void recomp_mouse_title_menu(int on);
int  recomp_mouse_title_menu_on(void);

/* The title's own cursor is up, moved by a stick (Future Perfect's MapMaker):
 * the mouse's buttons work as the pad's, as in the title's menus above, but
 * the window keeps the pointer (recomp_mouse_pad_buttons); and mouse look
 * without native look moves the left stick, not the right, so the mouse moves
 * the cursor (recomp_mouse_look_left; not turned over by mouse_invert). */
void recomp_mouse_pad_buttons(int on);
void recomp_mouse_look_left(int on);
/* The dead zone the title's stick reads have, as a share of a full push
 * (0, the default: none of ours). A mouse that moves a stick is then lifted
 * past it, so a slow movement still moves (Future Perfect: 0.25). */
void recomp_mouse_stick_dead_zone(float dz);

/* ---- the number row and the wheel, a PC shooter's -----------------------
 *
 * A PC shooter picks its weapons with the number keys and zooms with the
 * wheel; a console title has neither. A number key goes to the title as a
 * value it acts on itself -- recomp_input_number_key (src/hle/hle_input.c),
 * the latched one under online co-op, so both machines act on it alike. The
 * wheel, when a title takes it, goes to the title's filter, which turns it
 * into the title's own controls on controller 1 (a swap, a zoom); then
 * bindings to mouse:wheel_up and mouse:wheel_down are left to it. Neither
 * acts while the title's own menu has the mouse or the in-game menu is up. */
/* The number key 1..9 pressed most recently since the last call, or 0. */
int  recomp_input_take_number_key(void);
/* fn: wheel is +1 for a press away from the player, -1 towards, 0 none;
 * pad, controller 1 as bound, which fn adds the controls it means to. */
void recomp_bindings_set_wheel_filter(void (*fn)(int wheel, XBOX_GAMEPAD *pad));

/* Native mouse look: the title turns its camera by the mouse as an angle
 * (TimeSplitters 2 adds it to the player's own yaw and pitch), and the right
 * stick is left to the pad. The title opts in at start; then each frame it
 * asks for the look through recomp_input_look (src/hle/hle_input.c), which
 * under online co-op is the latched one. */
void recomp_mouse_look_native(int on);
int  recomp_mouse_look_is_native(void);
/* The mouse's look since the last call, in degrees: x to the right, y down,
 * sensitivity and invert applied; zero with mouse look off or the in-game
 * menu open. Takes it: each movement is given once. */
void recomp_mouse_take_look(float *dx_deg, float *dy_deg);

#ifdef __cplusplus
}
#endif

#endif

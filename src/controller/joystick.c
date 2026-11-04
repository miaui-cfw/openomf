#include "controller/joystick.h"
#include "utils/allocator.h"
#include "utils/log.h"
#include <stdlib.h>
#ifdef RG35XX
#include "game/game_state.h"
#include "resources/ids.h"
#include "video/renderers/rg35xx/rg35xx_renderer.h"
#endif

#define UP -32768
#define DOWN 32767
#define LEFT -32768
#define RIGHT 32767
#define CENTER 0

void joystick_free(controller *ctrl) {
    joystick *k = ctrl->data;
    if(k->haptic) {
        SDL_HapticClose(k->haptic);
    }
    SDL_GameControllerClose(k->joy);
    omf_free(k->keys);
    omf_free(k);
}

static inline void joystick_cmd(controller *ctrl, int action, ctrl_event **ev) {
    controller_cmd(ctrl, action, ev);
}

int joystick_count(void) {
    int valid_joysticks = 0;
    SDL_Joystick *joy;
    for(int i = 0; i < SDL_NumJoysticks(); i++) {
        joy = SDL_JoystickOpen(i);
        if(joy) {
            valid_joysticks++;
        }
        if(SDL_JoystickGetAttached(joy)) {
            SDL_JoystickClose(joy);
        }
    }
    return valid_joysticks;
}

int joystick_nth_id(int n) {
    SDL_Joystick *joy;
    int c = 0;
    for(int i = 0; i < SDL_NumJoysticks(); i++) {
        joy = SDL_JoystickOpen(i);
        if(joy) {
            c++;
            if(SDL_JoystickGetAttached(joy)) {
                SDL_JoystickClose(joy);
            }
            if(c == n) {
                return i;
            }
        }
    }
    return -1;
}

int joystick_offset(int id, const char *name) {
    int offset = 0;
    for(int i = 0; i < id; i++) {
        if(i != id && !strcmp(name, SDL_JoystickNameForIndex(i))) {
            offset++;
        }
    }
    return offset;
}

int joystick_name_to_id(const char *name, int offset) {
    for(int i = 0; i < SDL_NumJoysticks(); i++) {
        if(!strcmp(name, SDL_JoystickNameForIndex(i))) {
            if(offset) {
                offset--;
            } else {
                return i;
            }
        }
    }
    return -1;
}

#ifdef RG35XX
/*
 * The RG35XX: A and X punch, B and Y kick; START and MENU are the escape (the pause menu in a fight). B is also
 * the way back: everywhere but in a fight (an arena, not paused by its menu, not a demo), B is the escape and not a
 * kick, for the menus and for screens that read the players' pads (the melee selection), so a B never both kicks
 * (selects) and goes back.
 */
static bool b_is_back(const controller *ctrl) {
    game_state *gs = ctrl->gs;
    return gs == NULL || !is_arena(gs->this_id) || game_state_is_paused(gs) || is_demoplay(gs);
}

static bool pressed(const joystick *k, int button) {
    return button != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(k->joy, button);
}

/*
 * An attack button: in a menu it counts (and is remembered as held); in a fight a button still held from a menu
 * counts only after it is released, so the A or B that closes the pause menu does not also punch or kick. Kept per
 * pad (its instance), not per controller: while the game is paused only the menu controller polls the pad.
 */
static unsigned held_from_menu[8];

static bool attack_button(const joystick *k, int button, bool menu) {
    if(button == SDL_CONTROLLER_BUTTON_INVALID) {
        return false;
    }
    unsigned *held = &held_from_menu[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(k->joy)) & 7];
    const unsigned bit = 1u << button;
    if(!SDL_GameControllerGetButton(k->joy, button)) {
        *held &= ~bit;
        return false;
    }
    if(menu) {
        *held |= bit;
        return true;
    }
    return !(*held & bit);
}
#endif

static int internal_joystick_poll(joystick *k, controller *ctrl, ctrl_event **ev, bool allow_esc) {
    if(!SDL_GameControllerGetAttached(k->joy)) {
        controller_close(ctrl, ev);
        return 0;
    }

    Sint16 x_axis = SDL_GameControllerGetAxis(k->joy, k->keys->x_axis);
    Sint16 y_axis = SDL_GameControllerGetAxis(k->joy, k->keys->y_axis);
    int dpadup = SDL_GameControllerGetButton(k->joy, k->keys->dpad[0]);
    int dpaddown = SDL_GameControllerGetButton(k->joy, k->keys->dpad[1]);
    int dpadleft = SDL_GameControllerGetButton(k->joy, k->keys->dpad[2]);
    int dpadright = SDL_GameControllerGetButton(k->joy, k->keys->dpad[3]);

    int action = ACT_NONE;

    // joystick input
    // TODO the devide by 2 should be a 'dead zone' variable that can be set in the option menu but this devide works
    // well 99% of the time. Analog Stick (Axis 1) Movement
    if(x_axis <= LEFT / 2 && y_axis <= UP / 2) {
        action = ACT_UP | ACT_LEFT;
    } else if(x_axis <= LEFT / 2 && y_axis >= DOWN / 2) {
        action = ACT_DOWN | ACT_LEFT;
    } else if(x_axis >= RIGHT / 2 && y_axis <= UP / 2) {
        action = ACT_UP | ACT_RIGHT;
    } else if(x_axis >= RIGHT / 2 && y_axis >= DOWN / 2) {
        action = ACT_DOWN | ACT_RIGHT;
    } else if(x_axis >= RIGHT / 2) {
        action = ACT_RIGHT;
    } else if(x_axis <= LEFT / 2) {
        action = ACT_LEFT;
    } else if(y_axis <= UP / 2) {
        action = ACT_UP;
    } else if(y_axis >= DOWN / 2) {
        action = ACT_DOWN;
        // check the d-pad instead
    } else if(dpadup && dpadleft) {
        action = ACT_UP | ACT_LEFT;
    } else if(dpaddown && dpadleft) {
        action = ACT_DOWN | ACT_LEFT;
    } else if(dpadup && dpadright) {
        action = ACT_UP | ACT_RIGHT;
    } else if(dpaddown && dpadright) {
        action = ACT_DOWN | ACT_RIGHT;
    } else if(dpadright) {
        action = ACT_RIGHT;
    } else if(dpadleft) {
        action = ACT_LEFT;
    } else if(dpadup) {
        action = ACT_UP;
    } else if(dpaddown) {
        action = ACT_DOWN;
    }

    // button input
#ifdef RG35XX
    const bool back = b_is_back(ctrl);
    const bool a = attack_button(k, k->keys->punch, back), x = attack_button(k, k->keys->punch2, back);
    const bool b = attack_button(k, k->keys->kick, back), y = attack_button(k, k->keys->kick2, back);
    if(a || x) {
        action |= ACT_PUNCH;
    } else if((!back && b) || y) {
        action |= ACT_KICK;
    }

    // --stats: a player's attacks as they change (what a fight receives)
    if(rg35xx_renderer_stats && !allow_esc) {
        static int last_attack = -1;
        const int attack = action & (ACT_PUNCH | ACT_KICK);
        if(attack != last_attack) {
            log_info("input: %s%s%s", attack & ACT_PUNCH ? "punch" : "", attack & ACT_KICK ? "kick" : "",
                     attack ? (back ? " (menu context)" : " (fight)") : "none");
            last_attack = attack;
        }
    }

    if(action) {
        joystick_cmd(ctrl, action, ev);
    }

    if(allow_esc && (pressed(k, k->keys->escape) || pressed(k, k->keys->escape2) || (back && b))) {
        joystick_cmd(ctrl, ACT_ESC, ev);
    }
#else
    if(SDL_GameControllerGetButton(k->joy, k->keys->punch)) {
        action |= ACT_PUNCH;
    } else if(SDL_GameControllerGetButton(k->joy, k->keys->kick)) {
        action |= ACT_KICK;
    }

    if(action) {
        joystick_cmd(ctrl, action, ev);
    }

    if(allow_esc && SDL_GameControllerGetButton(k->joy, k->keys->escape)) {
        joystick_cmd(ctrl, ACT_ESC, ev);
    }
#endif

    if(ctrl->current == 0) {
        joystick_cmd(ctrl, ACT_STOP, ev);
    }

    return 0;
}

int joystick_poll(controller *ctrl, ctrl_event **ev) {
    joystick *k = ctrl->data;

    ctrl->last = ctrl->current;
    ctrl->current = 0;

    return internal_joystick_poll(k, ctrl, ev, false);
}

int joystick_rumble(controller *ctrl, float magnitude, int duration) {
    joystick *k = ctrl->data;
    if(k->rumble == 1) {
        SDL_HapticRumblePlay(k->haptic, magnitude, duration);
    }
    return 0;
}

static inline void internal_joystick_default_keys(joystick_keys *keys) {
    keys->x_axis = SDL_CONTROLLER_AXIS_LEFTX;
    keys->y_axis = SDL_CONTROLLER_AXIS_LEFTY;
    keys->dpad[0] = SDL_CONTROLLER_BUTTON_DPAD_UP;
    keys->dpad[1] = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    keys->dpad[2] = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    keys->dpad[3] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    keys->punch = SDL_CONTROLLER_BUTTON_A;
    keys->kick = SDL_CONTROLLER_BUTTON_B;
    keys->escape = SDL_CONTROLLER_BUTTON_START;
#ifdef RG35XX
    keys->punch2 = SDL_CONTROLLER_BUTTON_X;
    keys->kick2 = SDL_CONTROLLER_BUTTON_Y;
    keys->escape2 = SDL_CONTROLLER_BUTTON_GUIDE;
#endif
}

int joystick_create(controller *ctrl, int joystick_id) {
    joystick *k = omf_calloc(1, sizeof(joystick));
    k->keys = omf_calloc(1, sizeof(joystick_keys));
    internal_joystick_default_keys(k->keys);
    k->rumble = 0;
    ctrl->data = k;
    ctrl->type = CTRL_TYPE_GAMEPAD;
    ctrl->poll_fun = &joystick_poll;
    ctrl->free_fun = &joystick_free;
    ctrl->supports_delay = true;

    k->joy = SDL_GameControllerOpen(joystick_id);
    if(k->joy) {
        k->haptic = SDL_HapticOpenFromJoystick(SDL_GameControllerGetJoystick(k->joy));
        if(k->haptic) {
            if(SDL_HapticRumbleSupported(k->haptic)) {
                if(SDL_HapticRumbleInit(k->haptic) == 0) {
                    k->rumble = 1;
                    ctrl->rumble_fun = joystick_rumble;
                } else {
                    log_error("Failed to initialize rumble: %s", SDL_GetError());
                }
            } else {
                log_debug("Rumble not supported");
            }
        } else {
            log_debug("Haptic not supported");
        }
        log_debug("Game controller initialized");
        return 1;
    }

    log_error("Failed to open game controller: %s", SDL_GetError());
    return 0;
}

static vector every_gamepad;

void joystick_init(void) {
    int num_joysticks = SDL_NumJoysticks();
    vector_create_with_size(&every_gamepad, sizeof(SDL_GameController *), num_joysticks);

    for(int idx = 0; idx < num_joysticks; idx++) {
        joystick_deviceadded(idx);
    }
}

void joystick_close(void) {
    iterator it;
    vector_iter_begin(&every_gamepad, &it);
    SDL_GameController **gamepad;
    foreach(it, gamepad) {
        SDL_GameControllerClose(*gamepad);
    }
    vector_free(&every_gamepad);
}

void joystick_menu_poll_all(controller *menu_ctrl, ctrl_event **ev) {
    if(vector_size(&every_gamepad) == 0) {
        return;
    }

    joystick k;
    memset(&k, 0, sizeof(k));
    joystick_keys keys;
    internal_joystick_default_keys(&keys);
    k.keys = &keys;

    iterator it;
    vector_iter_begin(&every_gamepad, &it);
    SDL_GameController **gamepad;
    foreach(it, gamepad) {
        k.joy = *gamepad;
        internal_joystick_poll(&k, menu_ctrl, ev, true);
    }
}

void joystick_deviceadded(int sdl_joystick_index) {
    SDL_GameController *gamepad = SDL_GameControllerOpen(sdl_joystick_index);
    if(!gamepad) {
        return;
    }
    vector_append(&every_gamepad, &gamepad);
}

void joystick_deviceremoved(int sdl_joystick_instance_id) {
    unsigned gamepad_count = vector_size(&every_gamepad);
    for(unsigned idx = 0; idx < gamepad_count; idx++) {
        SDL_GameController *gamepad = *(SDL_GameController **)vector_get(&every_gamepad, idx);
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(gamepad);
        if(SDL_JoystickInstanceID(joy) == sdl_joystick_instance_id) {
            SDL_GameControllerClose(gamepad);
            vector_swapdelete_at(&every_gamepad, idx);
            return;
        }
    }
}

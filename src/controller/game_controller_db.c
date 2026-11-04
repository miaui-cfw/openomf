#include <SDL_gamecontroller.h>
#include <stdio.h>
#include <string.h>

#include "resources/resource_files.h"
#include "utils/log.h"
#include "utils/path.h"
#include "utils/str.h"

void joystick_load_external_mappings(void) {
    int loaded;
    path db_filename = get_game_controller_db_filename();
    if((loaded = SDL_GameControllerAddMappingsFromFile(path_c(&db_filename))) > 0) {
        log_info("Loaded %d external game controller mappings from %s", loaded, path_c(&db_filename));
        log_info("We have %d known game controller mappings", SDL_GameControllerNumMappings());
    } else {
        log_info("No external game controller mappings file found from '%s' (%s); skipping ...", path_c(&db_filename),
                 SDL_GetError());
    }
#ifdef RG35XX
    // The RG35XX's pad, by its labels (MinUI's button numbers): A right (0), B bottom (1), X top (2), Y left (3),
    // L1 R1 L2 R2 (4-7), SELECT 8, START 9, MENU 10 (guide), the d-pad buttons 13-16. No axes, no hat.
    for(int i = 0; i < SDL_NumJoysticks(); i++) {
        // ours even if a generic mapping matched the GUID (adding replaces it)
        if(strcmp(SDL_JoystickNameForIndex(i), "RG35XX Gamepad") != 0) {
            continue;
        }
        char guid[33], mapping[320];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
        snprintf(mapping, sizeof mapping,
                 "%s,RG35XX Gamepad,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,"
                 "righttrigger:b7,back:b8,start:b9,guide:b10,dpup:b13,dpdown:b14,dpleft:b15,dpright:b16,",
                 guid);
        if(SDL_GameControllerAddMapping(mapping) < 0) {
            log_error("RG35XX pad mapping: %s", SDL_GetError());
        } else {
            log_info("RG35XX pad mapped (%s)", guid);
        }
    }
#endif
}

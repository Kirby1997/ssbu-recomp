/* tests/port/io/test_pad.c - nnhid.c + nnpad_sdl.c on Linux. Two runs, each in a fresh process (the keyboard player and
 * the controllers.ini path are fixed once resolved, as in the game, where SSBU_PAD_SELFTEST is a run of its own):
 *   test_pad pad   nnhid_pad_selftest (boot.c SSBU_PAD_SELFTEST): SDL virtual joysticks (gamepad and raw stick) through a
 *                  controllers.ini in $TMPDIR: style, state, slots, hotplug, sticks, triggers, rumble, six-axis, LED, remap;
 *   test_pad hid desktop|gamescope   nnhid_selftest (six-axis ring, close/reopen, the synthetic XInput pad; no XInput on
 *                  Linux); without the keyboard focus pads are live under gamescope and not on a desktop (the Makefile sets
 *                  the environment for each); no key held without a window; the real pads SDL enumerates (informational:
 *                  nothing has to be attached).
 * No window and no event pump here (display.c owns it in the game): pads are read by state, keys are all up. */
#include "nnhid.c"
#include <SDL3/SDL.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("SSBU_REC", "/dev/null", 1);                   /* the pad recording (nnhid.c) is not wanted here */
    if (argc > 1 && !strcmp(argv[1], "pad")) {
        int bad = nnhid_pad_selftest();
        printf("%s (%d failed)\n", bad ? "PAD SELFTEST FAILED" : "pad selftest ok", bad);
        return bad != 0;
    }
    Ctx c;
    memset(&c, 0, sizeof c);
    int bad = nnhid_selftest(&c);
    int gamescope = argc > 2 && !strcmp(argv[2], "gamescope");
    int live = devices_live();                            /* display_window_state is not linked: focused 0 */
    printf("%s: pads live without the keyboard focus: %d (expected %d: %s)\n", gamescope ? "gamescope" : "desktop", live, gamescope,
           gamescope ? "gamescope never focuses a window Steam did not launch" : "a game in the background does not take a pad");
    bad += live != gamescope;
    int down = 0;
    for (int vk = 1; vk < 0x100; vk++) down += nnpad_key_down(vk);
    printf("keys held without a window: %d (expected 0)\n", down);
    bad += down != 0;
    int n = 0;
    SDL_UpdateJoysticks();
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    printf("real pads seen by SDL %d.%d.%d: %d\n", SDL_GetVersion() / 1000000, SDL_GetVersion() / 1000 % 1000, SDL_GetVersion() % 1000, n);
    for (int i = 0; ids && i < n; i++) {
        char g[40];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), g, sizeof g);
        printf("  \"%s\" guid %s vid %04x pid %04x %s\n", SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?", g,
               SDL_GetJoystickVendorForID(ids[i]), SDL_GetJoystickProductForID(ids[i]), SDL_IsGamepad(ids[i]) ? "gamepad" : "joystick (no mapping)");
    }
    SDL_free(ids);
    printf("%s (%d failed)\n", bad ? "HID TEST FAILED" : "hid test ok", bad);
    return bad != 0;
}

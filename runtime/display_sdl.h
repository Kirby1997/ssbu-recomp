/* display_sdl.h - the game window on non-Windows hosts (display.c, SDL3 video). Not used on _WIN32.
 *
 * display.c's window thread is SDL's "main thread" (it calls SDL_Init(SDL_INIT_VIDEO)): it creates the window and runs the
 * ONLY event pump of the process (SDL_WaitEventTimeout/SDL_PollEvent). Other SDL users (nnpad_sdl.c) must not pump or
 * flush events; they read SDL_GetKeyboardState / gamepad state, which this pump keeps current.
 * Every function here may be called from any thread; the values are copies the window thread refreshes on each event. */
#ifndef SSBU_DISPLAY_SDL_H
#define SSBU_DISPLAY_SDL_H
#ifndef _WIN32
typedef struct SDL_Window SDL_Window;
SDL_Window *display_sdl_window(void);                       /* NULL until the window exists (display_init returns after it does) */
void display_window_state(int *focused, int *minimised);    /* focused: the window has the keyboard (SDL_WINDOW_INPUT_FOCUS); before the window exists: 1, 0 */
void display_window_pixels(int *w, int *h);                 /* drawable size in pixels (SDL_GetWindowSizeInPixels, kept by the window thread); 0, 0 before */
#endif
#endif

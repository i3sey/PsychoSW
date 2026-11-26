#ifndef RECOMP_LAUNCHER_UI_H
#define RECOMP_LAUNCHER_UI_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void recomp_sdl_adopt_window(SDL_Window* win, SDL_GLContext gl_ctx, int w, int h);
void recomp_sdl_shutdown_rumble(void);
void recomp_sdl_shutdown_sensors(void);
void recomp_sdl_notify_focus(bool focused);
void recomp_sdl_notify_operation_mode(void);
void recomp_gl_set_backbuffer_scale(int guest_w, int guest_h, int host_w, int host_h);
void recomp_resolve_target_resolution(int* out_w, int* out_h);
void recomp_sync_display_settings_ini(const char* game_dir, int target_w, int target_h);
void recomp_openal_set_suspended(bool suspended);
void recomp_time_notify_pause(bool paused);

/* Shows the pre-game settings launcher (unless RECOMP_SKIP_LAUNCHER=1).
 * Creates and hands over the persistent 1280x720 GL window to hle_sdl12. */
void launcher_ui_run(const char* game_dir);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_LAUNCHER_UI_H */

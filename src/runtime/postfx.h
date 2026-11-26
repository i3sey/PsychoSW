#ifndef RECOMP_POSTFX_H
#define RECOMP_POSTFX_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs optional post-process FXAA, FPS overlay, and In-Game Quick Menu right before SwapWindow. */
void recomp_postfx_on_swap(int win_w, int win_h);
void recomp_postfx_draw_loading(int win_w, int win_h, const char* stage, const char* hint, float progress);
void recomp_postfx_shutdown(void);

/* In-game Quick Menu overlay state & controls */
bool recomp_overlay_is_open(void);
void recomp_overlay_set_open(bool open);
void recomp_overlay_toggle(void);
/* Returns true if swap interval or rumble state should be refreshed immediately */
bool recomp_overlay_handle_input(int move_y, int move_x, bool confirm, bool cancel);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_POSTFX_H */

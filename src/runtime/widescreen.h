#ifndef RECOMP_WIDESCREEN_H
#define RECOMP_WIDESCREEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compute virtual 480p widescreen projection metrics from current window dimensions.
 * Returns false if widescreen is disabled or window aspect <= 4:3.
 */
bool recomp_widescreen_metrics(float* out_aspect, float* out_ortho_w, float* out_hud_offset, float* out_hud_scale);

/**
 * Push/restore horizontal HUD anchor mode (-1 = left edge, 0 = center, +1 = right edge).
 * Returns the previous anchor value.
 */
int recomp_widescreen_set_hud_edge(int edge);

/**
 * Adjust default ESpriteManager::DrawAll 2D ortho (pos, size) from 640x480 to widescreen
 * (hud_offset, 0) / (480*aspect, 480) so 2D sprites, icons, and fonts preserve 1:1 circles.
 */
void recomp_widescreen_apply_ortho(uint32_t pos_vec2_addr, uint32_t size_vec2_addr);

/**
 * Horizontal cull test replacing hardcoded 0..640 bounds in ESpriteManager::AddSprite.
 * Widens bounds during active gameplay/cutscenes while preserving 0..640 inside menus.
 */
bool recomp_widescreen_cull_x(float x, float w);

/**
 * Expand fullscreen quads (fades/flashes/psychedelic overlays) across widescreen pillars
 * and shift edge-anchored HUD quads horizontally by g_hud_edge * hud_offset.
 */
void recomp_widescreen_adjust_sprite(uint32_t spr_addr, uint32_t tex_id, uint32_t scale_ptr);

/**
 * Pillarbox / letterbox Bink video blits inside GameApp::playVideoBink to preserve video aspect.
 */
void recomp_widescreen_adjust_bink_quad(uint32_t bink_ptr, uint32_t pos_vec2_addr, uint32_t scale_vec2_addr);

/**
 * Remap ECamera::WorldCoordsToScreen virtual 640x480 X into expanded widescreen ortho space.
 */
void recomp_widescreen_world_to_screen_x(uint32_t out_vec3_addr);

/**
 * Inverse-remap widescreen virtual X back to 4:3 X for ECamera::ScreenCoordsToWorld.
 */
void recomp_widescreen_screen_to_world_x(uint32_t x_float_addr);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_WIDESCREEN_H */

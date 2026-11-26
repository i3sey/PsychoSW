#include "runtime/widescreen.h"
#include "runtime/cpu.h"
#include "runtime/settings.h"

#include <math.h>
#include <stddef.h>

extern void recomp_sdl_get_window_size(int* out_w, int* out_h);

static int g_hud_edge = 0;

bool recomp_widescreen_metrics(float* out_aspect, float* out_ortho_w, float* out_hud_offset, float* out_hud_scale) {
    const RecompSettings* s = recomp_settings_get();
    if (!s || !s->widescreen) {
        return false;
    }
    int w = 0, h = 0;
    recomp_sdl_get_window_size(&w, &h);
    if (w <= 0 || h <= 0) {
        return false;
    }
    float aspect = (float)w / (float)h;
    const float base_aspect = 4.0f / 3.0f;
    if (aspect <= base_aspect + 0.01f) {
        return false;
    }
    float ortho_w = 480.0f * aspect;
    float hud_off = (ortho_w - 640.0f) * 0.5f;
    float hud_scale = aspect / base_aspect;
    if (out_aspect) *out_aspect = aspect;
    if (out_ortho_w) *out_ortho_w = ortho_w;
    if (out_hud_offset) *out_hud_offset = hud_off;
    if (out_hud_scale) *out_hud_scale = hud_scale;
    return true;
}

int recomp_widescreen_set_hud_edge(int edge) {
    int prev = g_hud_edge;
    g_hud_edge = edge;
    return prev;
}

void recomp_widescreen_apply_ortho(uint32_t pos_vec2_addr, uint32_t size_vec2_addr) {
    float ortho_w = 640.0f;
    float hud_off = 0.0f;
    if (!recomp_widescreen_metrics(NULL, &ortho_w, &hud_off, NULL)) {
        return;
    }
    mem_wf32(pos_vec2_addr, hud_off);
    mem_wf32(pos_vec2_addr + 4u, 0.0f);
    mem_wf32(size_vec2_addr, ortho_w);
    mem_wf32(size_vec2_addr + 4u, 480.0f);
}

bool recomp_widescreen_cull_x(float x, float w) {
    float min_bound = 0.0f;
    float max_bound = 640.0f;
    float hud_off = 0.0f;
    if (recomp_widescreen_metrics(NULL, NULL, &hud_off, NULL)) {
        uint32_t app = mem_r32(0x86aeee0u);
        bool in_menu = (app != 0u) && (mem_r8(app + 0xb4u) != 0 || mem_r8(app + 0xb5u) != 0);
        if (!in_menu && g_hud_edge == 0) {
            min_bound = -hud_off;
            max_bound = 640.0f + hud_off;
        }
    }
    return (x >= max_bound) || ((x + w) <= min_bound);
}

static bool vec2_close(float sx, float sy, float tx, float ty) {
    return fabsf(sx - tx) < 0.05f && fabsf(sy - ty) < 0.05f;
}

void recomp_widescreen_adjust_sprite(uint32_t spr_addr, uint32_t tex_id, uint32_t scale_ptr) {
    if (!spr_addr) {
        return;
    }
    float hud_off = 0.0f;
    float hud_scale = 1.0f;
    if (!recomp_widescreen_metrics(NULL, NULL, &hud_off, &hud_scale)) {
        return;
    }

    static const uint32_t x_offs[4] = { 0x24u, 0x2cu, 0x34u, 0x3cu };
    static const uint32_t y_offs[4] = { 0x28u, 0x30u, 0x38u, 0x40u };

    float min_x = mem_rf32(spr_addr + x_offs[0]);
    float max_x = min_x;
    float min_y = mem_rf32(spr_addr + y_offs[0]);
    float max_y = min_y;
    for (int i = 1; i < 4; i++) {
        float vx = mem_rf32(spr_addr + x_offs[i]);
        float vy = mem_rf32(spr_addr + y_offs[i]);
        if (vx < min_x) min_x = vx;
        if (vx > max_x) max_x = vx;
        if (vy < min_y) min_y = vy;
        if (vy > max_y) max_y = vy;
    }

    bool spans_640x480 = (fabsf(min_x) < 0.5f && fabsf(max_x - 640.0f) < 0.5f &&
                          fabsf(min_y) < 0.5f && fabsf(max_y - 480.0f) < 0.5f);

    uint32_t app = mem_r32(0x86aeee0u);
    uint32_t white_tex = (app != 0u) ? mem_r32(app + 0x8128u) : 0u;
    uint32_t psyche_tex = (app != 0u) ? mem_r32(app + 0x812cu) : 0u;

    bool matching_fs_scale = false;
    if (scale_ptr != 0u) {
        float sx = mem_rf32(scale_ptr);
        float sy = mem_rf32(scale_ptr + 4u);
        matching_fs_scale = vec2_close(sx, sy, 640.0f, 480.0f) ||
                            vec2_close(sx, sy, 20.0f, 15.0f) ||
                            vec2_close(sx, sy, 2.5f, 1.875f);
    }

    if (spans_640x480 && (tex_id == white_tex || tex_id == psyche_tex || matching_fs_scale)) {
        for (int i = 0; i < 4; i++) {
            float vx = mem_rf32(spr_addr + x_offs[i]);
            mem_wf32(spr_addr + x_offs[i], vx * hud_scale - hud_off);
        }
        return;
    }

    if (g_hud_edge != 0) {
        float dx = (float)g_hud_edge * hud_off;
        for (int i = 0; i < 4; i++) {
            float vx = mem_rf32(spr_addr + x_offs[i]);
            mem_wf32(spr_addr + x_offs[i], vx + dx);
        }
    }
}

void recomp_widescreen_adjust_bink_quad(uint32_t bink_ptr, uint32_t pos_vec2_addr, uint32_t scale_vec2_addr) {
    const RecompSettings* s = recomp_settings_get();
    if (!s || !s->widescreen || !bink_ptr) {
        return;
    }
    uint32_t vw = mem_r32(bink_ptr);
    uint32_t vh = mem_r32(bink_ptr + 4u);
    int sw = 0, sh = 0;
    recomp_sdl_get_window_size(&sw, &sh);
    if (!vw || !vh || sw <= 0 || sh <= 0) {
        return;
    }

    float v_asp = (float)vw / (float)vh;
    float s_asp = (float)sw / (float)sh;
    float sx = 1.0f;
    float sy = 1.0f;
    if (s_asp > v_asp + 0.001f) {
        sx = v_asp / s_asp;
    } else if (v_asp > s_asp + 0.001f) {
        sy = s_asp / v_asp;
    }

    mem_wf32(scale_vec2_addr, sx);
    mem_wf32(scale_vec2_addr + 4u, sy);
    mem_wf32(pos_vec2_addr, (float)sw * (1.0f - sx) * 0.5f);
    mem_wf32(pos_vec2_addr + 4u, (float)sh * (1.0f - sy) * 0.5f);
}

void recomp_widescreen_world_to_screen_x(uint32_t out_vec3_addr) {
    float hud_off = 0.0f;
    float hud_scale = 1.0f;
    if (!out_vec3_addr || !recomp_widescreen_metrics(NULL, NULL, &hud_off, &hud_scale)) {
        return;
    }
    float x = mem_rf32(out_vec3_addr);
    mem_wf32(out_vec3_addr, x * hud_scale - hud_off);
}

void recomp_widescreen_screen_to_world_x(uint32_t x_float_addr) {
    float hud_off = 0.0f;
    float hud_scale = 1.0f;
    if (!x_float_addr || !recomp_widescreen_metrics(NULL, NULL, &hud_off, &hud_scale)) {
        return;
    }
    float x = mem_rf32(x_float_addr);
    mem_wf32(x_float_addr, (x + hud_off) / hud_scale);
}

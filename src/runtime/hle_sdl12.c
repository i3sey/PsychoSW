#include "runtime/cpu.h"
#include "runtime/launcher_ui.h"
#include "runtime/memory.h"
#include "runtime/postfx.h"
#include "runtime/settings.h"
#include "runtime/vfs.h"

#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <EGL/egl.h>
#include <GL/gl.h>
#include <switch.h>
#else
#include <SDL2/SDL_opengl.h>
#endif

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>

extern uint32_t recomp_gl_get_proc_address(const char* name);
extern bool recomp_gl_thread_is_active(void);
extern void recomp_gl_thread_start(SDL_Window* win, SDL_GLContext gl_ctx);
extern void recomp_gl_thread_shutdown(void);
extern void recomp_gl_thread_swap(int win_w, int win_h, uint64_t* out_pfx_ticks, uint64_t* out_swap_ticks);
extern void recomp_gl_thread_set_swap_interval(int interval);
extern void recomp_gl_thread_resize(int win_w, int win_h, int guest_w, int guest_h, int swap_interval);

static inline uint32_t arg32(CPUContext* ctx, int idx) {
    return mem_r32(ctx->esp + (uint32_t)(4 * (idx + 1)));
}

static inline const char* str32(uint32_t p) {
    return p ? (const char*)(g_mem_base + p) : NULL;
}

static SDL_Window* g_window = NULL;
static SDL_GLContext g_gl_ctx = NULL;
static uint32_t g_guest_screen = 0;
static uint32_t g_guest_vinfo = 0;
static uint32_t g_guest_pfmt = 0;
static uint32_t g_guest_modes_arr = 0;
static uint32_t g_guest_keystate = 0;
static uint32_t g_guest_err_str = 0;
static int g_win_w = 1280;
static int g_win_h = 720;
static int g_guest_w = 1280;
static int g_guest_h = 720;
static volatile bool g_op_mode_dirty = false;
#ifdef __SWITCH__
static int g_last_op_mode = -1;
#endif
static uint64_t g_sim_dock_at = 0;
static uint64_t g_sim_overlay_at = 0;
static uint64_t g_frame_count = 0;
uint64_t recomp_sdl_get_frame_count(void) { return g_frame_count; }
static uint32_t g_prev_swap_ms = 0;
static uint32_t g_ov_prev_vpad = 0;
static int      g_ov_prev_sy = 0;
static int      g_ov_prev_sx = 0;
static bool     g_ov_combo_held = false;

static void recomp_sdl_apply_swap_interval(void) {
    const RecompSettings* st = recomp_settings_get();
#ifdef __SWITCH__
    int interval = (st && st->fps_cap <= 30) ? 2 : 0;
#else
    int interval = (st && st->fps_cap <= 30) ? 2 : 1;
#endif
    recomp_gl_thread_set_swap_interval(interval);
}
static uint64_t g_max_frames = 0;
static bool g_dump_frames = false;
static uint64_t g_dump_every = 15;
static uint64_t g_dump_at[64];
static int g_dump_at_count = 0;
static char g_dump_dir[512] = "artifacts";
static bool g_sdl_env_parsed = false;
static bool g_subtitles_synced = false;
static bool g_prompts_patched = false;

void recomp_sdl_adopt_window(SDL_Window* win, SDL_GLContext gl_ctx, int w, int h) {
    g_window = win;
    g_gl_ctx = gl_ctx;
    if (w > 0) { g_win_w = w; g_guest_w = w; }
    if (h > 0) { g_win_h = h; g_guest_h = h; }
}

void recomp_sdl_get_window_size(int* out_w, int* out_h) {
    if (out_w) *out_w = g_guest_w;
    if (out_h) *out_h = g_guest_h;
}

void recomp_resolve_target_resolution(int* out_w, int* out_h) {
    const RecompSettings* st = recomp_settings_get();
    int w = 1280, h = 720;
    if (st->res_mode == RECOMP_RES_1080P) {
        w = 1920;
        h = 1080;
    } else if (st->res_mode == RECOMP_RES_720P) {
        w = 1280;
        h = 720;
    } else {
#ifdef __SWITCH__
        if (appletGetOperationMode() == AppletOperationMode_Console) {
            w = 1920;
            h = 1080;
        }
#endif
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void parse_debug_env(void) {
    if (g_sdl_env_parsed) return;
    g_sdl_env_parsed = true;
    const char* df = getenv("RECOMP_DUMP_FRAMES");
    if (df && atoi(df) > 0) g_dump_frames = true;
    const char* dd = getenv("RECOMP_DUMP_DIR");
    if (dd && *dd) snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", dd);
    const char* de = getenv("RECOMP_DUMP_EVERY");
    if (de && atoi(de) > 0) g_dump_every = (uint64_t)atoll(de);
    const char* da = getenv("RECOMP_DUMP_AT");
    if (da && *da) {
        g_dump_frames = true;
        char copy[512];
        snprintf(copy, sizeof(copy), "%s", da);
        char* save = NULL;
        for (char* tok = strtok_r(copy, ",", &save); tok && g_dump_at_count < 64; tok = strtok_r(NULL, ",", &save)) {
            uint64_t f = (uint64_t)atoll(tok);
            if (f > 0) g_dump_at[g_dump_at_count++] = f;
        }
    }
    const char* mf = getenv("RECOMP_MAX_FRAMES");
    if (mf && atoi(mf) > 0) g_max_frames = (uint64_t)atoll(mf);
    const char* sd = getenv("RECOMP_SIM_DOCK_AT");
    if (sd && atoi(sd) > 0) g_sim_dock_at = (uint64_t)atoll(sd);
    const char* so = getenv("RECOMP_SIM_OVERLAY_AT");
    if (so && atoi(so) > 0) g_sim_overlay_at = (uint64_t)atoll(so);
}

static bool should_dump_frame(uint64_t frame_idx) {
    if (!g_dump_frames) return false;
    if (g_dump_at_count > 0) {
        for (int i = 0; i < g_dump_at_count; i++) {
            if (g_dump_at[i] == frame_idx) return true;
        }
        return (g_max_frames > 0 && frame_idx == g_max_frames);
    }
    return (frame_idx == 1 || (frame_idx % g_dump_every) == 0 ||
            (g_max_frames > 0 && frame_idx == g_max_frames));
}

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif

static void dump_bmp_frame(uint64_t frame_idx) {
    typedef void (APIENTRY *PFN_glReadPixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    static PFN_glReadPixels_t pfn_read_pixels = NULL;
    static bool checked_read_pixels = false;
    if (!checked_read_pixels) {
        checked_read_pixels = true;
        pfn_read_pixels = (PFN_glReadPixels_t)SDL_GL_GetProcAddress("glReadPixels");
    }
    if (!pfn_read_pixels) return;

    int w = g_win_w, h = g_win_h;
    if (w <= 0 || h <= 0) return;
    uint8_t* pixels = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
    if (!pixels) return;
    pfn_read_pixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

    char path[1024];
    snprintf(path, sizeof(path), "%s/frame_%04llu.bmp", g_dump_dir, (unsigned long long)frame_idx);
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        snprintf(path, sizeof(path), "/tmp/pshy_frame_%04llu.bmp", (unsigned long long)frame_idx);
        fp = fopen(path, "wb");
    }
    if (fp) {
        uint32_t img_sz = (uint32_t)(w * h * 4);
        uint32_t file_sz = 54u + img_sz;
        uint8_t hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &file_sz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 32;
        memcpy(hdr + 34, &img_sz, 4);
        fwrite(hdr, 1, 54, fp);
        fwrite(pixels, 1, img_sz, fp);
        fclose(fp);
        fprintf(stderr, "[sdl] Dumped frame %llu -> %s\n", (unsigned long long)frame_idx, path);
    }
    free(pixels);
}

static void ensure_sdl_guest_structs(void) {
    if (!g_guest_pfmt) {
        g_guest_pfmt = guest_rt_alloc(64, 16);
        mem_w8(g_guest_pfmt + 4, 32);  /* BitsPerPixel */
        mem_w8(g_guest_pfmt + 5, 4);   /* BytesPerPixel */
        mem_w32(g_guest_pfmt + 12, 0x00FF0000u); /* Rmask */
        mem_w32(g_guest_pfmt + 16, 0x0000FF00u); /* Gmask */
        mem_w32(g_guest_pfmt + 20, 0x000000FFu); /* Bmask */
        mem_w32(g_guest_pfmt + 24, 0xFF000000u); /* Amask */
    }
    if (!g_guest_vinfo) {
        g_guest_vinfo = guest_rt_alloc(32, 16);
        mem_w32(g_guest_vinfo + 0x00, 0x01u); /* hw_available */
        mem_w32(g_guest_vinfo + 0x04, g_guest_pfmt);
        mem_w32(g_guest_vinfo + 0x0C, (uint32_t)g_win_w);
        mem_w32(g_guest_vinfo + 0x10, (uint32_t)g_win_h);
    }
    if (!g_guest_screen) {
        g_guest_screen = guest_rt_alloc(64, 16);
        mem_w32(g_guest_screen + 0x00, 0x80000002u); /* SDL_OPENGL */
        mem_w32(g_guest_screen + 0x04, g_guest_pfmt);
        mem_w32(g_guest_screen + 0x08, (uint32_t)g_win_w);
        mem_w32(g_guest_screen + 0x0C, (uint32_t)g_win_h);
        mem_w16(g_guest_screen + 0x10, (uint16_t)(g_win_w * 4));
    }
    if (!g_guest_modes_arr) {
        static const uint16_t modes[][2] = {
            {1920, 1080}, {1280, 720}, {1024, 768}, {800, 600}, {640, 480}
        };
        g_guest_modes_arr = guest_rt_alloc(6 * 4, 16);
        for (int i = 0; i < 5; i++) {
            uint32_t r = guest_rt_alloc(8, 4);
            mem_w16(r + 0, 0);
            mem_w16(r + 2, 0);
            mem_w16(r + 4, modes[i][0]);
            mem_w16(r + 6, modes[i][1]);
            mem_w32(g_guest_modes_arr + (uint32_t)(i * 4), r);
        }
        mem_w32(g_guest_modes_arr + 20, 0);
    }
    if (!g_guest_keystate) {
        g_guest_keystate = guest_rt_alloc(512, 16);
    }
    if (!g_guest_err_str) {
        g_guest_err_str = guest_rt_alloc(512, 16);
    }
}

/* ---------------- SDL Init / Video / GL ---------------- */

void imp_SDL_Init(CPUContext* ctx) {
    parse_debug_env();
    ensure_sdl_guest_structs();
    uint32_t f12 = arg32(ctx, 0);
    uint32_t f2 = 0;
    if (f12 & 0x01u) f2 |= SDL_INIT_TIMER;
    if (f12 & 0x10u) f2 |= SDL_INIT_AUDIO;
    if (f12 & 0x20u) f2 |= SDL_INIT_VIDEO;
    if (f12 & 0x200u) f2 |= SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
#ifdef __SWITCH__
    f2 |= SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
#endif
    if (f2 == 0) f2 = SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
    int rc = 0;
    if (!SDL_WasInit(f2)) {
        rc = SDL_InitSubSystem(f2);
    }
    ctx->eax = (uint32_t)rc;
}

void imp_SDL_QuitSubSystem(CPUContext* ctx) {
    uint32_t f12 = arg32(ctx, 0);
    uint32_t f2 = 0;
    if (f12 & 0x200u) f2 |= SDL_INIT_JOYSTICK;
    /* Keep SDL_INIT_VIDEO alive when launcher adopted window */
    if (f2) SDL_QuitSubSystem(f2);
}

void imp_SDL_Quit(CPUContext* ctx) {
    (void)ctx;
    recomp_gl_thread_shutdown();
    if (g_gl_ctx) { SDL_GL_DeleteContext(g_gl_ctx); g_gl_ctx = NULL; }
    if (g_window) { SDL_DestroyWindow(g_window); g_window = NULL; }
    SDL_Quit();
}

void imp_SDL_GetError(CPUContext* ctx) {
    ensure_sdl_guest_structs();
    const char* err = SDL_GetError();
    snprintf((char*)(g_mem_base + g_guest_err_str), 512, "%s", err ? err : "");
    ctx->eax = g_guest_err_str;
}

void imp_SDL_GL_LoadLibrary(CPUContext* ctx) {
    (void)ctx;
    ctx->eax = 0;
}

void imp_SDL_GL_SetAttribute(CPUContext* ctx) {
    uint32_t attr = arg32(ctx, 0);
    int val = (int)arg32(ctx, 1);
    int rc = 0;
    switch (attr) {
        case 0: rc = SDL_GL_SetAttribute(SDL_GL_RED_SIZE, val); break;
        case 1: rc = SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, val); break;
        case 2: rc = SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, val); break;
        case 3: rc = SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, val); break;
        case 4: rc = SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, val); break;
        case 5: rc = SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, val); break;
        case 6: rc = SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, val); break;
        case 7: rc = SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, val); break;
#ifdef __SWITCH__
        case 13: rc = SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0); break;
        case 14: rc = SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0); break;
#else
        case 13: rc = SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, val); break;
        case 14: rc = SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, val); break;
#endif
        case 16: SDL_GL_SetSwapInterval(val); rc = 0; break;
        default: rc = 0; break;
    }
    ctx->eax = (uint32_t)rc;
}

void imp_SDL_GL_GetProcAddress(CPUContext* ctx) {
    const char* name = str32(arg32(ctx, 0));
    ctx->eax = recomp_gl_get_proc_address(name);
}

static uint32_t g_boot_last_draw_ms = 0;
static int      g_boot_stage = 0;
static int      g_boot_count = 0;
static void render_boot_loading_frame(bool do_swap);

void imp_SDL_SetVideoMode(CPUContext* ctx) {
    ensure_sdl_guest_structs();
    int w = (int)arg32(ctx, 0);
    int h = (int)arg32(ctx, 1);
    int bpp = (int)arg32(ctx, 2);
    uint32_t flags = arg32(ctx, 3);
    (void)bpp;

#ifdef __SWITCH__
    recomp_resolve_target_resolution(&w, &h);
    uint32_t win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;
#else
    {
        const RecompSettings* st = recomp_settings_get();
        if (st->res_mode == RECOMP_RES_1080P) {
            w = 1920; h = 1080;
        } else if (st->res_mode == RECOMP_RES_720P) {
            w = 1280; h = 720;
        } else {
            if (w <= 0) w = 1280;
            if (h <= 0) h = 720;
        }
    }
    uint32_t win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (flags & 0x80000000u) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    (void)flags;

    if (!g_window) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        g_window = SDL_CreateWindow("Psychonauts",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    w, h, win_flags);
        if (!g_window) {
            fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
            ctx->eax = 0;
            return;
        }
        g_gl_ctx = SDL_GL_CreateContext(g_window);
        if (!g_gl_ctx) {
            fprintf(stderr, "[sdl] SDL_GL_CreateContext(2.1 compat) failed: %s, trying 3.3 compat...\n", SDL_GetError());
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            g_gl_ctx = SDL_GL_CreateContext(g_window);
        }
        if (!g_gl_ctx) {
            fprintf(stderr, "[sdl] SDL_GL_CreateContext(3.3 compat) failed: %s, trying default...\n", SDL_GetError());
            SDL_GL_ResetAttributes();
            g_gl_ctx = SDL_GL_CreateContext(g_window);
        }
        if (!g_gl_ctx) {
            fprintf(stderr, "[sdl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
            ctx->eax = 0;
            return;
        }
        SDL_GL_MakeCurrent(g_window, g_gl_ctx);
        recomp_sdl_apply_swap_interval();
    } else if (w != g_win_w || h != g_win_h) {
        SDL_SetWindowSize(g_window, w, h);
        recomp_sdl_apply_swap_interval();
#ifndef __SWITCH__
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif
    } else {
        recomp_sdl_apply_swap_interval();
    }

    g_win_w = w;
    g_win_h = h;
    g_guest_w = w;
    g_guest_h = h;
#ifdef __SWITCH__
    g_last_op_mode = (int)appletGetOperationMode();
#endif
    recomp_gl_set_backbuffer_scale(g_guest_w, g_guest_h, g_win_w, g_win_h);
    if (g_frame_count == 0) {
        g_boot_stage = 0;
        g_boot_count = 0;
        g_boot_last_draw_ms = SDL_GetTicks();
        render_boot_loading_frame(true);
    }
    mem_w32(g_guest_screen + 0x08, (uint32_t)w);
    mem_w32(g_guest_screen + 0x0C, (uint32_t)h);
    mem_w16(g_guest_screen + 0x10, (uint16_t)(w * 4));
    mem_w32(g_guest_vinfo + 0x0C, (uint32_t)w);
    mem_w32(g_guest_vinfo + 0x10, (uint32_t)h);

    ctx->eax = g_guest_screen;
}

static void apply_engine_runtime_hooks(void) {
    uint32_t app = mem_r32(0x086AEEE0u);
    if (app < 0x08048000u || app >= 0x3F000000u) return;

    const RecompSettings* st = recomp_settings_get();
    if (!g_subtitles_synced) {
        mem_w8(app + 0xAEu, st->subtitles ? 1u : 0u);
        g_subtitles_synced = true;
    }

    if (st->button_icons != RECOMP_ICONS_PC) {
        mem_w8(app + 0xDEu, 1u);
        if (!g_prompts_patched) {
            /* Pass 1 emits "%.0s%s"; Pass 2 discards PC token and prints localized joystick prompt */
            memcpy(g_mem_base + 0x0858B885u, "%%.0s%%s", 9);
            if (st->text_lang == RECOMP_TEXT_RU_BUKA) {
                memcpy(g_mem_base + 0x0858B897u, "/JoyX/", 7);
                memcpy(g_mem_base + 0x0858B8A3u, "/JoyZ/", 7);
            } else {
                memcpy(g_mem_base + 0x0858B897u, "/GLPC080TO/", 12);
                memcpy(g_mem_base + 0x0858B8A3u, "/GLPC081TO/", 12);
            }
            g_prompts_patched = true;
        }
    } else if (g_prompts_patched) {
        memcpy(g_mem_base + 0x0858B885u, "%s%%s%s%%s%s", 13);
        memcpy(g_mem_base + 0x0858B897u, "/GLPC080TO/", 12);
        memcpy(g_mem_base + 0x0858B8A3u, "/GLPC081TO/", 12);
        mem_w8(app + 0xDEu, 0u);
        g_prompts_patched = false;
    }
}

bool recomp_sdl_is_boot_loading(void) {
    return g_window != NULL && g_frame_count < 6u;
}

static void render_boot_loading_frame(bool do_swap) {
    if (!g_window || g_win_w <= 0 || g_win_h <= 0) return;
    bool en = (recomp_settings_get()->ui_lang == RECOMP_UI_EN);
    char stage_buf[128];
    float progress = 0.08f;
    if (g_boot_stage == 0) {
        snprintf(stage_buf, sizeof(stage_buf), "%s",
                 en ? "Initializing engine..." : "Инициализация движка...");
        progress = 0.08f;
    } else if (g_boot_stage == 1) {
        int capped = g_boot_count > 366 ? 366 : g_boot_count;
        snprintf(stage_buf, sizeof(stage_buf),
                 en ? "Loading textures... %d / 366" : "Загрузка текстур... %d / 366",
                 capped);
        progress = 0.10f + 0.35f * ((float)capped / 366.0f);
    } else {
        int capped = g_boot_count > 120 ? 120 : g_boot_count;
        snprintf(stage_buf, sizeof(stage_buf),
                 en ? "Compiling shaders... %d / 120" : "Компиляция шейдеров... %d / 120",
                 capped);
        progress = 0.45f + 0.50f * ((float)capped / 120.0f);
    }
    const char* hint = en
        ? "First launch may take 15-30s while building the shader cache"
        : "Первый запуск занимает 15-30 сек. (сборка кеша шейдеров)";
    recomp_postfx_draw_loading(g_win_w, g_win_h, stage_buf, hint, progress);
    if (do_swap) {
        SDL_GL_SwapWindow(g_window);
        SDL_PumpEvents();
    }
}

void recomp_sdl_notify_boot_progress(int stage, int count) {
    if (!recomp_sdl_is_boot_loading()) return;
    bool stage_changed = (stage > g_boot_stage);
    if (stage < g_boot_stage) return;
    g_boot_stage = stage;
    g_boot_count = count;
    uint32_t now = SDL_GetTicks();
    if (!stage_changed && g_boot_last_draw_ms != 0 && (now - g_boot_last_draw_ms) < 140u) {
        return;
    }
    g_boot_last_draw_ms = now;
    render_boot_loading_frame(true);
}

static void recomp_sdl_apply_operation_mode_if_needed(void);
static void recomp_sdl_update_overlay_input(void);
extern uint64_t g_recomp_icall_count;
extern void recomp_gl_prof_end_frame(uint64_t frame_num, uint64_t total_ticks,
                                     uint64_t postfx_ticks, uint64_t swap_ticks,
                                     uint64_t icalls);
extern void recomp_gl_invalidate_state_cache(void);
#ifndef __SWITCH__
extern void recomp_host_prof_frame(uint64_t frame);
extern void recomp_host_prof_dump(void);
#endif
static uint64_t g_prof_prev_frame_tick = 0;
static uint64_t g_prof_prev_icalls = 0;

static inline uint64_t sdl_prof_tick(void) {
#ifdef __SWITCH__
    return armGetSystemTick();
#else
    return SDL_GetPerformanceCounter();
#endif
}

void imp_SDL_GL_SwapBuffers(CPUContext* ctx) {
    (void)ctx;
    g_frame_count++;
#ifdef __SWITCH__
    if (g_frame_count == 6) {
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        fprintf(stderr, "[switch] Returned CPU boost mode to Normal after initial load\n");
    }
#endif
    apply_engine_runtime_hooks();
    recomp_sdl_update_overlay_input();
    if (g_frame_count == 1 || g_frame_count == 5 || g_frame_count == 30 || g_frame_count == 120 || (g_frame_count % 300) == 0) {
        fprintf(stderr, "[sdl] SwapBuffers frame=%llu (%dx%d)\n",
                (unsigned long long)g_frame_count, g_win_w, g_win_h);
        fflush(stderr);
    }
    uint64_t pfx_ticks = 0, swp_ticks = 0;
    if (g_window && recomp_gl_thread_is_active()) {
        recomp_gl_thread_swap(g_win_w, g_win_h, &pfx_ticks, &swp_ticks);
    } else {
        uint64_t t_pfx0 = sdl_prof_tick();
        if (g_window) {
            if (g_frame_count <= 4u) {
                render_boot_loading_frame(false);
                recomp_gl_invalidate_state_cache();
            } else {
                recomp_postfx_on_swap(g_win_w, g_win_h);
            }
        }
        pfx_ticks = sdl_prof_tick() - t_pfx0;
        if (should_dump_frame(g_frame_count)) {
            dump_bmp_frame(g_frame_count);
        }
        uint64_t t_swp0 = sdl_prof_tick();
        if (g_window) {
            SDL_GL_SwapWindow(g_window);
        }
        swp_ticks = sdl_prof_tick() - t_swp0;
        if (g_frame_count == 6 && g_window && g_gl_ctx && !g_dump_frames) {
            recomp_gl_thread_start(g_window, g_gl_ctx);
        }
    }
    uint64_t now_tick = sdl_prof_tick();
    if (g_prof_prev_frame_tick != 0) {
        uint64_t cur_icalls = g_recomp_icall_count;
        recomp_gl_prof_end_frame(g_frame_count, now_tick - g_prof_prev_frame_tick,
                                 pfx_ticks, swp_ticks, cur_icalls - g_prof_prev_icalls);
        g_prof_prev_icalls = cur_icalls;
    } else {
        g_prof_prev_icalls = g_recomp_icall_count;
    }
    g_prof_prev_frame_tick = now_tick;
    if (recomp_settings_get()->fps_cap <= 30) {
        uint32_t now = SDL_GetTicks();
        if (g_prev_swap_ms != 0) {
            uint32_t elapsed = now - g_prev_swap_ms;
            if (elapsed < 33u) {
                SDL_Delay(33u - elapsed);
                now = SDL_GetTicks();
            }
        }
        g_prev_swap_ms = now;
    } else {
        g_prev_swap_ms = 0;
    }
    recomp_sdl_apply_operation_mode_if_needed();
#ifndef __SWITCH__
    recomp_host_prof_frame(g_frame_count);
#endif
    if (g_max_frames > 0 && g_frame_count >= g_max_frames) {
#ifndef __SWITCH__
        recomp_host_prof_dump();
#endif
        fprintf(stderr, "[sdl] Reached RECOMP_MAX_FRAMES (%llu), exiting.\n",
                (unsigned long long)g_max_frames);
        fflush(stderr);
        _exit(0);
    }
}

void imp_SDL_GetVideoInfo(CPUContext* ctx) {
    ensure_sdl_guest_structs();
    ctx->eax = g_guest_vinfo;
}

void imp_SDL_ListModes(CPUContext* ctx) {
    (void)arg32(ctx, 0);
    (void)arg32(ctx, 1);
    ensure_sdl_guest_structs();
    ctx->eax = g_guest_modes_arr;
}

void imp_SDL_SetGammaRamp(CPUContext* ctx) { (void)ctx; ctx->eax = 0; }
void imp_SDL_WM_SetCaption(CPUContext* ctx) {
    const char* title = str32(arg32(ctx, 0));
    if (g_window && title) SDL_SetWindowTitle(g_window, title);
}
void imp_SDL_WM_SetIcon(CPUContext* ctx) { (void)ctx; }
void imp_SDL_ShowCursor(CPUContext* ctx) { ctx->eax = (uint32_t)SDL_ShowCursor((int)arg32(ctx, 0)); }

void imp_SDL_WM_GrabInput(CPUContext* ctx) {
    int mode = (int)arg32(ctx, 0);
    if (g_window && mode >= 0) {
        SDL_SetRelativeMouseMode(mode ? SDL_TRUE : SDL_FALSE);
    }
    ctx->eax = (g_window && SDL_GetRelativeMouseMode()) ? 1u : 0u;
}

void imp_SDL_EnableUNICODE(CPUContext* ctx) {
    (void)arg32(ctx, 0);
    ctx->eax = 1;
}

static uint32_t g_last_raw_ticks = 0;
static uint32_t g_ticks_sleep_off = 0;
static uint32_t g_paused_ticks = 0;
static bool     g_sdl_focused = true;
static float    g_gyro_rem_x = 0.0f;
static float    g_gyro_rem_y = 0.0f;
static uint32_t g_gyro_prev_ms = 0;

static uint32_t get_comp_ticks(void) {
    uint32_t now = SDL_GetTicks();
    if (!g_sdl_focused) {
        return g_paused_ticks - g_ticks_sleep_off;
    }
    if (g_last_raw_ticks != 0) {
        uint32_t delta = now - g_last_raw_ticks;
        if (delta > 1000u) {
            g_ticks_sleep_off += (delta - 16u);
        }
    }
    g_last_raw_ticks = now;
    return now - g_ticks_sleep_off;
}

void imp_SDL_GetTicks(CPUContext* ctx) { ctx->eax = get_comp_ticks(); }
void imp_SDL_Delay(CPUContext* ctx) { SDL_Delay(arg32(ctx, 0)); }

/* ---------------- Keyboard / Mouse / Events ---------------- */

static uint32_t sdl2_key_to_sdl12(SDL_Keycode k) {
    if (k >= 0 && k < 128) return (uint32_t)k;
    switch (k) {
        case SDLK_KP_0: return 256; case SDLK_KP_1: return 257; case SDLK_KP_2: return 258;
        case SDLK_KP_3: return 259; case SDLK_KP_4: return 260; case SDLK_KP_5: return 261;
        case SDLK_KP_6: return 262; case SDLK_KP_7: return 263; case SDLK_KP_8: return 264;
        case SDLK_KP_9: return 265; case SDLK_KP_PERIOD: return 266; case SDLK_KP_DIVIDE: return 267;
        case SDLK_KP_MULTIPLY: return 268; case SDLK_KP_MINUS: return 269; case SDLK_KP_PLUS: return 270;
        case SDLK_KP_ENTER: return 271; case SDLK_KP_EQUALS: return 272;
        case SDLK_UP: return 273; case SDLK_DOWN: return 274;
        case SDLK_RIGHT: return 275; case SDLK_LEFT: return 276;
        case SDLK_INSERT: return 277; case SDLK_HOME: return 278;
        case SDLK_END: return 279; case SDLK_PAGEUP: return 280; case SDLK_PAGEDOWN: return 281;
        case SDLK_F1: return 282; case SDLK_F2: return 283; case SDLK_F3: return 284;
        case SDLK_F4: return 285; case SDLK_F5: return 286; case SDLK_F6: return 287;
        case SDLK_F7: return 288; case SDLK_F8: return 289; case SDLK_F9: return 290;
        case SDLK_F10: return 291; case SDLK_F11: return 292; case SDLK_F12: return 293;
        case SDLK_RSHIFT: return 303; case SDLK_LSHIFT: return 304;
        case SDLK_RCTRL: return 305; case SDLK_LCTRL: return 306;
        case SDLK_RALT: return 307; case SDLK_LALT: return 308;
        default: return 0;
    }
}

static inline int16_t clamp_i16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void apply_radial_deadzone(int32_t raw_x, int32_t raw_y, int16_t* out_x, int16_t* out_y) {
    int pct = recomp_settings_get()->stick_deadzone;
    if (pct < 5) pct = 5;
    if (pct > 30) pct = 30;
    const double max_v = 32767.0;
    const double dz = max_v * ((double)pct / 100.0);
    double fx = (double)raw_x;
    double fy = (double)raw_y;
    double mag = sqrt(fx * fx + fy * fy);
    if (mag <= dz || mag < 1.0) {
        *out_x = 0;
        *out_y = 0;
        return;
    }
    double norm = (mag - dz) / (max_v - dz);
    if (norm > 1.0) norm = 1.0;
    double scale = (norm * max_v) / mag;
    *out_x = clamp_i16((int32_t)llround(fx * scale));
    *out_y = clamp_i16((int32_t)llround(fy * scale));
}

#define SYNTH_KEY_CAP 64
static uint32_t g_synth_keys[SYNTH_KEY_CAP];
static int g_synth_head = 0;
static int g_synth_tail = 0;

static void synth_key_push(uint32_t sym) {
    int next = (g_synth_tail + 1) % SYNTH_KEY_CAP;
    if (next == g_synth_head) return;
    g_synth_keys[g_synth_tail] = sym;
    g_synth_tail = next;
}

static uint32_t synth_key_pop(void) {
    if (g_synth_head == g_synth_tail) return 0u;
    uint32_t sym = g_synth_keys[g_synth_head];
    g_synth_head = (g_synth_head + 1) % SYNTH_KEY_CAP;
    return sym;
}

/* Unified virtual button mask bits (used on both Switch & host RECOMP_SIM_GAMEPAD=1) */
enum {
    VPAD_A      = 1u << 0,
    VPAD_B      = 1u << 1,
    VPAD_X      = 1u << 2,
    VPAD_Y      = 1u << 3,
    VPAD_L      = 1u << 4,
    VPAD_R      = 1u << 5,
    VPAD_ZL     = 1u << 6,
    VPAD_ZR     = 1u << 7,
    VPAD_PLUS   = 1u << 8,
    VPAD_MINUS  = 1u << 9,
    VPAD_L3     = 1u << 10,
    VPAD_R3     = 1u << 11,
    VPAD_UP     = 1u << 12,
    VPAD_RIGHT  = 1u << 13,
    VPAD_DOWN   = 1u << 14,
    VPAD_LEFT   = 1u << 15,
};

#ifdef __SWITCH__
static PadState g_nx_pad;
static bool g_nx_pad_inited = false;
static HidVibrationDeviceHandle g_vib_handles[4];
static int g_vib_count = 0;
static bool g_vib_inited = false;
static uint32_t g_vib_prev_key = 0xFFFFFFFFu;
static HidSixAxisSensorHandle g_six_handheld;
static HidSixAxisSensorHandle g_six_fullkey;
static HidSixAxisSensorHandle g_six_joydual[2];
static bool g_six_has_handheld = false;
static bool g_six_has_fullkey = false;
static bool g_six_has_joydual = false;
static bool g_six_inited = false;

static void switch_init_vibration(void) {
    if (g_vib_inited) return;
    g_vib_inited = true;
    HidVibrationDeviceHandle h[2];
    if (R_SUCCEEDED(hidInitializeVibrationDevices(h, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
        g_vib_handles[g_vib_count++] = h[0];
        g_vib_handles[g_vib_count++] = h[1];
    }
    if (R_SUCCEEDED(hidInitializeVibrationDevices(h, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey))) {
        g_vib_handles[g_vib_count++] = h[0];
        g_vib_handles[g_vib_count++] = h[1];
    }
    fprintf(stderr, "[sdl-switch] Initialized %d HD rumble handles\n", g_vib_count);
}

static void switch_init_sixaxis(void) {
    if (g_six_inited) return;
    g_six_inited = true;
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_six_handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
        hidStartSixAxisSensor(g_six_handheld);
        g_six_has_handheld = true;
    }
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_six_fullkey, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey))) {
        hidStartSixAxisSensor(g_six_fullkey);
        g_six_has_fullkey = true;
    }
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(g_six_joydual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual))) {
        hidStartSixAxisSensor(g_six_joydual[0]);
        hidStartSixAxisSensor(g_six_joydual[1]);
        g_six_has_joydual = true;
    }
    fprintf(stderr, "[sdl-switch] Initialized SixAxis sensors (handheld=%d fullkey=%d joydual=%d)\n",
            (int)g_six_has_handheld, (int)g_six_has_fullkey, (int)g_six_has_joydual);
}

static bool switch_read_sixaxis(float* out_vx, float* out_vy, float* out_vz) {
    switch_init_sixaxis();
    u32 styles = padGetStyleSet(&g_nx_pad);
    HidSixAxisSensorState st;
    memset(&st, 0, sizeof(st));
    size_t n = 0;
    if ((styles & HidNpadStyleTag_NpadHandheld) && g_six_has_handheld) {
        n = hidGetSixAxisSensorStates(g_six_handheld, &st, 1);
    } else if ((styles & HidNpadStyleTag_NpadJoyDual) && g_six_has_joydual) {
        n = hidGetSixAxisSensorStates(g_six_joydual[1], &st, 1);
    } else if (g_six_has_fullkey) {
        n = hidGetSixAxisSensorStates(g_six_fullkey, &st, 1);
    } else if (g_six_has_handheld) {
        n = hidGetSixAxisSensorStates(g_six_handheld, &st, 1);
    }
    if (n == 0) return false;
    *out_vx = st.angular_velocity.x;
    *out_vy = st.angular_velocity.y;
    *out_vz = st.angular_velocity.z;
    return true;
}

static void switch_update_nx_pad(void) {
    if (!g_nx_pad_inited) {
        padInitializeAny(&g_nx_pad);
        g_nx_pad_inited = true;
        fprintf(stderr, "[sdl-switch] Initialized libnx PadState (PAD_ANY_ID_MASK=0x1000100FF)\n");
    }
    padUpdate(&g_nx_pad);
}

static uint32_t switch_read_vpad_buttons(void) {
    switch_update_nx_pad();
    u64 k = padGetButtons(&g_nx_pad);
    uint32_t m = 0;
    if (k & HidNpadButton_A)      m |= VPAD_A;
    if (k & HidNpadButton_B)      m |= VPAD_B;
    if (k & HidNpadButton_X)      m |= VPAD_X;
    if (k & HidNpadButton_Y)      m |= VPAD_Y;
    if (k & HidNpadButton_L)      m |= VPAD_L;
    if (k & HidNpadButton_R)      m |= VPAD_R;
    if (k & HidNpadButton_ZL)     m |= VPAD_ZL;
    if (k & HidNpadButton_ZR)     m |= VPAD_ZR;
    if (k & HidNpadButton_Plus)   m |= VPAD_PLUS;
    if (k & HidNpadButton_Minus)  m |= VPAD_MINUS;
    if (k & HidNpadButton_StickL) m |= VPAD_L3;
    if (k & HidNpadButton_StickR) m |= VPAD_R3;
    if (k & HidNpadButton_Up)     m |= VPAD_UP;
    if (k & HidNpadButton_Right)  m |= VPAD_RIGHT;
    if (k & HidNpadButton_Down)   m |= VPAD_DOWN;
    if (k & HidNpadButton_Left)   m |= VPAD_LEFT;
    return m;
}
#else
typedef struct SimInputEvent {
    uint64_t f_start;
    uint64_t f_end;
    char tok[16];
} SimInputEvent;

static bool g_sim_inited = false;
static bool g_sim_enabled = false;
static SimInputEvent g_sim_events[128];
static int g_sim_count = 0;

static void init_sim_gamepad(void) {
    if (g_sim_inited) return;
    g_sim_inited = true;
    const char* sim = getenv("RECOMP_SIM_GAMEPAD");
    if (!sim || atoi(sim) <= 0) return;
    g_sim_enabled = true;
    const char* scr = getenv("RECOMP_INPUT_SCRIPT");
    if (!scr || !*scr) return;
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", scr);
    char* save = NULL;
    for (char* it = strtok_r(copy, ",", &save); it && g_sim_count < 128; it = strtok_r(NULL, ",", &save)) {
        uint64_t a = 0, b = 0;
        char t[16] = {0};
        if (sscanf(it, "%llu-%llu:%15s", (unsigned long long*)&a, (unsigned long long*)&b, t) == 3) {
            g_sim_events[g_sim_count].f_start = a;
            g_sim_events[g_sim_count].f_end = b;
            snprintf(g_sim_events[g_sim_count].tok, sizeof(g_sim_events[g_sim_count].tok), "%s", t);
            g_sim_count++;
        } else if (sscanf(it, "%llu:%15s", (unsigned long long*)&a, t) == 2) {
            g_sim_events[g_sim_count].f_start = a;
            g_sim_events[g_sim_count].f_end = a + 3u;
            snprintf(g_sim_events[g_sim_count].tok, sizeof(g_sim_events[g_sim_count].tok), "%s", t);
            g_sim_count++;
        }
    }
    fprintf(stderr, "[sdl-sim] Virtual Switch controller active (%d scripted actions)\n", g_sim_count);
}

static void sim_read_vpad(uint32_t* out_mask, int16_t* lx, int16_t* ly, int16_t* rx, int16_t* ry) {
    init_sim_gamepad();
    uint32_t m = 0;
    int32_t ax = 0, ay = 0, cx = 0, cy = 0;
    uint64_t f = g_frame_count;
    for (int i = 0; i < g_sim_count; i++) {
        if (f < g_sim_events[i].f_start || f > g_sim_events[i].f_end) continue;
        const char* t = g_sim_events[i].tok;
        if (strcasecmp(t, "A") == 0)          m |= VPAD_A;
        else if (strcasecmp(t, "B") == 0)     m |= VPAD_B;
        else if (strcasecmp(t, "X") == 0)     m |= VPAD_X;
        else if (strcasecmp(t, "Y") == 0)     m |= VPAD_Y;
        else if (strcasecmp(t, "L") == 0)     m |= VPAD_L;
        else if (strcasecmp(t, "R") == 0)     m |= VPAD_R;
        else if (strcasecmp(t, "ZL") == 0)    m |= VPAD_ZL;
        else if (strcasecmp(t, "ZR") == 0)    m |= VPAD_ZR;
        else if (strcasecmp(t, "PLUS") == 0)  m |= VPAD_PLUS;
        else if (strcasecmp(t, "MINUS") == 0) m |= VPAD_MINUS;
        else if (strcasecmp(t, "L3") == 0)    m |= VPAD_L3;
        else if (strcasecmp(t, "R3") == 0)    m |= VPAD_R3;
        else if (strcasecmp(t, "UP") == 0)    m |= VPAD_UP;
        else if (strcasecmp(t, "DOWN") == 0)  m |= VPAD_DOWN;
        else if (strcasecmp(t, "LEFT") == 0)  m |= VPAD_LEFT;
        else if (strcasecmp(t, "RIGHT") == 0) m |= VPAD_RIGHT;
        else if (strcasecmp(t, "LX+") == 0)   ax = 32767;
        else if (strcasecmp(t, "LX-") == 0)   ax = -32767;
        else if (strcasecmp(t, "LY+") == 0)   ay = -32767; /* SDL forward up = negative Y */
        else if (strcasecmp(t, "LY-") == 0)   ay = 32767;
        else if (strcasecmp(t, "RX+") == 0)   cx = 32767;
        else if (strcasecmp(t, "RX-") == 0)   cx = -32767;
        else if (strcasecmp(t, "RY+") == 0)   cy = -32767;
        else if (strcasecmp(t, "RY-") == 0)   cy = 32767;
    }
    if (out_mask) *out_mask = m;
    if (lx) *lx = clamp_i16(ax);
    if (ly) *ly = clamp_i16(ay);
    if (rx) *rx = clamp_i16(cx);
    if (ry) *ry = clamp_i16(cy);
}

static bool sim_read_gyro(float* out_vx, float* out_vy, float* out_vz) {
    const char* env = getenv("RECOMP_SIM_GYRO");
    if (!env || !*env) return false;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    if (sscanf(env, "%f,%f,%f", &vx, &vy, &vz) >= 2) {
        *out_vx = vx;
        *out_vy = vy;
        *out_vz = vz;
        return true;
    }
    return false;
}
#endif

static uint32_t current_vpad_mask(void) {
#ifdef __SWITCH__
    return switch_read_vpad_buttons();
#else
    uint32_t m = 0;
    sim_read_vpad(&m, NULL, NULL, NULL, NULL);
    return m;
#endif
}

static bool is_gyro_active_now(RecompGyroMode gmode, uint32_t vpad_mask) {
    if (gmode == RECOMP_GYRO_OFF) return false;
    uint32_t app = mem_r32(0x086AEEE0u);
    if (app < 0x08048000u || app >= 0x3F000000u) return false;
    uint32_t cam = mem_r32(app + 0x8D1Cu);
    if (cam < 0x08048000u || cam >= 0x3F000000u) return false;
    if (gmode == RECOMP_GYRO_ALWAYS) {
        uint32_t domain = mem_r32(app + 0x240u);
        return domain != 0u;
    }
    /* AIM_* modes: active in First-Person/FreeLook (ECamera + 0x908 == 2) or while Lock-On/PSI trigger is held */
    if (mem_r32(cam + 0x908u) == 2u) return true;
    if (vpad_mask & (VPAD_L | VPAD_R | VPAD_ZL | VPAD_ZR)) return true;
    return false;
}

static void apply_gyro_to_mouse_delta(int* io_dx, int* io_dy) {
    const RecompSettings* st = recomp_settings_get();
    RecompGyroMode gmode = st->gyro_mode;
    if (gmode == RECOMP_GYRO_OFF || !g_sdl_focused || recomp_overlay_is_open()) {
        g_gyro_rem_x = 0.0f;
        g_gyro_rem_y = 0.0f;
        g_gyro_prev_ms = 0;
        return;
    }

    uint32_t vm = current_vpad_mask();
    if (!is_gyro_active_now(gmode, vm)) {
        g_gyro_rem_x = 0.0f;
        g_gyro_rem_y = 0.0f;
        g_gyro_prev_ms = get_comp_ticks();
        return;
    }

    float avx = 0.0f, avy = 0.0f, avz = 0.0f;
    bool got = false;
#ifdef __SWITCH__
    got = switch_read_sixaxis(&avx, &avy, &avz);
#else
    got = sim_read_gyro(&avx, &avy, &avz);
#endif
    if (!got) return;

    static bool s_logged_once = false;
    if (!s_logged_once) {
        s_logged_once = true;
        fprintf(stderr, "[gyro] SixAxis aiming active (mode=%d)\n", (int)gmode);
    }

    uint32_t now_ms = get_comp_ticks();
    float dt = (g_gyro_prev_ms > 0) ? ((float)(now_ms - g_gyro_prev_ms) / 1000.0f) : (1.0f / 60.0f);
    g_gyro_prev_ms = now_ms;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.05f)  dt = 0.05f;

    float mag = sqrtf(avx * avx + avy * avy + avz * avz);
    if (mag < 0.006f) {
        return;
    }

    float sens = 1450.0f;
    switch (gmode) {
        case RECOMP_GYRO_AIM_LOW:  sens = 900.0f;  break;
        case RECOMP_GYRO_AIM_HIGH: sens = 2100.0f; break;
        case RECOMP_GYRO_ALWAYS:   sens = 1350.0f; break;
        default:                   sens = 1450.0f; break;
    }

    /* Switch angular_velocity: +y is counter-clockwise yaw (left), -z contributes in handheld tilt, +x pitches nose up */
    float yaw_rate   = -avy - 0.35f * avz;
    float pitch_rate = -avx;
    if (st->invert_cam_x) yaw_rate = -yaw_rate;
    if (st->invert_cam_y) pitch_rate = -pitch_rate;

    g_gyro_rem_x += yaw_rate * sens * dt;
    g_gyro_rem_y += pitch_rate * sens * dt;

    int gdx = (int)g_gyro_rem_x;
    int gdy = (int)g_gyro_rem_y;
    g_gyro_rem_x -= (float)gdx;
    g_gyro_rem_y -= (float)gdy;

    *io_dx += gdx;
    *io_dy += gdy;

    const char* glog = getenv("RECOMP_GYRO_LOG");
    if (glog && atoi(glog) > 0 && (gdx != 0 || gdy != 0)) {
        fprintf(stderr, "[gyro] frame=%llu mode=%d gdx=%d gdy=%d\n",
                (unsigned long long)g_frame_count, (int)gmode, gdx, gdy);
    }
}

static bool is_virtual_nx_active(void) {
#ifdef __SWITCH__
    return true;
#else
    init_sim_gamepad();
    return g_sim_enabled;
#endif
}

static void process_vpad_keyboard_shortcuts(void) {
    if (!is_virtual_nx_active() || recomp_overlay_is_open()) return;
    static uint32_t s_prev_vpad = 0;
    uint32_t cur = 0;
#ifdef __SWITCH__
    cur = switch_read_vpad_buttons();
#else
    sim_read_vpad(&cur, NULL, NULL, NULL, NULL);
#endif
    uint32_t rose = cur & ~s_prev_vpad;
    s_prev_vpad = cur;

    if (rose & VPAD_PLUS)  synth_key_push(13u); /* SDLK_RETURN */
    if (rose & VPAD_MINUS) synth_key_push(27u); /* SDLK_ESCAPE */

    /* Smart EKeyboardUI shortcuts (Profile Name entry: 0x086A86A8 == 0x0838D680) */
    uint32_t kb_cb = mem_r32(0x086A86A8u);
    if (kb_cb == 0x0838D680u) {
        if (rose & VPAD_B) synth_key_push(13u);
        if (rose & VPAD_A) synth_key_push(27u);
#ifdef __SWITCH__
        if (rose & VPAD_X) {
            SwkbdConfig kbd;
            char out_name[32] = {0};
            if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
                swkbdConfigMakePresetDefault(&kbd);
                bool en = (recomp_settings_get()->ui_lang == RECOMP_UI_EN);
                swkbdConfigSetHeaderText(&kbd, en ? "Profile Name" : "Имя профиля");
                swkbdConfigSetGuideText(&kbd, en ? "Enter name in Latin letters (up to 12 chars)"
                                                 : "Введите имя латиницей (до 12 символов)");
                swkbdConfigSetInitialText(&kbd, "Raz");
                swkbdConfigSetStringLenMax(&kbd, 12);
                Result s_rc = swkbdShow(&kbd, out_name, sizeof(out_name));
                swkbdClose(&kbd);
                if (R_SUCCEEDED(s_rc) && out_name[0]) {
                    for (int i = 0; i < 16; i++) synth_key_push(8u); /* Backspace */
                    for (int i = 0; out_name[i] && i < 16; i++) {
                        unsigned char ch = (unsigned char)out_name[i];
                        if (ch >= 32 && ch < 127) synth_key_push((uint32_t)ch);
                    }
                    synth_key_push(13u); /* Return */
                }
            }
        }
#endif
    }
}

void recomp_sdl_shutdown_rumble(void) {
#ifdef __SWITCH__
    g_vib_prev_key = 0xFFFFFFFFu;
    if (g_vib_inited && g_vib_count > 0) {
        HidVibrationValue zero[4];
        memset(zero, 0, sizeof(zero));
        hidSendVibrationValues(g_vib_handles, zero, g_vib_count);
    }
#endif
}

void recomp_sdl_shutdown_sensors(void) {
#ifdef __SWITCH__
    if (g_six_has_handheld) { hidStopSixAxisSensor(g_six_handheld); g_six_has_handheld = false; }
    if (g_six_has_fullkey)  { hidStopSixAxisSensor(g_six_fullkey);  g_six_has_fullkey = false; }
    if (g_six_has_joydual) {
        hidStopSixAxisSensor(g_six_joydual[0]);
        hidStopSixAxisSensor(g_six_joydual[1]);
        g_six_has_joydual = false;
    }
#endif
}

void recomp_sdl_notify_operation_mode(void) {
    g_op_mode_dirty = true;
}

static void recomp_sdl_apply_operation_mode_if_needed(void) {
    if (!g_window) return;
    const RecompSettings* st = recomp_settings_get();
    if (!st || st->res_mode != RECOMP_RES_AUTO) {
        g_op_mode_dirty = false;
        return;
    }

    int new_w = g_win_w;
    int new_h = g_win_h;
    bool changed = false;

#ifdef __SWITCH__
    int cur_mode = (int)appletGetOperationMode();
    if (g_op_mode_dirty || (g_last_op_mode >= 0 && cur_mode != g_last_op_mode)) {
        g_op_mode_dirty = false;
        g_last_op_mode = cur_mode;
        recomp_resolve_target_resolution(&new_w, &new_h);
        changed = (new_w != g_win_w || new_h != g_win_h);
    }
#else
    if (g_sim_dock_at > 0 && g_frame_count == g_sim_dock_at) {
        g_sim_dock_at = 0;
        if (g_win_w == 1280 && g_win_h == 720) {
            new_w = 1920; new_h = 1080;
        } else {
            new_w = 1280; new_h = 720;
        }
        changed = true;
    } else if (g_op_mode_dirty) {
        g_op_mode_dirty = false;
    }
#endif

    if (!changed) return;

    recomp_sdl_shutdown_rumble();
    g_gyro_rem_x = 0.0f;
    g_gyro_rem_y = 0.0f;
    g_gyro_prev_ms = 0;

    fprintf(stderr, "[switch] OperationMode changed -> backbuffer %dx%d -> %dx%d (guest %dx%d)\n",
            g_win_w, g_win_h, new_w, new_h, g_guest_w, g_guest_h);
    g_win_w = new_w;
    g_win_h = new_h;
    {
        int interval = (st && st->fps_cap <= 30) ? 2 : 1;
        if (recomp_gl_thread_is_active()) {
            recomp_gl_thread_resize(new_w, new_h, g_guest_w, g_guest_h, interval);
        } else {
            SDL_SetWindowSize(g_window, new_w, new_h);
            SDL_GL_SetSwapInterval(interval);
#ifndef __SWITCH__
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif
            recomp_gl_set_backbuffer_scale(g_guest_w, g_guest_h, g_win_w, g_win_h);
        }
    }
}

static void recomp_sdl_update_overlay_input(void) {
#ifndef __SWITCH__
    if (g_sim_overlay_at > 0 && g_frame_count == g_sim_overlay_at) {
        g_sim_overlay_at = 0;
        recomp_overlay_set_open(true);
    }
#endif
    uint32_t vm = current_vpad_mask();
    bool combo = ((vm & (VPAD_L | VPAD_R | VPAD_MINUS)) == (VPAD_L | VPAD_R | VPAD_MINUS));
    if (combo && !g_ov_combo_held) {
        g_ov_combo_held = true;
        recomp_overlay_toggle();
        if (recomp_overlay_is_open()) {
            recomp_sdl_shutdown_rumble();
        }
        g_ov_prev_vpad = vm;
        return;
    }
    if (!combo) {
        g_ov_combo_held = false;
    }

    if (!recomp_overlay_is_open()) {
        g_ov_prev_vpad = vm;
        g_ov_prev_sy = 0;
        g_ov_prev_sx = 0;
        return;
    }

    uint32_t rose = vm & ~g_ov_prev_vpad;
    g_ov_prev_vpad = vm;

    int move_y = 0;
    int move_x = 0;
    if (rose & VPAD_UP)    move_y = -1;
    if (rose & VPAD_DOWN)  move_y = +1;
    if (rose & VPAD_LEFT)  move_x = -1;
    if (rose & VPAD_RIGHT) move_x = +1;

#ifdef __SWITCH__
    switch_update_nx_pad();
    HidAnalogStickState ls = padGetStickPos(&g_nx_pad, 0);
    int sy = (ls.y > 16000) ? -1 : ((ls.y < -16000) ? +1 : 0);
    int sx = (ls.x > 16000) ? +1 : ((ls.x < -16000) ? -1 : 0);
    if (sy != 0 && sy != g_ov_prev_sy) move_y = sy;
    if (sx != 0 && sx != g_ov_prev_sx) move_x = sx;
    g_ov_prev_sy = sy;
    g_ov_prev_sx = sx;
#endif

    bool confirm = (rose & VPAD_B) != 0;
    bool cancel  = (rose & (VPAD_A | VPAD_MINUS)) != 0;
    if (recomp_overlay_handle_input(move_y, move_x, confirm, cancel)) {
        recomp_sdl_apply_swap_interval();
        if (!recomp_settings_get()->rumble) {
            recomp_sdl_shutdown_rumble();
        }
    }
}

void recomp_sdl_notify_focus(bool focused) {
    uint32_t now = SDL_GetTicks();
    if (!focused && g_sdl_focused) {
        g_sdl_focused = false;
        g_paused_ticks = now;
        recomp_sdl_shutdown_rumble();
        g_gyro_rem_x = 0.0f;
        g_gyro_rem_y = 0.0f;
        g_gyro_prev_ms = 0;
    } else if (focused && !g_sdl_focused) {
        g_sdl_focused = true;
        uint32_t dur = now - g_paused_ticks;
        if (dur > 16u) g_ticks_sleep_off += (dur - 16u);
        g_last_raw_ticks = now;
    }
}

void imp_XInputControllerExists(CPUContext* ctx) {
    if (!recomp_settings_get()->rumble) {
        ctx->eax = 0u;
        return;
    }
#ifdef __SWITCH__
    ctx->eax = 1u;
#else
    ctx->eax = is_virtual_nx_active() ? 1u : 0u;
#endif
}

void imp_DFXInputSetState(CPUContext* ctx) {
    if (!recomp_settings_get()->rumble) return;
    float low = mem_rf32(ctx->esp + 4u);
    float high = mem_rf32(ctx->esp + 8u);
    if (low < 0.0f) low = 0.0f;
    if (low > 1.0f) low = 1.0f;
    if (high < 0.0f) high = 0.0f;
    if (high > 1.0f) high = 1.0f;
#ifdef __SWITCH__
    switch_init_vibration();
    if (g_vib_count <= 0) return;
    uint32_t qlow = (uint32_t)(low * 100.0f);
    uint32_t qhigh = (uint32_t)(high * 100.0f);
    uint32_t key = (qlow << 16) | qhigh;
    if (key == g_vib_prev_key) return;
    g_vib_prev_key = key;
    HidVibrationValue vals[4];
    for (int i = 0; i < g_vib_count; i++) {
        /* Even handle = left (low motor), odd = right (high motor) */
        vals[i].amp_low   = (i % 2 == 0) ? low * 0.85f : low * 0.35f;
        vals[i].freq_low  = 160.0f;
        vals[i].amp_high  = (i % 2 == 1) ? high * 0.85f : high * 0.35f;
        vals[i].freq_high = 320.0f;
    }
    hidSendVibrationValues(g_vib_handles, vals, g_vib_count);
#endif
}

void imp_SDL_GetKeyState(CPUContext* ctx) {
    ensure_sdl_guest_structs();
    uint32_t numkeys_p = arg32(ctx, 0);
    if (numkeys_p) mem_w32(numkeys_p, 323u);
    int n = 0;
    const uint8_t* ks = SDL_GetKeyboardState(&n);
    memset(g_mem_base + g_guest_keystate, 0, 323);
    if (ks) {
        /* Map common ASCII/special scancodes -> SDL 1.2 keysyms */
        for (int sc = 0; sc < n; sc++) {
            if (!ks[sc]) continue;
            SDL_Keycode kc = SDL_GetKeyFromScancode((SDL_Scancode)sc);
            uint32_t k12 = sdl2_key_to_sdl12(kc);
            if (k12 < 323) g_mem_base[g_guest_keystate + k12] = 1;
        }
    }
    if (is_virtual_nx_active() && !recomp_overlay_is_open()) {
        uint32_t vm = 0;
#ifdef __SWITCH__
        vm = switch_read_vpad_buttons();
#else
        sim_read_vpad(&vm, NULL, NULL, NULL, NULL);
#endif
        if (vm & VPAD_MINUS) g_mem_base[g_guest_keystate + 27] = 1; /* SDLK_ESCAPE */
        if (vm & VPAD_PLUS)  g_mem_base[g_guest_keystate + 13] = 1; /* SDLK_RETURN */
    }
    ctx->eax = g_guest_keystate;
}

void imp_SDL_GetRelativeMouseState(CPUContext* ctx) {
    int dx = 0, dy = 0;
    uint32_t buttons = SDL_GetRelativeMouseState(&dx, &dy);
    apply_gyro_to_mouse_delta(&dx, &dy);
    uint32_t px = arg32(ctx, 0);
    uint32_t py = arg32(ctx, 1);
    if (px) mem_w32(px, (uint32_t)dx);
    if (py) mem_w32(py, (uint32_t)dy);
    ctx->eax = buttons;
}

void imp_SDL_PollEvent(CPUContext* ctx) {
    uint32_t ev_ptr = arg32(ctx, 0);
    process_vpad_keyboard_shortcuts();
    if (ev_ptr) {
        uint32_t synth_sym = synth_key_pop();
        if (synth_sym) {
            memset(g_mem_base + ev_ptr, 0, 24);
            mem_w8(ev_ptr + 0, 2); /* SDL_KEYDOWN */
            mem_w8(ev_ptr + 3, 1); /* SDL_PRESSED */
            mem_w32(ev_ptr + 8, synth_sym);
            mem_w16(ev_ptr + 16, (synth_sym < 128) ? (uint16_t)synth_sym : 0);
            ctx->eax = 1;
            return;
        }
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (!ev_ptr) { ctx->eax = 1; return; }
        memset(g_mem_base + ev_ptr, 0, 24);
        if (ev.type == SDL_QUIT) {
            mem_w8(ev_ptr + 0, 12); /* SDL_QUIT */
            ctx->eax = 1;
            return;
        }
        if (ev.type == SDL_WINDOWEVENT) {
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                mem_w8(ev_ptr + 0, 1); /* SDL_ACTIVEEVENT */
                mem_w8(ev_ptr + 1, (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) ? 1 : 0);
                mem_w8(ev_ptr + 2, 2); /* SDL_APPINPUTFOCUS */
                ctx->eax = 1;
                return;
            }
            continue;
        }
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F2) {
                recomp_overlay_toggle();
                continue;
            }
            if (recomp_overlay_is_open()) {
                if (ev.type == SDL_KEYDOWN) {
                    int my = 0, mx = 0;
                    bool conf = false, canc = false;
                    switch (ev.key.keysym.sym) {
                        case SDLK_UP:     case SDLK_w: my = -1; break;
                        case SDLK_DOWN:   case SDLK_s: my = +1; break;
                        case SDLK_LEFT:   case SDLK_a: mx = -1; break;
                        case SDLK_RIGHT:  case SDLK_d: mx = +1; break;
                        case SDLK_RETURN: case SDLK_SPACE: conf = true; break;
                        case SDLK_ESCAPE: canc = true; break;
                        default: break;
                    }
                    if (recomp_overlay_handle_input(my, mx, conf, canc)) {
                        recomp_sdl_apply_swap_interval();
                        if (!recomp_settings_get()->rumble) {
                            recomp_sdl_shutdown_rumble();
                        }
                    }
                }
                continue;
            }
            uint32_t sym12 = sdl2_key_to_sdl12(ev.key.keysym.sym);
            mem_w8(ev_ptr + 0, (ev.type == SDL_KEYDOWN) ? 2 : 3);
            mem_w8(ev_ptr + 2, 0);
            mem_w8(ev_ptr + 3, (ev.type == SDL_KEYDOWN) ? 1 : 0);
            mem_w8(ev_ptr + 4, (uint8_t)ev.key.keysym.scancode);
            mem_w32(ev_ptr + 8, sym12);
            uint32_t mod12 = 0;
            if (ev.key.keysym.mod & KMOD_LSHIFT) mod12 |= 0x0001u;
            if (ev.key.keysym.mod & KMOD_RSHIFT) mod12 |= 0x0002u;
            if (ev.key.keysym.mod & KMOD_LCTRL)  mod12 |= 0x0040u;
            if (ev.key.keysym.mod & KMOD_RCTRL)  mod12 |= 0x0080u;
            if (ev.key.keysym.mod & KMOD_LALT)   mod12 |= 0x0100u;
            if (ev.key.keysym.mod & KMOD_RALT)   mod12 |= 0x0200u;
            mem_w32(ev_ptr + 12, mod12);
            mem_w16(ev_ptr + 16, (sym12 < 128) ? (uint16_t)sym12 : 0);
            ctx->eax = 1;
            return;
        }
    }
    ctx->eax = 0;
}

/* ---------------- Joystick / Switch & Xbox 360 Mapping ---------------- */

typedef struct JoyHandle {
    bool open;
    SDL_GameController* gc;
    SDL_Joystick* joy;
    uint32_t name_gptr;
} JoyHandle;

static JoyHandle g_joys[8];

void imp_SDL_NumJoysticks(CPUContext* ctx) {
    if (is_virtual_nx_active()) {
        ctx->eax = 1u;
        return;
    }
    ctx->eax = (uint32_t)SDL_NumJoysticks();
}

void imp_SDL_JoystickOpen(CPUContext* ctx) {
    int idx = (int)arg32(ctx, 0);
    if (idx < 0 || idx >= 8) { ctx->eax = 0; return; }
    if (idx == 0 && is_virtual_nx_active()) {
        g_joys[0].open = true;
        ctx->eax = 0x08760000u;
        return;
    }
    if (SDL_IsGameController(idx)) {
        g_joys[idx].gc = SDL_GameControllerOpen(idx);
        g_joys[idx].joy = g_joys[idx].gc ? SDL_GameControllerGetJoystick(g_joys[idx].gc) : NULL;
    } else {
        g_joys[idx].gc = NULL;
        g_joys[idx].joy = SDL_JoystickOpen(idx);
    }
    if (!g_joys[idx].joy) { ctx->eax = 0; return; }
    g_joys[idx].open = true;
    ctx->eax = 0x08760000u + (uint32_t)(idx * 4);
}

static int joy_idx_from_handle(uint32_t h) {
    if (h < 0x08760000u) return -1;
    int idx = (int)((h - 0x08760000u) >> 2);
    return (idx >= 0 && idx < 8 && g_joys[idx].open) ? idx : -1;
}

void imp_SDL_JoystickName(CPUContext* ctx) {
    int idx = (int)arg32(ctx, 0);
    if (idx < 0 || idx >= 8) { ctx->eax = 0; return; }
    if (!g_joys[idx].name_gptr) {
        const char* name = (idx == 0 && is_virtual_nx_active())
            ? "Nintendo Switch Controller"
            : (SDL_IsGameController(idx) ? "Microsoft X-Box 360 pad" : SDL_JoystickNameForIndex(idx));
        g_joys[idx].name_gptr = guest_strdup(name ? name : "Nintendo Switch Controller");
    }
    ctx->eax = g_joys[idx].name_gptr;
}

void imp_SDL_JoystickUpdate(CPUContext* ctx) {
    (void)ctx;
    SDL_JoystickUpdate();
    SDL_GameControllerUpdate();
#ifdef __SWITCH__
    switch_update_nx_pad();
#endif
}

void imp_SDL_JoystickNumAxes(CPUContext* ctx) {
    int idx = joy_idx_from_handle(arg32(ctx, 0));
    if (idx < 0) { ctx->eax = 0; return; }
    if (idx == 0 && is_virtual_nx_active()) { ctx->eax = 6u; return; }
    ctx->eax = g_joys[idx].gc ? 6u : (uint32_t)SDL_JoystickNumAxes(g_joys[idx].joy);
}

void imp_SDL_JoystickNumButtons(CPUContext* ctx) {
    int idx = joy_idx_from_handle(arg32(ctx, 0));
    if (idx < 0) { ctx->eax = 0; return; }
    if (idx == 0 && is_virtual_nx_active()) { ctx->eax = 12u; return; }
    ctx->eax = g_joys[idx].gc ? 11u : (uint32_t)SDL_JoystickNumButtons(g_joys[idx].joy);
}

void imp_SDL_JoystickNumHats(CPUContext* ctx) {
    int idx = joy_idx_from_handle(arg32(ctx, 0));
    if (idx < 0) { ctx->eax = 0; return; }
    if (idx == 0 && is_virtual_nx_active()) { ctx->eax = 1u; return; }
    ctx->eax = g_joys[idx].gc ? 1u : (uint32_t)SDL_JoystickNumHats(g_joys[idx].joy);
}

static int16_t gc_read_axis_mapped(SDL_GameController* gc, int axis) {
    int16_t sx = 0, sy = 0;
    switch (axis) {
        case 0:
            apply_radial_deadzone(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                                  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY), &sx, &sy);
            return sx;
        case 1:
            apply_radial_deadzone(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                                  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY), &sx, &sy);
            return sy;
        case 2: return (int16_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 2 - 32768);
        case 3:
            apply_radial_deadzone(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
                                  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY), &sx, &sy);
            return sx;
        case 4:
            apply_radial_deadzone(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
                                  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY), &sx, &sy);
            return sy;
        case 5: return (int16_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 2 - 32768);
        default: return 0;
    }
}

void imp_SDL_JoystickGetAxis(CPUContext* ctx) {
    if (recomp_overlay_is_open()) { ctx->eax = 0; return; }
    int idx = joy_idx_from_handle(arg32(ctx, 0));
    int axis = (int)arg32(ctx, 1);
    if (idx < 0) { ctx->eax = 0; return; }
    const RecompSettings* st = recomp_settings_get();
    if (idx == 0 && is_virtual_nx_active()) {
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
#ifdef __SWITCH__
        switch_update_nx_pad();
        HidAnalogStickState ls = padGetStickPos(&g_nx_pad, 0);
        HidAnalogStickState rs = padGetStickPos(&g_nx_pad, 1);
        apply_radial_deadzone(ls.x, -ls.y, &lx, &ly);
        apply_radial_deadzone(rs.x, -rs.y, &rx, &ry);
#else
        sim_read_vpad(NULL, &lx, &ly, &rx, &ry);
#endif
        if (st->invert_cam_x) rx = clamp_i16(-(int32_t)rx);
        if (st->invert_cam_y) ry = clamp_i16(-(int32_t)ry);
        int16_t val = 0;
        switch (axis) {
            case 0: val = lx; break;
            case 1: val = ly; break;
            case 2: val = rx; break;
            case 3: val = 0; break;
            case 4: val = 0; break;
            case 5: val = ry; break;
            default: val = 0; break;
        }
        ctx->eax = (uint32_t)(int32_t)val;
        return;
    }
    if (g_joys[idx].gc) {
        int16_t v = gc_read_axis_mapped(g_joys[idx].gc, axis);
        if (axis == 2 && st->invert_cam_x) v = clamp_i16(-(int32_t)v);
        if (axis == 5 && st->invert_cam_y) v = clamp_i16(-(int32_t)v);
        ctx->eax = (uint32_t)(int32_t)v;
    } else {
        ctx->eax = (uint32_t)(int32_t)SDL_JoystickGetAxis(g_joys[idx].joy, axis);
    }
}

void imp_SDL_JoystickGetButton(CPUContext* ctx) {
    if (recomp_overlay_is_open()) { ctx->eax = 0; return; }
    int idx = joy_idx_from_handle(arg32(ctx, 0));
    int btn = (int)arg32(ctx, 1);
    if (idx < 0) { ctx->eax = 0; return; }
    const RecompSettings* st = recomp_settings_get();
    static const SDL_GameControllerButton map[11] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_GUIDE,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK
    };
    if (idx == 0 && is_virtual_nx_active()) {
        static const uint32_t vpad_map[12] = {
            VPAD_Y,      /* 0 (0x118): Attack / Melee */
            VPAD_B,      /* 1 (0x119): Jump / Confirm / Select Bunk */
            VPAD_A,      /* 2 (0x11A): Cancel / Back */
            VPAD_X,      /* 3 (0x11B): Use / Interact */
            VPAD_L,      /* 4 (0x11C): LockOn / Float */
            VPAD_R,      /* 5 (0x11D): PsiPower1 */
            VPAD_ZL,     /* 6 (0x11E): PsiPower2 */
            VPAD_ZR,     /* 7 (0x11F): PsiPower3 */
            VPAD_PLUS,   /* 8 (0x120): Journal / Pause */
            VPAD_MINUS,  /* 9 (0x121): Powers */
            VPAD_L3,     /* 10 (0x122): Stats / Bacon */
            VPAD_R3      /* 11 (0x123): FirstPerson */
        };
        if (btn >= 0 && btn < 12) {
            uint32_t mask = vpad_map[btn];
            if (st->swap_ab) {
                if (btn == 1) mask = VPAD_A;
                else if (btn == 2) mask = VPAD_B;
            }
            uint32_t vm = 0;
#ifdef __SWITCH__
            vm = switch_read_vpad_buttons();
#else
            sim_read_vpad(&vm, NULL, NULL, NULL, NULL);
#endif
            ctx->eax = (mask && (vm & mask)) ? 1u : 0u;
            return;
        }
        ctx->eax = 0u;
        return;
    }
    if (g_joys[idx].gc) {
        int mbtn = btn;
        if (st->swap_ab) {
            if (btn == 0) mbtn = 1;
            else if (btn == 1) mbtn = 0;
        }
        ctx->eax = (mbtn >= 0 && mbtn < 11) ? (uint32_t)SDL_GameControllerGetButton(g_joys[idx].gc, map[mbtn]) : 0u;
    } else {
        ctx->eax = (uint32_t)SDL_JoystickGetButton(g_joys[idx].joy, btn);
    }
}

void imp_SDL_JoystickGetHat(CPUContext* ctx) {
    if (recomp_overlay_is_open()) { ctx->eax = 0; return; }
    int idx = joy_idx_from_handle(arg32(ctx, 0));
    int hat = (int)arg32(ctx, 1);
    if (idx < 0) { ctx->eax = 0; return; }
    if (idx == 0 && is_virtual_nx_active()) {
        uint32_t vm = 0;
#ifdef __SWITCH__
        vm = switch_read_vpad_buttons();
#else
        sim_read_vpad(&vm, NULL, NULL, NULL, NULL);
#endif
        uint8_t h = 0;
        if (vm & VPAD_UP)    h |= 0x01;
        if (vm & VPAD_RIGHT) h |= 0x02;
        if (vm & VPAD_DOWN)  h |= 0x04;
        if (vm & VPAD_LEFT)  h |= 0x08;
        ctx->eax = h;
        return;
    }
    if (g_joys[idx].gc) {
        uint8_t h = 0;
        if (SDL_GameControllerGetButton(g_joys[idx].gc, SDL_CONTROLLER_BUTTON_DPAD_UP))    h |= 0x01;
        if (SDL_GameControllerGetButton(g_joys[idx].gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) h |= 0x02;
        if (SDL_GameControllerGetButton(g_joys[idx].gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  h |= 0x04;
        if (SDL_GameControllerGetButton(g_joys[idx].gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  h |= 0x08;
        ctx->eax = h;
    } else {
        ctx->eax = (uint32_t)SDL_JoystickGetHat(g_joys[idx].joy, hat);
    }
}

/* ---------------- RWops / BMP ---------------- */

#define MAX_RW_HANDLES 64
static SDL_RWops* g_rwops[MAX_RW_HANDLES];
static pthread_mutex_t g_sdl_res_lock = PTHREAD_MUTEX_INITIALIZER;

void imp_SDL_RWFromFile(CPUContext* ctx) {
    const char* file = str32(arg32(ctx, 0));
    const char* mode = str32(arg32(ctx, 1));
    if (!file || !mode) { ctx->eax = 0; return; }
    char host_path[1024];
    bool wr = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    if (!vfs_resolve_path(file, host_path, sizeof(host_path), wr)) {
        snprintf(host_path, sizeof(host_path), "%s", file);
    }
    SDL_RWops* rw = SDL_RWFromFile(host_path, mode);
    if (!rw) { ctx->eax = 0; return; }
    pthread_mutex_lock(&g_sdl_res_lock);
    uint32_t h = 0;
    for (int i = 1; i < MAX_RW_HANDLES; i++) {
        if (!g_rwops[i]) {
            g_rwops[i] = rw;
            h = 0x08770000u + (uint32_t)(i * 4);
            break;
        }
    }
    pthread_mutex_unlock(&g_sdl_res_lock);
    if (!h) SDL_RWclose(rw);
    ctx->eax = h;
}

void imp_SDL_LoadBMP_RW(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    int freesrc = (int)arg32(ctx, 1);
    if (h < 0x08770004u) { ctx->eax = 0; return; }
    uint32_t idx = (h - 0x08770000u) >> 2;
    if (idx >= MAX_RW_HANDLES || !g_rwops[idx]) { ctx->eax = 0; return; }
    SDL_Surface* surf = SDL_LoadBMP_RW(g_rwops[idx], freesrc);
    if (freesrc) {
        pthread_mutex_lock(&g_sdl_res_lock);
        g_rwops[idx] = NULL;
        pthread_mutex_unlock(&g_sdl_res_lock);
    }
    if (surf) SDL_FreeSurface(surf);
    ctx->eax = 0; /* Only passed to SDL_WM_SetIcon which is a no-op */
}

/* ---------------- Mutexes & Threads ---------------- */

#define MAX_SDL_MUTEXES 256
#define MAX_SDL_THREADS 64

static SDL_mutex* g_sdl_mutexes[MAX_SDL_MUTEXES];

typedef struct GuestThreadLaunch {
    uint32_t fn_ptr;
    uint32_t data_ptr;
    uint32_t stack_top;
    SDL_Thread* thread;
} GuestThreadLaunch;

static GuestThreadLaunch g_sdl_threads[MAX_SDL_THREADS];

void imp_SDL_CreateMutex(CPUContext* ctx) {
    SDL_mutex* m = SDL_CreateMutex();
    if (!m) { ctx->eax = 0; return; }
    pthread_mutex_lock(&g_sdl_res_lock);
    uint32_t h = 0;
    for (int i = 1; i < MAX_SDL_MUTEXES; i++) {
        if (!g_sdl_mutexes[i]) {
            g_sdl_mutexes[i] = m;
            h = 0x08780000u + (uint32_t)(i * 4);
            break;
        }
    }
    pthread_mutex_unlock(&g_sdl_res_lock);
    ctx->eax = h;
}

void imp_SDL_DestroyMutex(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    if (h < 0x08780004u) return;
    uint32_t idx = (h - 0x08780000u) >> 2;
    if (idx < MAX_SDL_MUTEXES && g_sdl_mutexes[idx]) {
        SDL_DestroyMutex(g_sdl_mutexes[idx]);
        g_sdl_mutexes[idx] = NULL;
    }
}

void imp_SDL_mutexP(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    uint32_t idx = (h >= 0x08780004u) ? ((h - 0x08780000u) >> 2) : 0;
    ctx->eax = (idx && idx < MAX_SDL_MUTEXES && g_sdl_mutexes[idx]) ? (uint32_t)SDL_LockMutex(g_sdl_mutexes[idx]) : 0xFFFFFFFFu;
}

void imp_SDL_mutexV(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    uint32_t idx = (h >= 0x08780004u) ? ((h - 0x08780000u) >> 2) : 0;
    ctx->eax = (idx && idx < MAX_SDL_MUTEXES && g_sdl_mutexes[idx]) ? (uint32_t)SDL_UnlockMutex(g_sdl_mutexes[idx]) : 0xFFFFFFFFu;
}

static int sdl_guest_thread_entry(void* p) {
    GuestThreadLaunch* l = (GuestThreadLaunch*)p;
#ifdef __SWITCH__
    /* Pin guest background/audio threads to Core 2 (Core 0 = Main Guest, Core 1 = Async GL)
     * at prio 50: preempts Mesa/perf workers (59), yields to SDL_RunAudio (40). */
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x4))) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7);
    }
    Result prio_rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x32);
    fprintf(stderr, "[sdl-guest] fn=0x%08x core=2 prio=50 rc=0x%x\n",
            l->fn_ptr, (unsigned)prio_rc);
    fflush(stderr);
#endif
    CPUContext tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.fpu_cw = 0x037Fu;
    tctx.esp = l->stack_top - 16u;
    mem_w32(tctx.esp + 0, 0xDEAD0001u);
    mem_w32(tctx.esp + 4, l->data_ptr);
    recomp_icall(&tctx, l->fn_ptr);
    return (int)tctx.eax;
}

void imp_SDL_CreateThread(CPUContext* ctx) {
    uint32_t fn = arg32(ctx, 0);
    uint32_t data = arg32(ctx, 1);
    uint32_t stack_top = guest_alloc_stack(2u * 1024u * 1024u);
    if (!stack_top) { ctx->eax = 0; return; }
    pthread_mutex_lock(&g_sdl_res_lock);
    int slot = -1;
    for (int i = 1; i < MAX_SDL_THREADS; i++) {
        if (!g_sdl_threads[i].thread) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_sdl_res_lock);
        ctx->eax = 0;
        return;
    }
    g_sdl_threads[slot].fn_ptr = fn;
    g_sdl_threads[slot].data_ptr = data;
    g_sdl_threads[slot].stack_top = stack_top;
    g_sdl_threads[slot].thread = SDL_CreateThread(sdl_guest_thread_entry, "guest_sdl", &g_sdl_threads[slot]);
    pthread_mutex_unlock(&g_sdl_res_lock);
    ctx->eax = g_sdl_threads[slot].thread ? (0x08790000u + (uint32_t)(slot * 4)) : 0u;
}

void imp_SDL_WaitThread(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    uint32_t status_p = arg32(ctx, 1);
    if (h < 0x08790004u) return;
    uint32_t idx = (h - 0x08790000u) >> 2;
    if (idx < MAX_SDL_THREADS && g_sdl_threads[idx].thread) {
        int st = 0;
        SDL_WaitThread(g_sdl_threads[idx].thread, &st);
        g_sdl_threads[idx].thread = NULL;
        if (status_p) mem_w32(status_p, (uint32_t)st);
    }
}

#define _GNU_SOURCE
#include "runtime/launcher_ui.h"
#include "runtime/launcher_font_atlas.h"
#include "runtime/settings.h"

#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <EGL/egl.h>
#include <GL/gl.h>
#include <switch.h>
#else
#include <SDL2/SDL_opengl.h>
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static const int LAUNCH_W = 1280;
static const int LAUNCH_H = 720;

static GLuint g_font_tex = 0;

/* Dynamic GL pointers (Switch libEGL.a only resolves GL via eglGetProcAddress) */
typedef void (APIENTRY *LGL_Begin_t)(GLenum);
typedef void (APIENTRY *LGL_BindTexture_t)(GLenum, GLuint);
typedef void (APIENTRY *LGL_BlendFunc_t)(GLenum, GLenum);
typedef void (APIENTRY *LGL_Clear_t)(GLbitfield);
typedef void (APIENTRY *LGL_ClearColor_t)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (APIENTRY *LGL_Color4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *LGL_DeleteTextures_t)(GLsizei, const GLuint*);
typedef void (APIENTRY *LGL_Disable_t)(GLenum);
typedef void (APIENTRY *LGL_Enable_t)(GLenum);
typedef void (APIENTRY *LGL_End_t)(void);
typedef void (APIENTRY *LGL_GenTextures_t)(GLsizei, GLuint*);
typedef void (APIENTRY *LGL_LoadIdentity_t)(void);
typedef void (APIENTRY *LGL_MatrixMode_t)(GLenum);
typedef void (APIENTRY *LGL_Ortho_t)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (APIENTRY *LGL_ReadPixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
typedef void (APIENTRY *LGL_TexCoord2f_t)(GLfloat, GLfloat);
typedef void (APIENTRY *LGL_TexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
typedef void (APIENTRY *LGL_TexParameteri_t)(GLenum, GLenum, GLint);
typedef void (APIENTRY *LGL_Vertex2f_t)(GLfloat, GLfloat);
typedef void (APIENTRY *LGL_Viewport_t)(GLint, GLint, GLsizei, GLsizei);

static LGL_Begin_t          lglBegin = NULL;
static LGL_BindTexture_t    lglBindTexture = NULL;
static LGL_BlendFunc_t      lglBlendFunc = NULL;
static LGL_Clear_t          lglClear = NULL;
static LGL_ClearColor_t     lglClearColor = NULL;
static LGL_Color4f_t        lglColor4f = NULL;
static LGL_DeleteTextures_t lglDeleteTextures = NULL;
static LGL_Disable_t        lglDisable = NULL;
static LGL_Enable_t         lglEnable = NULL;
static LGL_End_t            lglEnd = NULL;
static LGL_GenTextures_t    lglGenTextures = NULL;
static LGL_LoadIdentity_t   lglLoadIdentity = NULL;
static LGL_MatrixMode_t     lglMatrixMode = NULL;
static LGL_Ortho_t          lglOrtho = NULL;
static LGL_ReadPixels_t     lglReadPixels = NULL;
static LGL_TexCoord2f_t     lglTexCoord2f = NULL;
static LGL_TexImage2D_t     lglTexImage2D = NULL;
static LGL_TexParameteri_t  lglTexParameteri = NULL;
static LGL_Vertex2f_t       lglVertex2f = NULL;
static LGL_Viewport_t       lglViewport = NULL;

static void* launcher_gl_proc(const char* name) {
#ifdef __SWITCH__
    void* p = (void*)eglGetProcAddress(name);
    if (!p) p = SDL_GL_GetProcAddress(name);
    return p;
#else
    return SDL_GL_GetProcAddress(name);
#endif
}

static bool load_launcher_gl_procs(void) {
    lglBegin          = (LGL_Begin_t)launcher_gl_proc("glBegin");
    lglBindTexture    = (LGL_BindTexture_t)launcher_gl_proc("glBindTexture");
    lglBlendFunc      = (LGL_BlendFunc_t)launcher_gl_proc("glBlendFunc");
    lglClear          = (LGL_Clear_t)launcher_gl_proc("glClear");
    lglClearColor     = (LGL_ClearColor_t)launcher_gl_proc("glClearColor");
    lglColor4f        = (LGL_Color4f_t)launcher_gl_proc("glColor4f");
    lglDeleteTextures = (LGL_DeleteTextures_t)launcher_gl_proc("glDeleteTextures");
    lglDisable        = (LGL_Disable_t)launcher_gl_proc("glDisable");
    lglEnable         = (LGL_Enable_t)launcher_gl_proc("glEnable");
    lglEnd            = (LGL_End_t)launcher_gl_proc("glEnd");
    lglGenTextures    = (LGL_GenTextures_t)launcher_gl_proc("glGenTextures");
    lglLoadIdentity   = (LGL_LoadIdentity_t)launcher_gl_proc("glLoadIdentity");
    lglMatrixMode     = (LGL_MatrixMode_t)launcher_gl_proc("glMatrixMode");
    lglOrtho          = (LGL_Ortho_t)launcher_gl_proc("glOrtho");
    lglReadPixels     = (LGL_ReadPixels_t)launcher_gl_proc("glReadPixels");
    lglTexCoord2f     = (LGL_TexCoord2f_t)launcher_gl_proc("glTexCoord2f");
    lglTexImage2D     = (LGL_TexImage2D_t)launcher_gl_proc("glTexImage2D");
    lglTexParameteri  = (LGL_TexParameteri_t)launcher_gl_proc("glTexParameteri");
    lglVertex2f       = (LGL_Vertex2f_t)launcher_gl_proc("glVertex2f");
    lglViewport       = (LGL_Viewport_t)launcher_gl_proc("glViewport");
#ifndef __SWITCH__
    if (!lglBegin)          lglBegin = glBegin;
    if (!lglBindTexture)    lglBindTexture = glBindTexture;
    if (!lglBlendFunc)      lglBlendFunc = glBlendFunc;
    if (!lglClear)          lglClear = glClear;
    if (!lglClearColor)     lglClearColor = glClearColor;
    if (!lglColor4f)        lglColor4f = glColor4f;
    if (!lglDeleteTextures) lglDeleteTextures = glDeleteTextures;
    if (!lglDisable)        lglDisable = glDisable;
    if (!lglEnable)         lglEnable = glEnable;
    if (!lglEnd)            lglEnd = glEnd;
    if (!lglGenTextures)    lglGenTextures = glGenTextures;
    if (!lglLoadIdentity)   lglLoadIdentity = glLoadIdentity;
    if (!lglMatrixMode)     lglMatrixMode = glMatrixMode;
    if (!lglOrtho)          lglOrtho = glOrtho;
    if (!lglReadPixels)     lglReadPixels = glReadPixels;
    if (!lglTexCoord2f)     lglTexCoord2f = glTexCoord2f;
    if (!lglTexImage2D)     lglTexImage2D = glTexImage2D;
    if (!lglTexParameteri)  lglTexParameteri = glTexParameteri;
    if (!lglVertex2f)       lglVertex2f = glVertex2f;
    if (!lglViewport)       lglViewport = glViewport;
#endif
    return lglBegin && lglBindTexture && lglBlendFunc && lglClear && lglClearColor &&
           lglColor4f && lglDeleteTextures && lglDisable && lglEnable && lglEnd &&
           lglGenTextures && lglLoadIdentity && lglMatrixMode && lglOrtho &&
           lglReadPixels && lglTexCoord2f && lglTexImage2D && lglTexParameteri &&
           lglVertex2f && lglViewport;
}

static void init_font_texture(void) {
    if (g_font_tex) return;
    size_t npix = (size_t)LAUNCHER_ATLAS_W * (size_t)LAUNCHER_ATLAS_H;
    uint8_t* rgba = (uint8_t*)malloc(npix * 4u);
    if (!rgba) return;
    for (size_t i = 0; i < npix; i++) {
        rgba[i * 4u + 0] = 255u;
        rgba[i * 4u + 1] = 255u;
        rgba[i * 4u + 2] = 255u;
        rgba[i * 4u + 3] = k_launcher_atlas[i];
    }
    lglGenTextures(1, &g_font_tex);
    lglBindTexture(GL_TEXTURE_2D, g_font_tex);
    lglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    lglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    lglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    lglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    lglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LAUNCHER_ATLAS_W, LAUNCHER_ATLAS_H,
                  0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}

static uint32_t next_utf8_cp(const char** pp) {
    const unsigned char* s = (const unsigned char*)*pp;
    uint32_t c = s[0];
    if (c == 0) return 0;
    if (c < 0x80u) { *pp += 1; return c; }
    if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        *pp += 2;
        return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
    }
    if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *pp += 3;
        return ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    }
    *pp += 1;
    return '?';
}

static const LauncherGlyph* find_glyph(uint32_t cp) {
    for (int i = 0; i < k_launcher_glyph_count; i++) {
        if (k_launcher_glyphs[i].cp == cp) return &k_launcher_glyphs[i];
    }
    for (int i = 0; i < k_launcher_glyph_count; i++) {
        if (k_launcher_glyphs[i].cp == (uint32_t)'?') return &k_launcher_glyphs[i];
    }
    return &k_launcher_glyphs[0];
}

static float measure_text(const char* text, float scale) {
    float w = 0.0f;
    const char* p = text;
    while (*p) {
        uint32_t cp = next_utf8_cp(&p);
        const LauncherGlyph* g = find_glyph(cp);
        w += g->xadv * scale;
    }
    return w;
}

static void draw_text(float x, float y, float scale, uint32_t rgba, const char* text) {
    float r = ((rgba >> 24) & 0xFFu) / 255.0f;
    float g = ((rgba >> 16) & 0xFFu) / 255.0f;
    float b = ((rgba >> 8)  & 0xFFu) / 255.0f;
    float a = ( rgba        & 0xFFu) / 255.0f;

    lglEnable(GL_TEXTURE_2D);
    lglBindTexture(GL_TEXTURE_2D, g_font_tex);
    lglColor4f(r, g, b, a);
    lglBegin(GL_QUADS);
    float pen_x = x;
    const char* p = text;
    while (*p) {
        uint32_t cp = next_utf8_cp(&p);
        const LauncherGlyph* gl = find_glyph(cp);
        if (gl->w > 0.0f && gl->h > 0.0f) {
            float x0 = pen_x + gl->xoff * scale;
            float y0 = y + gl->yoff * scale;
            float x1 = x0 + gl->w * scale;
            float y1 = y0 + gl->h * scale;
            lglTexCoord2f(gl->u0, gl->v0); lglVertex2f(x0, y0);
            lglTexCoord2f(gl->u1, gl->v0); lglVertex2f(x1, y0);
            lglTexCoord2f(gl->u1, gl->v1); lglVertex2f(x1, y1);
            lglTexCoord2f(gl->u0, gl->v1); lglVertex2f(x0, y1);
        }
        pen_x += gl->xadv * scale;
    }
    lglEnd();
    lglDisable(GL_TEXTURE_2D);
}

static void draw_rect(float x, float y, float w, float h, uint32_t rgba) {
    float r = ((rgba >> 24) & 0xFFu) / 255.0f;
    float g = ((rgba >> 16) & 0xFFu) / 255.0f;
    float b = ((rgba >> 8)  & 0xFFu) / 255.0f;
    float a = ( rgba        & 0xFFu) / 255.0f;
    lglDisable(GL_TEXTURE_2D);
    lglColor4f(r, g, b, a);
    lglBegin(GL_QUADS);
    lglVertex2f(x,     y);
    lglVertex2f(x + w, y);
    lglVertex2f(x + w, y + h);
    lglVertex2f(x,     y + h);
    lglEnd();
}

static void draw_rect_outline(float x, float y, float w, float h, float t, uint32_t rgba) {
    draw_rect(x,         y,         w, t, rgba);
    draw_rect(x,         y + h - t, w, t, rgba);
    draw_rect(x,         y,         t, h, rgba);
    draw_rect(x + w - t, y,         t, h, rgba);
}

static void dump_launcher_bmp(const char* path) {
    uint8_t* pixels = (uint8_t*)malloc((size_t)LAUNCH_W * (size_t)LAUNCH_H * 4u);
    if (!pixels) return;
    lglReadPixels(0, 0, LAUNCH_W, LAUNCH_H, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    FILE* fp = fopen(path, "wb");
    if (fp) {
        uint32_t img_sz = (uint32_t)(LAUNCH_W * LAUNCH_H * 4);
        uint32_t file_sz = 54u + img_sz;
        uint8_t hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &file_sz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &LAUNCH_W, 4);
        memcpy(hdr + 22, &LAUNCH_H, 4);
        hdr[26] = 1; hdr[28] = 32;
        memcpy(hdr + 34, &img_sz, 4);
        fwrite(hdr, 1, 54, fp);
        fwrite(pixels, 1, img_sz, fp);
        fclose(fp);
        fprintf(stderr, "[launcher] Dumped BMP -> %s\n", path);
    }
    free(pixels);
}

static const char* label_ui_lang(RecompUiLang u) {
    return (u == RECOMP_UI_EN) ? "English" : "Русский";
}

static const char* label_text_lang(RecompTextLang t, bool en) {
    switch (t) {
        case RECOMP_TEXT_RU_PATCHED: return en ? "Russian (Patched / HSE-ZoG)" : "Русский (Доработанный / ВШЭ-ZoG)";
        case RECOMP_TEXT_RU_BUKA:    return en ? "Russian (Buka 2006)" : "Русский (Бука 2006)";
        default:                     return "English (Original)";
    }
}

static const char* label_voice_lang(RecompVoiceLang v, bool en) {
    if (v == RECOMP_VOICE_RU_BUKA) {
        return en ? "Russian (Buka)" : "Русская (Бука)";
    }
    return "English (Original)";
}

static const char* label_icons(RecompButtonIcons b, bool en) {
    switch (b) {
        case RECOMP_ICONS_SWITCH: return "Nintendo Switch (B/A/Y/X, +/-, ZL/ZR)";
        case RECOMP_ICONS_XBOX:   return en ? "Xbox (Colored A/B/X/Y)" : "Xbox (A/B/X/Y цветные)";
        default:                  return en ? "PC (Keyboard & Mouse)" : "ПК (Клавиатура и мышь)";
    }
}

static const char* label_res(RecompResMode r, bool en) {
    switch (r) {
        case RECOMP_RES_720P:  return "1280x720 (720p)";
        case RECOMP_RES_1080P: return "1920x1080 (1080p)";
        default:               return en ? "Auto (720p Handheld / 1080p Docked)" : "Авто (720p Портатив / 1080p Док)";
    }
}

static const char* label_aniso(int lvl, bool en) {
    if (lvl >= 16) return en ? "16x (Maximum)" : "16x (Максимум)";
    if (lvl >= 8)  return en ? "8x (Medium)" : "8x (Средняя)";
    return en ? "Off" : "Выключена";
}

static const char* label_gyro(RecompGyroMode g, bool en) {
    switch (g) {
        case RECOMP_GYRO_OFF:      return en ? "Off" : "Выключен";
        case RECOMP_GYRO_AIM_LOW:  return en ? "Aim Only (Low)" : "Прицел (Низкая)";
        case RECOMP_GYRO_AIM_HIGH: return en ? "Aim Only (High)" : "Прицел (Высокая)";
        case RECOMP_GYRO_ALWAYS:   return en ? "Always (Camera & Aim)" : "Всегда (Камера и прицел)";
        default:                   return en ? "Aim Only (Medium)" : "Прицел (Средняя)";
    }
}

static void cycle_ui_lang(RecompSettings* s) {
    s->ui_lang = (s->ui_lang == RECOMP_UI_RU) ? RECOMP_UI_EN : RECOMP_UI_RU;
}

static void cycle_text_lang(RecompSettings* s, int dir) {
    RecompTextLang opts[3];
    int n = 0;
    if (s->has_ru_patched) opts[n++] = RECOMP_TEXT_RU_PATCHED;
    if (s->has_ru_buka)    opts[n++] = RECOMP_TEXT_RU_BUKA;
    opts[n++] = RECOMP_TEXT_EN;
    int cur = 0;
    for (int i = 0; i < n; i++) {
        if (opts[i] == s->text_lang) { cur = i; break; }
    }
    cur = (cur + dir + n) % n;
    s->text_lang = opts[cur];
}

static void cycle_voice_lang(RecompSettings* s) {
    if (!s->has_ru_voice) {
        s->voice_lang = RECOMP_VOICE_EN;
        return;
    }
    s->voice_lang = (s->voice_lang == RECOMP_VOICE_RU_BUKA) ? RECOMP_VOICE_EN : RECOMP_VOICE_RU_BUKA;
}

static void cycle_icons(RecompSettings* s, int dir) {
    RecompButtonIcons opts[3];
    int n = 0;
    if (s->has_sw_icons) opts[n++] = RECOMP_ICONS_SWITCH;
    if (s->has_xb_icons) opts[n++] = RECOMP_ICONS_XBOX;
    opts[n++] = RECOMP_ICONS_PC;
    int cur = 0;
    for (int i = 0; i < n; i++) {
        if (opts[i] == s->button_icons) { cur = i; break; }
    }
    cur = (cur + dir + n) % n;
    s->button_icons = opts[cur];
}

static void cycle_res(RecompSettings* s, int dir) {
    static const RecompResMode opts[3] = {
        RECOMP_RES_AUTO, RECOMP_RES_720P, RECOMP_RES_1080P
    };
    int cur = 0;
    for (int i = 0; i < 3; i++) {
        if (opts[i] == s->res_mode) { cur = i; break; }
    }
    cur = (cur + dir + 3) % 3;
    s->res_mode = opts[cur];
}

static void cycle_aniso(RecompSettings* s, int dir) {
    static const int opts[3] = { 0, 8, 16 };
    int cur = 2;
    if (s->aniso_level <= 0) cur = 0;
    else if (s->aniso_level <= 8) cur = 1;
    cur = (cur + dir + 3) % 3;
    s->aniso_level = opts[cur];
}

static void cycle_gyro(RecompSettings* s, int dir) {
    static const RecompGyroMode opts[5] = {
        RECOMP_GYRO_OFF,
        RECOMP_GYRO_AIM_LOW,
        RECOMP_GYRO_AIM_MED,
        RECOMP_GYRO_AIM_HIGH,
        RECOMP_GYRO_ALWAYS
    };
    int cur = 2;
    for (int i = 0; i < 5; i++) {
        if (opts[i] == s->gyro_mode) { cur = i; break; }
    }
    cur = (cur + dir + 5) % 5;
    s->gyro_mode = opts[cur];
}

static void cycle_deadzone(RecompSettings* s, int dir) {
    int dz = s->stick_deadzone + dir * 5;
    if (dz < 5) dz = 25;
    if (dz > 25) dz = 5;
    s->stick_deadzone = dz;
}

#define LAUNCH_N_SETTINGS       17
#define LAUNCH_N_ROWS           18
#define LAUNCH_VISIBLE_SETTINGS  9

static void launcher_layout_metrics(float* out_cx, float* out_cw,
                                    float* out_card_y, float* out_card_h,
                                    float* out_rx, float* out_rw,
                                    float* out_row_h, float* out_start_h) {
    float cx = 120.0f;
    float cw = (float)LAUNCH_W - 240.0f;
    if (out_cx) *out_cx = cx;
    if (out_cw) *out_cw = cw;
    if (out_card_y) *out_card_y = 82.0f;
    if (out_card_h) *out_card_h = 542.0f;
    if (out_rx) *out_rx = cx + 22.0f;
    if (out_rw) *out_rw = cw - 52.0f;
    if (out_row_h) *out_row_h = 44.0f;
    if (out_start_h) *out_start_h = 52.0f;
}

static float launcher_visible_row_y(int vis_slot, float card_y) {
    return card_y + 16.0f + (float)vis_slot * 48.0f;
}

static float launcher_start_y(float card_y) {
    return card_y + 474.0f;
}

static int compute_scroll_top(int sel, int cur_scroll) {
    int max_scroll = LAUNCH_N_SETTINGS - LAUNCH_VISIBLE_SETTINGS;
    if (max_scroll < 0) max_scroll = 0;
    if (sel < LAUNCH_N_SETTINGS) {
        if (sel < cur_scroll) cur_scroll = sel;
        if (sel >= cur_scroll + LAUNCH_VISIBLE_SETTINGS) {
            cur_scroll = sel - LAUNCH_VISIBLE_SETTINGS + 1;
        }
    }
    if (cur_scroll < 0) cur_scroll = 0;
    if (cur_scroll > max_scroll) cur_scroll = max_scroll;
    return cur_scroll;
}

static bool launcher_is_applet_mode(void) {
#ifdef __SWITCH__
    AppletType atype = appletGetAppletType();
    return (atype != AppletType_Application && atype != AppletType_SystemApplication);
#else
    const char* sim = getenv("RECOMP_SIM_APPLET_MODE");
    return (sim && atoi(sim) > 0);
#endif
}

static void render_launcher_frame(int sel, int scroll_top, bool auto_active, int auto_ms_left, bool splash_mode) {
    const RecompSettings* s = recomp_settings_get();
    bool en = (s->ui_lang == RECOMP_UI_EN);

    lglViewport(0, 0, LAUNCH_W, LAUNCH_H);
    lglMatrixMode(GL_PROJECTION);
    lglLoadIdentity();
    lglOrtho(0.0, (double)LAUNCH_W, (double)LAUNCH_H, 0.0, -1.0, 1.0);
    lglMatrixMode(GL_MODELVIEW);
    lglLoadIdentity();
    lglDisable(GL_DEPTH_TEST);
    lglDisable(GL_CULL_FACE);
    lglEnable(GL_BLEND);
    lglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Grounded Camp Whispering Rock charcoal slate palette */
    lglClearColor(0.078f, 0.090f, 0.102f, 1.0f);
    lglClear(GL_COLOR_BUFFER_BIT);

    /* Top accent bar */
    draw_rect(0.0f, 0.0f, (float)LAUNCH_W, 5.0f, 0xD98B2BFF);

    float cx = 0.0f, cw = 0.0f, card_y = 0.0f, card_h = 0.0f;
    float rx = 0.0f, rw = 0.0f, row_h = 0.0f, start_h = 0.0f;
    launcher_layout_metrics(&cx, &cw, &card_y, &card_h, &rx, &rw, &row_h, &start_h);

    /* Title block */
    draw_text(cx, 16.0f, 1.24f, 0xECE6D8FF, "PSYCHONAUTS : SWITCH EDITION");
    if (launcher_is_applet_mode()) {
        draw_rect(cx, 48.0f, cw, 28.0f, 0x4A2610FF);
        draw_rect(cx, 48.0f, 6.0f, 28.0f, 0xF0A840FF);
        draw_rect_outline(cx, 48.0f, cw, 28.0f, 1.5f, 0xD98B2BFF);
        draw_text(cx + 14.0f, 52.0f, 0.74f, 0xFFD27DFF,
                  en ? "WARNING: Applet Mode (low RAM) - hold R over a game (Title Override) for full RAM!"
                     : "ВНИМАНИЕ: Applet Mode (мало ОЗУ) - запустите через R на игре (Title Override)!");
    } else {
        draw_text(cx, 52.0f, 0.78f, 0x9AA3ADFF,
                  en ? "Static ARM64 / OpenGL Recompilation  -  Language, Graphics and Controls Setup"
                     : "Статическая рекомпиляция ARM64 / OpenGL  -  Настройка языка, графики и управления");
    }

    /* Card surface */
    draw_rect(cx, card_y, cw, card_h, 0x171B20FF);
    draw_rect_outline(cx, card_y, cw, card_h, 2.0f, 0x303842FF);

    if (splash_mode) {
        const char* msg1 = en ? "Loading Psychonauts and preparing shaders..."
                              : "Загрузка Psychonauts и подготовка шейдеров...";
        const char* msg2 = en
            ? "First boot may take 10-20 seconds (shader cache is saved to SD)"
            : "Подождите 10-20 секунд при первом запуске (кеш шейдеров сохраняется)";
        float w1 = measure_text(msg1, 1.05f);
        float w2 = measure_text(msg2, 0.80f);
        draw_text(cx + (cw - w1) * 0.5f, card_y + 225.0f, 1.05f, 0xECE6D8FF, msg1);
        draw_text(cx + (cw - w2) * 0.5f, card_y + 271.0f, 0.80f, 0xD98B2BFF, msg2);
        return;
    }

    char dz_buf[16];
    snprintf(dz_buf, sizeof(dz_buf), "%d%%", s->stick_deadzone);

    const char* labels[LAUNCH_N_ROWS] = {
        en ? "Menu Language (Язык)"    : "Язык меню (Language)",
        en ? "Game Text & Fonts"       : "Язык текста и шрифтов",
        en ? "Voice & Cutscenes"       : "Озвучка и видеоролики",
        en ? "Controller Button Icons" : "Иконки кнопок геймпада",
        en ? "Aspect Ratio"            : "Формат экрана",
        en ? "Render Resolution"       : "Разрешение рендера",
        en ? "Anisotropic Filtering"   : "Анизотропная фильтрация",
        en ? "Anti-Aliasing (FXAA)"    : "Сглаживание (FXAA)",
        en ? "FPS Limit"               : "Лимит FPS",
        en ? "FPS Counter"             : "Счётчик FPS",
        en ? "Gyro Aiming (SixAxis)"   : "Гироскоп (SixAxis)",
        en ? "Invert Camera (X)"       : "Инверсия камеры (X)",
        en ? "Invert Camera (Y)"       : "Инверсия камеры (Y)",
        en ? "Stick Deadzone"          : "Мёртвая зона стиков",
        en ? "Swap A / B Buttons"      : "Поменять кнопки A / B",
        en ? "In-Game Subtitles"       : "Субтитры в игре",
        en ? "Rumble (HD Rumble)"      : "Вибрация (HD Rumble)",
        en ? "START GAME"              : "НАЧАТЬ ИГРУ"
    };
    const char* values[LAUNCH_N_ROWS] = {
        label_ui_lang(s->ui_lang),
        label_text_lang(s->text_lang, en),
        label_voice_lang(s->voice_lang, en),
        label_icons(s->button_icons, en),
        s->widescreen ? (en ? "16:9 (Widescreen)" : "16:9 (Широкоформатный)")
                      : (en ? "4:3 (Original)" : "4:3 (Оригинал)"),
        label_res(s->res_mode, en),
        label_aniso(s->aniso_level, en),
        s->fxaa ? "FXAA" : (en ? "Off" : "Выключено"),
        (s->fps_cap <= 30) ? "30 FPS" : "60 FPS",
        s->show_fps ? (en ? "On" : "Включён") : (en ? "Off" : "Выключен"),
        label_gyro(s->gyro_mode, en),
        s->invert_cam_x ? (en ? "On" : "Вкл") : (en ? "Off" : "Выкл"),
        s->invert_cam_y ? (en ? "On" : "Вкл") : (en ? "Off" : "Выкл"),
        dz_buf,
        s->swap_ab ? (en ? "Swapped (A=Jump / B=Back)" : "Инвертировать (A=Прыжок / B=Назад)")
                   : (en ? "Default (B=Jump / A=Back)" : "По умолчанию (B=Прыжок / A=Назад)"),
        s->subtitles ? (en ? "On" : "Включены") : (en ? "Off" : "Выключены"),
        s->rumble ? (en ? "On" : "Включена") : (en ? "Off" : "Выключена"),
        en ? "[ B / A / + ]  Launch" : "[ B / A / + ]  Запустить"
    };

    scroll_top = compute_scroll_top(sel, scroll_top);

    /* Visible scrollable settings rows */
    for (int slot = 0; slot < LAUNCH_VISIBLE_SETTINGS; slot++) {
        int i = scroll_top + slot;
        if (i >= LAUNCH_N_SETTINGS) break;
        float ry = launcher_visible_row_y(slot, card_y);
        bool active = (i == sel);
        uint32_t bg = active ? 0x242A32FF : 0x1B2026FF;
        draw_rect(rx, ry, rw, row_h, bg);
        if (active) {
            draw_rect(rx, ry, 6.0f, row_h, 0xD98B2BFF);
            draw_rect_outline(rx, ry, rw, row_h, 1.5f, 0xD98B2BFF);
        } else {
            draw_rect_outline(rx, ry, rw, row_h, 1.0f, 0x2A323CFF);
        }

        float ty = ry + 11.0f;
        draw_text(rx + 24.0f, ty, 0.77f, active ? 0xFFFFFFFF : 0xC8CED6FF, labels[i]);
        char vbuf[256];
        if (active) {
            snprintf(vbuf, sizeof(vbuf), "<  %s  >", values[i]);
        } else {
            snprintf(vbuf, sizeof(vbuf), "%s", values[i]);
        }
        float vw = measure_text(vbuf, 0.77f);
        uint32_t vcol = active ? 0xF0A840FF : 0x9AA3ADFF;
        if (!active && i == 10 && s->gyro_mode == RECOMP_GYRO_OFF) {
            vcol = 0x88929CFF;
        }
        draw_text(rx + rw - 24.0f - vw, ty, 0.77f, vcol, vbuf);
    }

    /* Scrollbar indicator along right edge of settings list */
    if (LAUNCH_N_SETTINGS > LAUNCH_VISIBLE_SETTINGS) {
        float track_x = rx + rw + 10.0f;
        float track_y = launcher_visible_row_y(0, card_y);
        float track_h = (float)(LAUNCH_VISIBLE_SETTINGS - 1) * 48.0f + row_h;
        draw_rect(track_x, track_y, 6.0f, track_h, 0x222830FF);
        float thumb_h = track_h * ((float)LAUNCH_VISIBLE_SETTINGS / (float)LAUNCH_N_SETTINGS);
        int max_scroll = LAUNCH_N_SETTINGS - LAUNCH_VISIBLE_SETTINGS;
        float thumb_y = track_y + (track_h - thumb_h) * ((float)scroll_top / (float)max_scroll);
        draw_rect(track_x, thumb_y, 6.0f, thumb_h, 0xD98B2BFF);
    }

    /* Pinned START GAME button */
    {
        int i = LAUNCH_N_SETTINGS;
        float ry = launcher_start_y(card_y);
        bool active = (sel == i);
        uint32_t bg = active ? 0xD98B2BFF : 0x262D35FF;
        draw_rect(rx, ry, rw, start_h, bg);
        uint32_t fg = active ? 0x14171AFF : 0xECE6D8FF;
        uint32_t sub = active ? 0x241B0EFF : 0x9AA3ADFF;
        draw_text(rx + 24.0f, ry + 13.0f, 0.94f, fg, labels[i]);
        float vw = measure_text(values[i], 0.80f);
        draw_text(rx + rw - 24.0f - vw, ry + 15.0f, 0.80f, sub, values[i]);
    }

    /* Auto-start progress bar on repeat boots */
    if (auto_active) {
        int sec = (auto_ms_left + 999) / 1000;
        char abuf[256];
        snprintf(abuf, sizeof(abuf),
                 en ? "Auto-starting with saved settings in %d s - press any button to change"
                    : "Автозапуск с выбранными настройками через %d сек - нажмите любую кнопку для изменения",
                 sec);
        draw_text(cx, card_y + card_h + 10.0f, 0.74f, 0xD98B2BFF, abuf);
        float frac = (float)auto_ms_left / 4000.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        draw_rect(cx, card_y + card_h + 34.0f, cw, 5.0f, 0x262C34FF);
        draw_rect(cx, card_y + card_h + 34.0f, cw * frac, 5.0f, 0xD98B2BFF);
    }

    /* Footer controls hint */
    draw_text(cx, (float)LAUNCH_H - 36.0f, 0.74f, 0x8E99A5FF,
              en ? "[D-Pad / Stick / Touch] Select   [Left/Right / B / A] Change   [+] Start   [-] Reset"
                 : "[D-Pad / Стик / Сенсор] Выбор   [Left/Right / B / A] Изменить   [+] Старт   [-] Сброс");
}

static void apply_launcher_cycle(RecompSettings* mut, int sel, int dir) {
    switch (sel) {
        case 0:  cycle_ui_lang(mut); break;
        case 1:  cycle_text_lang(mut, dir); break;
        case 2:  cycle_voice_lang(mut); break;
        case 3:  cycle_icons(mut, dir); break;
        case 4:  mut->widescreen = !mut->widescreen; break;
        case 5:  cycle_res(mut, dir); break;
        case 6:  cycle_aniso(mut, dir); break;
        case 7:  mut->fxaa = !mut->fxaa; break;
        case 8:  mut->fps_cap = (mut->fps_cap <= 30) ? 60 : 30; break;
        case 9:  mut->show_fps = !mut->show_fps; break;
        case 10: cycle_gyro(mut, dir); break;
        case 11: mut->invert_cam_x = !mut->invert_cam_x; break;
        case 12: mut->invert_cam_y = !mut->invert_cam_y; break;
        case 13: cycle_deadzone(mut, dir); break;
        case 14: mut->swap_ab = !mut->swap_ab; break;
        case 15: mut->subtitles = !mut->subtitles; break;
        case 16: mut->rumble = !mut->rumble; break;
        default: break;
    }
}

void launcher_ui_run(const char* game_dir) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef __SWITCH__
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    uint32_t win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;
#else
    uint32_t win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#endif

    SDL_Window* win = SDL_CreateWindow("Psychonauts",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       LAUNCH_W, LAUNCH_H, win_flags);
    if (!win) {
        fprintf(stderr, "[launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
    if (!gl_ctx) {
        SDL_GL_ResetAttributes();
        gl_ctx = SDL_GL_CreateContext(win);
    }
    if (!gl_ctx) {
        fprintf(stderr, "[launcher] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return;
    }
    SDL_GL_MakeCurrent(win, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    recomp_sdl_adopt_window(win, gl_ctx, LAUNCH_W, LAUNCH_H);
    if (!load_launcher_gl_procs()) {
        fprintf(stderr, "[launcher] GL entry points unavailable, skipping launcher UI\n");
        return;
    }
    init_font_texture();

    const char* skip = getenv("RECOMP_SKIP_LAUNCHER");
    const char* dump_path = getenv("RECOMP_LAUNCHER_DUMP_PATH");
    int dump_sel = 0;
    const char* env_sel = getenv("RECOMP_LAUNCHER_DUMP_SEL");
    if (env_sel && *env_sel) {
        dump_sel = atoi(env_sel);
        if (dump_sel < 0 || dump_sel >= LAUNCH_N_ROWS) dump_sel = 0;
    }

    int scroll_top = compute_scroll_top(dump_sel, 0);

    if (dump_path && *dump_path) {
        render_launcher_frame(dump_sel, scroll_top, false, 0, false);
        dump_launcher_bmp(dump_path);
        SDL_GL_SwapWindow(win);
    }

    if (skip && atoi(skip) > 0) {
        render_launcher_frame(LAUNCH_N_SETTINGS, scroll_top, false, 0, true);
        SDL_GL_SwapWindow(win);
        goto restore_gl;
    }

    RecompSettings* mut = recomp_settings_mut();
    bool auto_timer = mut->configured_once && !launcher_is_applet_mode();
    const char* force_launcher = getenv("RECOMP_FORCE_LAUNCHER");
    if (force_launcher && atoi(force_launcher) > 0) {
        auto_timer = false;
    }
    uint32_t start_ms = SDL_GetTicks();
    int sel = dump_sel;

    float cx = 0.0f, cw = 0.0f, card_y = 0.0f, card_h = 0.0f;
    float rx = 0.0f, rw = 0.0f, row_h = 0.0f, start_h = 0.0f;
    launcher_layout_metrics(&cx, &cw, &card_y, &card_h, &rx, &rw, &row_h, &start_h);
    (void)cx; (void)cw; (void)card_h;

#ifdef __SWITCH__
    PadState pad;
    padInitializeAny(&pad);
    hidInitializeTouchScreen();
    u64 prev_k = padGetButtons(&pad);
    int prev_stick_dir_y = 0;
    int prev_stick_dir_x = 0;
    bool prev_touch = false;
#endif

    const char* test_tap_env = getenv("RECOMP_LAUNCHER_TEST_TAP");
    bool running = true;
    while (running) {
        int auto_left = 0;
        if (auto_timer) {
            uint32_t elapsed = SDL_GetTicks() - start_ms;
            if (elapsed >= 4000u) {
                running = false;
                break;
            }
            auto_left = (int)(4000u - elapsed);
        }

        int move_y = 0;
        int move_x = 0;
        bool act_confirm = false;
        bool act_start = false;
        bool reset_defaults = false;
        bool tap_triggered = false;
        float tap_x = 0.0f, tap_y = 0.0f;

        if (test_tap_env && *test_tap_env) {
            if (sscanf(test_tap_env, "%f,%f", &tap_x, &tap_y) == 2) {
                tap_triggered = true;
            }
            test_tap_env = NULL;
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (ev.type == SDL_KEYDOWN) {
                auto_timer = false;
                switch (ev.key.keysym.sym) {
                    case SDLK_UP:     case SDLK_w: move_y = -1; break;
                    case SDLK_DOWN:   case SDLK_s: move_y = +1; break;
                    case SDLK_LEFT:   case SDLK_a: move_x = -1; break;
                    case SDLK_RIGHT:  case SDLK_d: move_x = +1; break;
                    case SDLK_RETURN: case SDLK_SPACE: act_confirm = true; break;
                    case SDLK_F5:     act_start = true; break;
                    default: break;
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                int ww = LAUNCH_W, wh = LAUNCH_H;
                SDL_GetWindowSize(win, &ww, &wh);
                if (ww > 0 && wh > 0) {
                    tap_triggered = true;
                    tap_x = (float)ev.button.x * ((float)LAUNCH_W / (float)ww);
                    tap_y = (float)ev.button.y * ((float)LAUNCH_H / (float)wh);
                }
            }
        }

#ifdef __SWITCH__
        padUpdate(&pad);
        u64 k = padGetButtons(&pad);
        u64 rose = k & ~prev_k;
        prev_k = k;

        HidAnalogStickState ls = padGetStickPos(&pad, 0);
        int sy = (ls.y > 16000) ? -1 : ((ls.y < -16000) ? +1 : 0);
        int sx = (ls.x > 16000) ? +1 : ((ls.x < -16000) ? -1 : 0);
        if (sy != 0 && sy != prev_stick_dir_y) move_y = sy;
        if (sx != 0 && sx != prev_stick_dir_x) move_x = sx;
        prev_stick_dir_y = sy;
        prev_stick_dir_x = sx;

        if (rose & HidNpadButton_Up)    move_y = -1;
        if (rose & HidNpadButton_Down)  move_y = +1;
        if (rose & HidNpadButton_Left)  move_x = -1;
        if (rose & HidNpadButton_Right) move_x = +1;
        if (rose & (HidNpadButton_B | HidNpadButton_A)) act_confirm = true;
        if (rose & HidNpadButton_Plus)  act_start = true;
        if (rose & HidNpadButton_Minus) reset_defaults = true;

        HidTouchScreenState ts;
        if (hidGetTouchScreenStates(&ts, 1) > 0 && ts.count > 0) {
            if (!prev_touch) {
                prev_touch = true;
                tap_triggered = true;
                tap_x = (float)ts.touches[0].x;
                tap_y = (float)ts.touches[0].y;
            }
        } else {
            prev_touch = false;
        }
        if (rose || move_y || move_x || act_confirm || act_start || tap_triggered) {
            auto_timer = false;
        }
#endif

        if (reset_defaults) {
            auto_timer = false;
            mut->text_lang = mut->has_ru_patched ? RECOMP_TEXT_RU_PATCHED : RECOMP_TEXT_EN;
            mut->voice_lang = mut->has_ru_voice ? RECOMP_VOICE_RU_BUKA : RECOMP_VOICE_EN;
            mut->button_icons = mut->has_sw_icons ? RECOMP_ICONS_SWITCH : RECOMP_ICONS_PC;
            mut->widescreen = true;
            mut->res_mode = RECOMP_RES_AUTO;
            mut->aniso_level = 16;
            mut->fxaa = false;
            mut->fps_cap = 60;
            mut->show_fps = false;
            mut->gyro_mode = RECOMP_GYRO_AIM_MED;
            mut->invert_cam_x = false;
            mut->invert_cam_y = false;
            mut->stick_deadzone = 10;
            mut->swap_ab = false;
            mut->subtitles = true;
            mut->rumble = true;
        }

        if (tap_triggered) {
            auto_timer = false;
            float sy_start = launcher_start_y(card_y);
            if (tap_x >= rx && tap_x <= rx + rw && tap_y >= sy_start && tap_y <= sy_start + start_h) {
                sel = LAUNCH_N_SETTINGS;
                act_confirm = true;
            } else {
                for (int slot = 0; slot < LAUNCH_VISIBLE_SETTINGS; slot++) {
                    int idx = scroll_top + slot;
                    if (idx >= LAUNCH_N_SETTINGS) break;
                    float ry = launcher_visible_row_y(slot, card_y);
                    if (tap_x >= rx && tap_x <= rx + rw && tap_y >= ry && tap_y <= ry + row_h) {
                        sel = idx;
                        move_x = (tap_x >= rx + rw * 0.50f && tap_x < rx + rw * 0.78f) ? -1 : +1;
                        break;
                    }
                }
            }
        }

        if (move_y != 0) {
            sel = (sel + move_y + LAUNCH_N_ROWS) % LAUNCH_N_ROWS;
            scroll_top = compute_scroll_top(sel, scroll_top);
        }
        if (move_x != 0 || (act_confirm && sel < LAUNCH_N_SETTINGS)) {
            int dir = (move_x < 0) ? -1 : +1;
            apply_launcher_cycle(mut, sel, dir);
        }
        if (act_start || (act_confirm && sel == LAUNCH_N_SETTINGS)) {
            running = false;
        }

        render_launcher_frame(sel, scroll_top, auto_timer, auto_left, false);
        if (dump_path && *dump_path && getenv("RECOMP_LAUNCHER_DUMP_AFTER_TAP")) {
            dump_launcher_bmp(dump_path);
            unsetenv("RECOMP_LAUNCHER_DUMP_AFTER_TAP");
            if (getenv("RECOMP_LAUNCHER_EXIT_AFTER_DUMP")) {
                running = false;
            }
        }
        SDL_GL_SwapWindow(win);
    }

    mut->configured_once = true;
    recomp_settings_save(game_dir);

    render_launcher_frame(LAUNCH_N_SETTINGS, scroll_top, false, 0, true);
    SDL_GL_SwapWindow(win);

restore_gl:
    if (g_font_tex) {
        lglDeleteTextures(1, &g_font_tex);
        g_font_tex = 0;
    }
    lglDisable(GL_BLEND);
    lglDisable(GL_TEXTURE_2D);
    lglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    lglClear(GL_COLOR_BUFFER_BIT);
}

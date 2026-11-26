#define _GNU_SOURCE
#include "runtime/postfx.h"
#include "runtime/launcher_font_atlas.h"
#include "runtime/memory.h"
#include "runtime/settings.h"

#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
#else
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_opengl_glext.h>
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_BINDING
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#endif
#ifndef GL_FRAMEBUFFER_EXT
#define GL_FRAMEBUFFER_EXT 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_BINDING_EXT
#define GL_FRAMEBUFFER_BINDING_EXT 0x8CA6
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#endif
#ifndef GL_BLEND_DST_RGB
#define GL_BLEND_DST_RGB 0x80C8
#endif
#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA 0x80CB
#endif
#ifndef GL_BLEND_DST_ALPHA
#define GL_BLEND_DST_ALPHA 0x80CA
#endif
#ifndef GL_VERTEX_PROGRAM_ARB
#define GL_VERTEX_PROGRAM_ARB 0x8620
#endif
#ifndef GL_FRAGMENT_PROGRAM_ARB
#define GL_FRAGMENT_PROGRAM_ARB 0x8804
#endif
#ifndef GL_SAMPLER_BINDING
#define GL_SAMPLER_BINDING 0x8919
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNPACK_SKIP_PIXELS
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif
#ifndef GL_UNPACK_SKIP_ROWS
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif

typedef void (APIENTRY *PGL_ActiveTexture_t)(GLenum);
typedef void (APIENTRY *PGL_AttachShader_t)(GLuint, GLuint);
typedef void (APIENTRY *PGL_Begin_t)(GLenum);
typedef void (APIENTRY *PGL_BindBuffer_t)(GLenum, GLuint);
typedef void (APIENTRY *PGL_BindFramebuffer_t)(GLenum, GLuint);
typedef void (APIENTRY *PGL_BindSampler_t)(GLuint, GLuint);
typedef void (APIENTRY *PGL_BindTexture_t)(GLenum, GLuint);
typedef void (APIENTRY *PGL_BlendFuncSeparate_t)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *PGL_BlendFunc_t)(GLenum, GLenum);
typedef void (APIENTRY *PGL_Color4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PGL_CompileShader_t)(GLuint);
typedef void (APIENTRY *PGL_CopyTexSubImage2D_t)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
typedef GLuint (APIENTRY *PGL_CreateProgram_t)(void);
typedef GLuint (APIENTRY *PGL_CreateShader_t)(GLenum);
typedef void (APIENTRY *PGL_DeleteProgram_t)(GLuint);
typedef void (APIENTRY *PGL_DeleteShader_t)(GLuint);
typedef void (APIENTRY *PGL_DeleteTextures_t)(GLsizei, const GLuint*);
typedef void (APIENTRY *PGL_Disable_t)(GLenum);
typedef void (APIENTRY *PGL_Enable_t)(GLenum);
typedef void (APIENTRY *PGL_End_t)(void);
typedef void (APIENTRY *PGL_GenTextures_t)(GLsizei, GLuint*);
typedef void (APIENTRY *PGL_GetIntegerv_t)(GLenum, GLint*);
typedef void (APIENTRY *PGL_GetProgramiv_t)(GLuint, GLenum, GLint*);
typedef void (APIENTRY *PGL_GetShaderiv_t)(GLuint, GLenum, GLint*);
typedef GLint (APIENTRY *PGL_GetUniformLocation_t)(GLuint, const GLchar*);
typedef GLboolean (APIENTRY *PGL_IsEnabled_t)(GLenum);
typedef void (APIENTRY *PGL_LinkProgram_t)(GLuint);
typedef void (APIENTRY *PGL_PixelStorei_t)(GLenum, GLint);
typedef void (APIENTRY *PGL_ShaderSource_t)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void (APIENTRY *PGL_TexCoord2f_t)(GLfloat, GLfloat);
typedef void (APIENTRY *PGL_TexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
typedef void (APIENTRY *PGL_TexParameteri_t)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PGL_Uniform1i_t)(GLint, GLint);
typedef void (APIENTRY *PGL_Uniform2f_t)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *PGL_Uniform4f_t)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PGL_UseProgram_t)(GLuint);
typedef void (APIENTRY *PGL_Vertex2f_t)(GLfloat, GLfloat);
typedef void (APIENTRY *PGL_Viewport_t)(GLint, GLint, GLsizei, GLsizei);

static PGL_ActiveTexture_t      pglActiveTexture = NULL;
static PGL_AttachShader_t       pglAttachShader = NULL;
static PGL_Begin_t              pglBegin = NULL;
static PGL_BindBuffer_t         pglBindBuffer = NULL;
static PGL_BindFramebuffer_t    pglBindFramebuffer = NULL;
static PGL_BindSampler_t        pglBindSampler = NULL;
static PGL_BindTexture_t        pglBindTexture = NULL;
static PGL_BlendFuncSeparate_t  pglBlendFuncSeparate = NULL;
static PGL_BlendFunc_t          pglBlendFunc = NULL;
static PGL_Color4f_t            pglColor4f = NULL;
static PGL_CompileShader_t      pglCompileShader = NULL;
static PGL_CopyTexSubImage2D_t  pglCopyTexSubImage2D = NULL;
static PGL_CreateProgram_t      pglCreateProgram = NULL;
static PGL_CreateShader_t       pglCreateShader = NULL;
static PGL_DeleteProgram_t      pglDeleteProgram = NULL;
static PGL_DeleteShader_t       pglDeleteShader = NULL;
static PGL_DeleteTextures_t     pglDeleteTextures = NULL;
static PGL_Disable_t            pglDisable = NULL;
static PGL_Enable_t             pglEnable = NULL;
static PGL_End_t                pglEnd = NULL;
static PGL_GenTextures_t        pglGenTextures = NULL;
static PGL_GetIntegerv_t        pglGetIntegerv = NULL;
static PGL_GetProgramiv_t       pglGetProgramiv = NULL;
static PGL_GetShaderiv_t        pglGetShaderiv = NULL;
static PGL_GetUniformLocation_t pglGetUniformLocation = NULL;
static PGL_IsEnabled_t          pglIsEnabled = NULL;
static PGL_LinkProgram_t        pglLinkProgram = NULL;
static PGL_PixelStorei_t        pglPixelStorei = NULL;
static PGL_ShaderSource_t       pglShaderSource = NULL;
static PGL_TexCoord2f_t         pglTexCoord2f = NULL;
static PGL_TexImage2D_t         pglTexImage2D = NULL;
static PGL_TexParameteri_t      pglTexParameteri = NULL;
static PGL_Uniform1i_t          pglUniform1i = NULL;
static PGL_Uniform2f_t          pglUniform2f = NULL;
static PGL_Uniform4f_t          pglUniform4f = NULL;
static PGL_UseProgram_t         pglUseProgram = NULL;
static PGL_Vertex2f_t           pglVertex2f = NULL;
static PGL_Viewport_t           pglViewport = NULL;

static bool   g_gl_loaded = false;
static bool   g_gl_ok = false;

static GLuint g_fxaa_prog = 0;
static GLint  g_fxaa_u_tex = -1;
static GLint  g_fxaa_u_inv_res = -1;
static bool   g_fxaa_attempted = false;
static GLuint g_fxaa_tex = 0;
static int    g_fxaa_tex_w = 0;
static int    g_fxaa_tex_h = 0;

static GLuint g_ui_prog = 0;
static GLint  g_ui_u_tex = -1;
static GLint  g_ui_u_color = -1;
static GLint  g_ui_u_use_tex = -1;
static bool   g_ui_attempted = false;
static GLuint g_overlay_font_tex = 0;

/* FPS smoothing state */
static uint32_t g_fps_window_start = 0;
static uint32_t g_fps_frames = 0;
static float    g_fps_value = 0.0f;
static float    g_ms_value = 0.0f;

/* In-game Quick Menu overlay state */
#define OVERLAY_N_SETTINGS 9
#define OVERLAY_N_ROWS     10
static bool g_overlay_open = false;
static int  g_overlay_sel = 0;
static bool g_overlay_dirty = false;

bool recomp_overlay_is_open(void) {
    return g_overlay_open;
}

void recomp_overlay_set_open(bool open) {
    if (g_overlay_open && !open && g_overlay_dirty) {
        recomp_settings_save(recomp_get_game_dir());
        g_overlay_dirty = false;
    }
    g_overlay_open = open;
    if (open) g_overlay_sel = 0;
}

void recomp_overlay_toggle(void) {
    recomp_overlay_set_open(!g_overlay_open);
}

static void overlay_cycle_gyro(RecompSettings* s, int dir) {
    static const RecompGyroMode opts[5] = {
        RECOMP_GYRO_OFF, RECOMP_GYRO_AIM_LOW, RECOMP_GYRO_AIM_MED,
        RECOMP_GYRO_AIM_HIGH, RECOMP_GYRO_ALWAYS
    };
    int cur = 2;
    for (int i = 0; i < 5; i++) {
        if (opts[i] == s->gyro_mode) { cur = i; break; }
    }
    cur = (cur + dir + 5) % 5;
    s->gyro_mode = opts[cur];
}

static void overlay_cycle_deadzone(RecompSettings* s, int dir) {
    int dz = s->stick_deadzone + dir * 5;
    if (dz < 5) dz = 25;
    if (dz > 25) dz = 5;
    s->stick_deadzone = dz;
}

bool recomp_overlay_handle_input(int move_y, int move_x, bool confirm, bool cancel) {
    if (!g_overlay_open) return false;
    if (cancel) {
        recomp_overlay_set_open(false);
        return false;
    }
    if (move_y != 0) {
        g_overlay_sel = (g_overlay_sel + move_y + OVERLAY_N_ROWS) % OVERLAY_N_ROWS;
    }
    if (confirm && g_overlay_sel == OVERLAY_N_SETTINGS) {
        recomp_overlay_set_open(false);
        return false;
    }
    if (move_x != 0 || (confirm && g_overlay_sel < OVERLAY_N_SETTINGS)) {
        int dir = (move_x < 0) ? -1 : +1;
        RecompSettings* m = recomp_settings_mut();
        switch (g_overlay_sel) {
            case 0: overlay_cycle_gyro(m, dir); break;
            case 1: m->invert_cam_x = !m->invert_cam_x; break;
            case 2: m->invert_cam_y = !m->invert_cam_y; break;
            case 3: overlay_cycle_deadzone(m, dir); break;
            case 4: m->swap_ab = !m->swap_ab; break;
            case 5: m->rumble = !m->rumble; break;
            case 6: m->fxaa = !m->fxaa; break;
            case 7: m->fps_cap = (m->fps_cap <= 30) ? 60 : 30; break;
            case 8: m->show_fps = !m->show_fps; break;
            default: break;
        }
        g_overlay_dirty = true;
        return true;
    }
    return false;
}

static void* postfx_proc(const char* name) {
#ifdef __SWITCH__
    void* p = (void*)eglGetProcAddress(name);
    if (!p) p = SDL_GL_GetProcAddress(name);
    return p;
#else
    return SDL_GL_GetProcAddress(name);
#endif
}

static bool load_postfx_gl(void) {
    if (g_gl_loaded) return g_gl_ok;
    g_gl_loaded = true;

    pglActiveTexture      = (PGL_ActiveTexture_t)postfx_proc("glActiveTexture");
    pglAttachShader       = (PGL_AttachShader_t)postfx_proc("glAttachShader");
    pglBegin              = (PGL_Begin_t)postfx_proc("glBegin");
    pglBindBuffer         = (PGL_BindBuffer_t)postfx_proc("glBindBuffer");
    if (!pglBindBuffer) pglBindBuffer = (PGL_BindBuffer_t)postfx_proc("glBindBufferARB");
    pglBindFramebuffer    = (PGL_BindFramebuffer_t)postfx_proc("glBindFramebuffer");
    if (!pglBindFramebuffer) pglBindFramebuffer = (PGL_BindFramebuffer_t)postfx_proc("glBindFramebufferEXT");
    pglBindSampler        = (PGL_BindSampler_t)postfx_proc("glBindSampler");
    pglBindTexture        = (PGL_BindTexture_t)postfx_proc("glBindTexture");
    pglBlendFuncSeparate  = (PGL_BlendFuncSeparate_t)postfx_proc("glBlendFuncSeparate");
    pglBlendFunc          = (PGL_BlendFunc_t)postfx_proc("glBlendFunc");
    pglColor4f            = (PGL_Color4f_t)postfx_proc("glColor4f");
    pglCompileShader      = (PGL_CompileShader_t)postfx_proc("glCompileShader");
    pglCopyTexSubImage2D  = (PGL_CopyTexSubImage2D_t)postfx_proc("glCopyTexSubImage2D");
    pglCreateProgram      = (PGL_CreateProgram_t)postfx_proc("glCreateProgram");
    pglCreateShader       = (PGL_CreateShader_t)postfx_proc("glCreateShader");
    pglDeleteProgram      = (PGL_DeleteProgram_t)postfx_proc("glDeleteProgram");
    pglDeleteShader       = (PGL_DeleteShader_t)postfx_proc("glDeleteShader");
    pglDeleteTextures     = (PGL_DeleteTextures_t)postfx_proc("glDeleteTextures");
    pglDisable            = (PGL_Disable_t)postfx_proc("glDisable");
    pglEnable             = (PGL_Enable_t)postfx_proc("glEnable");
    pglEnd                = (PGL_End_t)postfx_proc("glEnd");
    pglGenTextures        = (PGL_GenTextures_t)postfx_proc("glGenTextures");
    pglGetIntegerv        = (PGL_GetIntegerv_t)postfx_proc("glGetIntegerv");
    pglGetProgramiv       = (PGL_GetProgramiv_t)postfx_proc("glGetProgramiv");
    pglGetShaderiv        = (PGL_GetShaderiv_t)postfx_proc("glGetShaderiv");
    pglGetUniformLocation = (PGL_GetUniformLocation_t)postfx_proc("glGetUniformLocation");
    pglIsEnabled          = (PGL_IsEnabled_t)postfx_proc("glIsEnabled");
    pglLinkProgram        = (PGL_LinkProgram_t)postfx_proc("glLinkProgram");
    pglPixelStorei        = (PGL_PixelStorei_t)postfx_proc("glPixelStorei");
    pglShaderSource       = (PGL_ShaderSource_t)postfx_proc("glShaderSource");
    pglTexCoord2f         = (PGL_TexCoord2f_t)postfx_proc("glTexCoord2f");
    pglTexImage2D         = (PGL_TexImage2D_t)postfx_proc("glTexImage2D");
    pglTexParameteri      = (PGL_TexParameteri_t)postfx_proc("glTexParameteri");
    pglUniform1i          = (PGL_Uniform1i_t)postfx_proc("glUniform1i");
    pglUniform2f          = (PGL_Uniform2f_t)postfx_proc("glUniform2f");
    pglUniform4f          = (PGL_Uniform4f_t)postfx_proc("glUniform4f");
    pglUseProgram         = (PGL_UseProgram_t)postfx_proc("glUseProgram");
    pglVertex2f           = (PGL_Vertex2f_t)postfx_proc("glVertex2f");
    pglViewport           = (PGL_Viewport_t)postfx_proc("glViewport");

#ifndef __SWITCH__
    if (!pglBegin)             pglBegin = glBegin;
    if (!pglBindTexture)       pglBindTexture = glBindTexture;
    if (!pglBlendFunc)         pglBlendFunc = glBlendFunc;
    if (!pglColor4f)           pglColor4f = glColor4f;
    if (!pglCopyTexSubImage2D) pglCopyTexSubImage2D = glCopyTexSubImage2D;
    if (!pglDeleteTextures)    pglDeleteTextures = glDeleteTextures;
    if (!pglDisable)           pglDisable = glDisable;
    if (!pglEnable)            pglEnable = glEnable;
    if (!pglEnd)               pglEnd = glEnd;
    if (!pglGenTextures)       pglGenTextures = glGenTextures;
    if (!pglGetIntegerv)       pglGetIntegerv = glGetIntegerv;
    if (!pglIsEnabled)         pglIsEnabled = glIsEnabled;
    if (!pglPixelStorei)       pglPixelStorei = glPixelStorei;
    if (!pglTexCoord2f)        pglTexCoord2f = glTexCoord2f;
    if (!pglTexImage2D)        pglTexImage2D = glTexImage2D;
    if (!pglTexParameteri)     pglTexParameteri = glTexParameteri;
    if (!pglVertex2f)          pglVertex2f = glVertex2f;
    if (!pglViewport)          pglViewport = glViewport;
#endif

    g_gl_ok = pglBegin && pglBindTexture && pglBlendFunc && pglColor4f &&
              pglCopyTexSubImage2D && pglDeleteTextures && pglDisable &&
              pglEnable && pglEnd && pglGenTextures && pglGetIntegerv &&
              pglIsEnabled && pglPixelStorei && pglTexCoord2f && pglTexImage2D &&
              pglTexParameteri && pglVertex2f && pglViewport;
    return g_gl_ok;
}

static GLuint compile_program(const char* vs_src, const char* fs_src) {
    if (!pglCreateShader || !pglShaderSource || !pglCompileShader || !pglGetShaderiv ||
        !pglCreateProgram || !pglAttachShader || !pglLinkProgram || !pglGetProgramiv) {
        return 0;
    }
    GLuint vs = pglCreateShader(GL_VERTEX_SHADER);
    GLuint fs = pglCreateShader(GL_FRAGMENT_SHADER);
    pglShaderSource(vs, 1, &vs_src, NULL);
    pglCompileShader(vs);
    pglShaderSource(fs, 1, &fs_src, NULL);
    pglCompileShader(fs);
    GLint vok = 0, fok = 0;
    pglGetShaderiv(vs, GL_COMPILE_STATUS, &vok);
    pglGetShaderiv(fs, GL_COMPILE_STATUS, &fok);
    GLuint prog = 0;
    if (vok && fok) {
        prog = pglCreateProgram();
        pglAttachShader(prog, vs);
        pglAttachShader(prog, fs);
        pglLinkProgram(prog);
        GLint lok = 0;
        pglGetProgramiv(prog, GL_LINK_STATUS, &lok);
        if (!lok) {
            if (pglDeleteProgram) pglDeleteProgram(prog);
            prog = 0;
        }
    }
    if (pglDeleteShader) {
        pglDeleteShader(vs);
        pglDeleteShader(fs);
    }
    return prog;
}

static const char* k_quad_vs =
    "#version 120\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = gl_MultiTexCoord0.xy;\n"
    "    gl_Position = gl_Vertex;\n"
    "}\n";

static const char* k_fxaa_fs =
    "#version 120\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_inv_res;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec3 luma = vec3(0.299, 0.587, 0.114);\n"
    "    vec3 rgbNW = texture2D(u_tex, v_uv + vec2(-1.0, -1.0) * u_inv_res).rgb;\n"
    "    vec3 rgbNE = texture2D(u_tex, v_uv + vec2( 1.0, -1.0) * u_inv_res).rgb;\n"
    "    vec3 rgbSW = texture2D(u_tex, v_uv + vec2(-1.0,  1.0) * u_inv_res).rgb;\n"
    "    vec3 rgbSE = texture2D(u_tex, v_uv + vec2( 1.0,  1.0) * u_inv_res).rgb;\n"
    "    vec3 rgbM  = texture2D(u_tex, v_uv).rgb;\n"
    "    float lumaNW = dot(rgbNW, luma);\n"
    "    float lumaNE = dot(rgbNE, luma);\n"
    "    float lumaSW = dot(rgbSW, luma);\n"
    "    float lumaSE = dot(rgbSE, luma);\n"
    "    float lumaM  = dot(rgbM,  luma);\n"
    "    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));\n"
    "    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));\n"
    "    float range = lumaMax - lumaMin;\n"
    "    if (range < max(0.0312, lumaMax * 0.125)) {\n"
    "        gl_FragColor = vec4(rgbM, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 dir;\n"
    "    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));\n"
    "    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));\n"
    "    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);\n"
    "    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
    "    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * u_inv_res;\n"
    "    vec3 rgbA = 0.5 * (\n"
    "        texture2D(u_tex, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +\n"
    "        texture2D(u_tex, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "    vec3 rgbB = rgbA * 0.5 + 0.25 * (\n"
    "        texture2D(u_tex, v_uv + dir * -0.5).rgb +\n"
    "        texture2D(u_tex, v_uv + dir *  0.5).rgb);\n"
    "    float lumaB = dot(rgbB, luma);\n"
    "    if (lumaB < lumaMin || lumaB > lumaMax) {\n"
    "        gl_FragColor = vec4(rgbA, 1.0);\n"
    "    } else {\n"
    "        gl_FragColor = vec4(rgbB, 1.0);\n"
    "    }\n"
    "}\n";

static const char* k_ui_fs =
    "#version 120\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "uniform int u_use_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    if (u_use_tex != 0) {\n"
    "        float a = texture2D(u_tex, v_uv).a;\n"
    "        gl_FragColor = vec4(u_color.rgb, u_color.a * a);\n"
    "    } else {\n"
    "        gl_FragColor = u_color;\n"
    "    }\n"
    "}\n";

static bool ensure_fxaa_resources(int w, int h) {
    if (!g_fxaa_attempted) {
        g_fxaa_attempted = true;
        if (pglUseProgram && pglGetUniformLocation && pglUniform1i && pglUniform2f) {
            g_fxaa_prog = compile_program(k_quad_vs, k_fxaa_fs);
            if (g_fxaa_prog) {
                g_fxaa_u_tex = pglGetUniformLocation(g_fxaa_prog, "u_tex");
                g_fxaa_u_inv_res = pglGetUniformLocation(g_fxaa_prog, "u_inv_res");
                fprintf(stderr, "[postfx] FXAA shader compiled and linked\n");
            }
        }
    }
    if (!g_fxaa_prog) return false;

    if (!g_fxaa_tex || g_fxaa_tex_w != w || g_fxaa_tex_h != h) {
        if (!g_fxaa_tex) pglGenTextures(1, &g_fxaa_tex);
        pglBindTexture(GL_TEXTURE_2D, g_fxaa_tex);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_fxaa_tex_w = w;
        g_fxaa_tex_h = h;
    }
    return true;
}

static bool ensure_ui_resources(void) {
    if (!g_ui_attempted) {
        g_ui_attempted = true;
        if (pglUseProgram && pglGetUniformLocation && pglUniform1i && pglUniform4f) {
            g_ui_prog = compile_program(k_quad_vs, k_ui_fs);
            if (g_ui_prog) {
                g_ui_u_tex = pglGetUniformLocation(g_ui_prog, "u_tex");
                g_ui_u_color = pglGetUniformLocation(g_ui_prog, "u_color");
                g_ui_u_use_tex = pglGetUniformLocation(g_ui_prog, "u_use_tex");
            }
        }
    }
    if (!g_ui_prog) return false;

    if (!g_overlay_font_tex) {
        size_t npix = (size_t)LAUNCHER_ATLAS_W * (size_t)LAUNCHER_ATLAS_H;
        uint8_t* rgba = (uint8_t*)malloc(npix * 4u);
        if (!rgba) return false;
        for (size_t i = 0; i < npix; i++) {
            rgba[i * 4u + 0] = 255u;
            rgba[i * 4u + 1] = 255u;
            rgba[i * 4u + 2] = 255u;
            rgba[i * 4u + 3] = k_launcher_atlas[i];
        }
        GLint prev_row = 0, prev_sp = 0, prev_sr = 0, prev_al = 4;
        pglGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row);
        pglGetIntegerv(GL_UNPACK_SKIP_PIXELS, &prev_sp);
        pglGetIntegerv(GL_UNPACK_SKIP_ROWS, &prev_sr);
        pglGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_al);
        pglPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        pglPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        pglPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        pglPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        pglGenTextures(1, &g_overlay_font_tex);
        pglBindTexture(GL_TEXTURE_2D, g_overlay_font_tex);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LAUNCHER_ATLAS_W, LAUNCHER_ATLAS_H,
                      0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

        pglPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row);
        pglPixelStorei(GL_UNPACK_SKIP_PIXELS, prev_sp);
        pglPixelStorei(GL_UNPACK_SKIP_ROWS, prev_sr);
        pglPixelStorei(GL_UNPACK_ALIGNMENT, prev_al);
        free(rgba);
    }
    return g_overlay_font_tex != 0;
}

static uint32_t overlay_next_utf8_cp(const char** pp) {
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

static const LauncherGlyph* overlay_find_glyph(uint32_t cp) {
    for (int i = 0; i < k_launcher_glyph_count; i++) {
        if (k_launcher_glyphs[i].cp == cp) return &k_launcher_glyphs[i];
    }
    return &k_launcher_glyphs[0];
}

static float overlay_measure_utf8(const char* text, float scale) {
    float w = 0.0f;
    const char* p = text;
    while (*p) {
        w += overlay_find_glyph(overlay_next_utf8_cp(&p))->xadv * scale;
    }
    return w;
}

static inline float px_to_ndc_x(float x, float win_w) { return (x / win_w) * 2.0f - 1.0f; }
static inline float px_to_ndc_y(float y, float win_h) { return 1.0f - (y / win_h) * 2.0f; }

static void overlay_draw_rect_ndc(float x, float y, float w, float h, float win_w, float win_h) {
    float x0 = px_to_ndc_x(x, win_w);
    float y0 = px_to_ndc_y(y, win_h);
    float x1 = px_to_ndc_x(x + w, win_w);
    float y1 = px_to_ndc_y(y + h, win_h);
    pglBegin(GL_QUADS);
    pglTexCoord2f(0.0f, 0.0f); pglVertex2f(x0, y0);
    pglTexCoord2f(1.0f, 0.0f); pglVertex2f(x1, y0);
    pglTexCoord2f(1.0f, 1.0f); pglVertex2f(x1, y1);
    pglTexCoord2f(0.0f, 1.0f); pglVertex2f(x0, y1);
    pglEnd();
}

static void overlay_draw_utf8_ndc(float x, float y, float scale, float win_w, float win_h, const char* text) {
    pglBegin(GL_QUADS);
    float pen = x;
    const char* p = text;
    while (*p) {
        const LauncherGlyph* gl = overlay_find_glyph(overlay_next_utf8_cp(&p));
        if (gl->w > 0.0f && gl->h > 0.0f) {
            float x0 = px_to_ndc_x(pen + gl->xoff * scale, win_w);
            float y0 = px_to_ndc_y(y + gl->yoff * scale, win_h);
            float x1 = px_to_ndc_x(pen + (gl->xoff + gl->w) * scale, win_w);
            float y1 = px_to_ndc_y(y + (gl->yoff + gl->h) * scale, win_h);
            pglTexCoord2f(gl->u0, gl->v0); pglVertex2f(x0, y0);
            pglTexCoord2f(gl->u1, gl->v0); pglVertex2f(x1, y0);
            pglTexCoord2f(gl->u1, gl->v1); pglVertex2f(x1, y1);
            pglTexCoord2f(gl->u0, gl->v1); pglVertex2f(x0, y1);
        }
        pen += gl->xadv * scale;
    }
    pglEnd();
}

static const char* overlay_gyro_label(RecompGyroMode g, bool en) {
    switch (g) {
        case RECOMP_GYRO_OFF:      return en ? "Off" : "Выключен";
        case RECOMP_GYRO_AIM_LOW:  return en ? "Aim (Low)" : "Прицел (Низкая)";
        case RECOMP_GYRO_AIM_HIGH: return en ? "Aim (High)" : "Прицел (Высокая)";
        case RECOMP_GYRO_ALWAYS:   return en ? "Always" : "Всегда";
        default:                   return en ? "Aim (Medium)" : "Прицел (Средняя)";
    }
}

static void render_quick_menu_overlay(int win_w, int win_h, const RecompSettings* s) {
    bool en = (s->ui_lang == RECOMP_UI_EN);
    float sr = (float)win_h / 720.0f;
    float fw = (float)win_w;
    float fh = (float)win_h;

    pglEnable(GL_BLEND);
    pglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    pglUseProgram(g_ui_prog);
    if (g_ui_u_tex >= 0) pglUniform1i(g_ui_u_tex, 0);

    /* Dimmed full-screen backdrop */
    if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 0);
    if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.04f, 0.05f, 0.07f, 0.72f);
    overlay_draw_rect_ndc(0.0f, 0.0f, fw, fh, fw, fh);

    float cw = 760.0f * sr;
    float ch = 540.0f * sr;
    float cx = (fw - cw) * 0.5f;
    float cy = (fh - ch) * 0.5f;

    /* Card background + top amber bar */
    if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.09f, 0.11f, 0.13f, 0.96f);
    overlay_draw_rect_ndc(cx, cy, cw, ch, fw, fh);
    if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.85f, 0.55f, 0.17f, 1.0f);
    overlay_draw_rect_ndc(cx, cy, cw, 4.0f * sr, fw, fh);

    /* Title */
    pglBindTexture(GL_TEXTURE_2D, g_overlay_font_tex);
    if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 1);
    if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.93f, 0.90f, 0.85f, 1.0f);
    overlay_draw_utf8_ndc(cx + 24.0f * sr, cy + 16.0f * sr, 0.92f * sr, fw, fh,
                          en ? "QUICK SETTINGS  (L + R + -)" : "БЫСТРЫЕ НАСТРОЙКИ  (L + R + -)");

    char dz_buf[16];
    snprintf(dz_buf, sizeof(dz_buf), "%d%%", s->stick_deadzone);

    const char* labels[OVERLAY_N_ROWS] = {
        en ? "Gyro Aiming (SixAxis)" : "Гироскоп (SixAxis)",
        en ? "Invert Camera (X)"     : "Инверсия камеры (X)",
        en ? "Invert Camera (Y)"     : "Инверсия камеры (Y)",
        en ? "Stick Deadzone"        : "Мёртвая зона стиков",
        en ? "Swap A / B Buttons"    : "Поменять кнопки A / B",
        en ? "Rumble (HD Rumble)"    : "Вибрация (HD Rumble)",
        en ? "Anti-Aliasing (FXAA)"  : "Сглаживание (FXAA)",
        en ? "FPS Limit"             : "Лимит FPS",
        en ? "FPS Counter"           : "Счётчик FPS",
        en ? "RESUME GAME"           : "ПРОДОЛЖИТЬ ИГРУ"
    };
    const char* values[OVERLAY_N_ROWS] = {
        overlay_gyro_label(s->gyro_mode, en),
        s->invert_cam_x ? (en ? "On" : "Вкл") : (en ? "Off" : "Выкл"),
        s->invert_cam_y ? (en ? "On" : "Вкл") : (en ? "Off" : "Выкл"),
        dz_buf,
        s->swap_ab ? (en ? "Swapped" : "Инвертировать") : (en ? "Default" : "По умолчанию"),
        s->rumble ? (en ? "On" : "Включена") : (en ? "Off" : "Выключена"),
        s->fxaa ? "FXAA" : (en ? "Off" : "Выключено"),
        (s->fps_cap <= 30) ? "30 FPS" : "60 FPS",
        s->show_fps ? (en ? "On" : "Включён") : (en ? "Off" : "Выключен"),
        en ? "[ A / B ]  Close" : "[ A / B ]  Закрыть"
    };

    float rx = cx + 20.0f * sr;
    float rw = cw - 40.0f * sr;
    float row_h = 38.0f * sr;
    float pitch = 42.0f * sr;
    float start_y = cy + 58.0f * sr;

    for (int i = 0; i < OVERLAY_N_ROWS; i++) {
        float ry = start_y + (float)i * pitch + (i == OVERLAY_N_SETTINGS ? 8.0f * sr : 0.0f);
        bool active = (i == g_overlay_sel);
        bool is_resume = (i == OVERLAY_N_SETTINGS);

        if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 0);
        if (is_resume) {
            if (active) pglUniform4f(g_ui_u_color, 0.85f, 0.55f, 0.17f, 1.0f);
            else        pglUniform4f(g_ui_u_color, 0.15f, 0.18f, 0.21f, 1.0f);
        } else {
            if (active) pglUniform4f(g_ui_u_color, 0.16f, 0.19f, 0.23f, 1.0f);
            else        pglUniform4f(g_ui_u_color, 0.11f, 0.13f, 0.16f, 0.95f);
        }
        overlay_draw_rect_ndc(rx, ry, rw, row_h, fw, fh);

        if (active && !is_resume) {
            pglUniform4f(g_ui_u_color, 0.85f, 0.55f, 0.17f, 1.0f);
            overlay_draw_rect_ndc(rx, ry, 5.0f * sr, row_h, fw, fh);
        }

        if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 1);
        float ty = ry + 9.0f * sr;
        float tscale = 0.72f * sr;
        if (is_resume) {
            if (active) pglUniform4f(g_ui_u_color, 0.08f, 0.09f, 0.10f, 1.0f);
            else        pglUniform4f(g_ui_u_color, 0.93f, 0.90f, 0.85f, 1.0f);
        } else {
            if (active) pglUniform4f(g_ui_u_color, 1.0f, 1.0f, 1.0f, 1.0f);
            else        pglUniform4f(g_ui_u_color, 0.78f, 0.81f, 0.84f, 1.0f);
        }
        overlay_draw_utf8_ndc(rx + 18.0f * sr, ty, tscale, fw, fh, labels[i]);

        char vbuf[128];
        if (active && !is_resume) {
            snprintf(vbuf, sizeof(vbuf), "<  %s  >", values[i]);
        } else {
            snprintf(vbuf, sizeof(vbuf), "%s", values[i]);
        }
        float vw = overlay_measure_utf8(vbuf, tscale);
        if (is_resume) {
            if (active) pglUniform4f(g_ui_u_color, 0.14f, 0.11f, 0.05f, 1.0f);
            else        pglUniform4f(g_ui_u_color, 0.60f, 0.64f, 0.68f, 1.0f);
        } else {
            if (active) pglUniform4f(g_ui_u_color, 0.94f, 0.66f, 0.25f, 1.0f);
            else        pglUniform4f(g_ui_u_color, 0.60f, 0.64f, 0.68f, 1.0f);
        }
        overlay_draw_utf8_ndc(rx + rw - 18.0f * sr - vw, ty, tscale, fw, fh, vbuf);
    }

    /* Footer hint */
    if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 1);
    pglUniform4f(g_ui_u_color, 0.56f, 0.60f, 0.65f, 1.0f);
    overlay_draw_utf8_ndc(cx + 24.0f * sr, cy + ch - 32.0f * sr, 0.65f * sr, fw, fh,
                          en ? "[D-Pad / Stick] Navigate   [Left/Right / B] Change   [A / -] Close & Save"
                             : "[D-Pad / Стик] Выбор   [Left/Right / B] Изменить   [A / -] Закрыть и сохранить");
}

void recomp_postfx_on_swap(int win_w, int win_h) {
    const RecompSettings* st = recomp_settings_get();
    if (!st || (!st->fxaa && !st->show_fps && !g_overlay_open)) return;
    if (win_w <= 0 || win_h <= 0) return;
    if (!load_postfx_gl()) return;

    if (st->show_fps) {
        uint32_t now = SDL_GetTicks();
        if (g_fps_window_start == 0) {
            g_fps_window_start = now;
            g_fps_frames = 0;
        }
        g_fps_frames++;
        uint32_t dt = now - g_fps_window_start;
        if (dt >= 250u) {
            g_fps_value = (float)g_fps_frames * 1000.0f / (float)dt;
            g_ms_value = (float)dt / (float)g_fps_frames;
            g_fps_window_start = now;
            g_fps_frames = 0;
        }
    }

    /* Save GL state */
    GLint prev_prog = 0;
    GLint prev_active_tex = GL_TEXTURE0;
    GLint prev_tex0 = 0;
    GLint prev_sampler0 = 0;
    GLint prev_abuf = 0, prev_ebuf = 0, prev_pbo = 0;
    GLint prev_fbo = 0;
    GLint prev_vp[4] = {0, 0, win_w, win_h};
    GLint prev_src_rgb = GL_SRC_ALPHA, prev_dst_rgb = GL_ONE_MINUS_SRC_ALPHA;
    GLint prev_src_a = GL_SRC_ALPHA, prev_dst_a = GL_ONE_MINUS_SRC_ALPHA;

    if (pglUseProgram) pglGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    if (pglActiveTexture) pglGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    if (pglActiveTexture) pglActiveTexture(GL_TEXTURE0);
    pglGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
    if (pglBindSampler) pglGetIntegerv(GL_SAMPLER_BINDING, &prev_sampler0);
    if (pglBindBuffer) {
        pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abuf);
        pglGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev_ebuf);
        pglGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pbo);
    }
    if (pglBindFramebuffer) pglGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev_fbo);
    pglGetIntegerv(GL_VIEWPORT, prev_vp);
    pglGetIntegerv(GL_BLEND_SRC_RGB, &prev_src_rgb);
    pglGetIntegerv(GL_BLEND_DST_RGB, &prev_dst_rgb);
    pglGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_src_a);
    pglGetIntegerv(GL_BLEND_DST_ALPHA, &prev_dst_a);

    GLboolean en_scissor  = pglIsEnabled(GL_SCISSOR_TEST);
    GLboolean en_depth    = pglIsEnabled(GL_DEPTH_TEST);
    GLboolean en_cull     = pglIsEnabled(GL_CULL_FACE);
    GLboolean en_blend    = pglIsEnabled(GL_BLEND);
    GLboolean en_alpha    = pglIsEnabled(GL_ALPHA_TEST);
    GLboolean en_tex2d    = pglIsEnabled(GL_TEXTURE_2D);
    GLboolean en_lighting = pglIsEnabled(GL_LIGHTING);
    GLboolean en_fog      = pglIsEnabled(GL_FOG);
    GLboolean en_vp_arb   = pglIsEnabled(GL_VERTEX_PROGRAM_ARB);
    GLboolean en_fp_arb   = pglIsEnabled(GL_FRAGMENT_PROGRAM_ARB);

    if (pglBindFramebuffer && prev_fbo != 0) pglBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
    if (pglBindSampler && prev_sampler0 != 0) pglBindSampler(0, 0);
    if (pglBindBuffer) {
        if (prev_abuf != 0) pglBindBuffer(GL_ARRAY_BUFFER, 0);
        if (prev_ebuf != 0) pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        if (prev_pbo  != 0) pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    pglViewport(0, 0, win_w, win_h);
    pglDisable(GL_SCISSOR_TEST);
    pglDisable(GL_DEPTH_TEST);
    pglDisable(GL_CULL_FACE);
    pglDisable(GL_ALPHA_TEST);
    pglDisable(GL_LIGHTING);
    pglDisable(GL_FOG);
    if (en_vp_arb) pglDisable(GL_VERTEX_PROGRAM_ARB);
    if (en_fp_arb) pglDisable(GL_FRAGMENT_PROGRAM_ARB);

    /* 1. Optional FXAA pass */
    if (st->fxaa && ensure_fxaa_resources(win_w, win_h)) {
        pglDisable(GL_BLEND);
        pglEnable(GL_TEXTURE_2D);
        pglBindTexture(GL_TEXTURE_2D, g_fxaa_tex);
        pglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, win_w, win_h);
        pglUseProgram(g_fxaa_prog);
        if (g_fxaa_u_tex >= 0) pglUniform1i(g_fxaa_u_tex, 0);
        if (g_fxaa_u_inv_res >= 0) pglUniform2f(g_fxaa_u_inv_res, 1.0f / (float)win_w, 1.0f / (float)win_h);
        pglBegin(GL_QUADS);
        pglTexCoord2f(0.0f, 0.0f); pglVertex2f(-1.0f, -1.0f);
        pglTexCoord2f(1.0f, 0.0f); pglVertex2f( 1.0f, -1.0f);
        pglTexCoord2f(1.0f, 1.0f); pglVertex2f( 1.0f,  1.0f);
        pglTexCoord2f(0.0f, 1.0f); pglVertex2f(-1.0f,  1.0f);
        pglEnd();
    }

    /* 2. Optional FPS / frame time overlay (shader-driven, immune to fixed-function state) */
    if (st->show_fps && ensure_ui_resources()) {
        pglEnable(GL_BLEND);
        pglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        pglUseProgram(g_ui_prog);
        if (g_ui_u_tex >= 0) pglUniform1i(g_ui_u_tex, 0);

        float scale_ref = (float)win_h / 720.0f;
        float ui_scale = scale_ref * 0.72f;
        char buf[64];
        if (g_fps_value > 0.0f) {
            snprintf(buf, sizeof(buf), "%.1f FPS  (%.1f ms)", g_fps_value, g_ms_value);
        } else {
            snprintf(buf, sizeof(buf), "-- FPS");
        }
        float tw = overlay_measure_utf8(buf, ui_scale);
        float pad_x = 10.0f * scale_ref;
        float pad_y = 6.0f * scale_ref;
        float bx = 12.0f * scale_ref;
        float by = 12.0f * scale_ref;
        float bw = tw + pad_x * 2.0f;
        float bh = (float)LAUNCHER_FONT_LINE_H * ui_scale + pad_y * 2.0f;

        if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 0);
        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.07f, 0.09f, 0.11f, 0.80f);
        overlay_draw_rect_ndc(bx, by, bw, bh, (float)win_w, (float)win_h);

        pglBindTexture(GL_TEXTURE_2D, g_overlay_font_tex);
        if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 1);
        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.95f, 0.72f, 0.28f, 1.0f);
        overlay_draw_utf8_ndc(bx + pad_x, by + pad_y - 2.0f * ui_scale,
                              ui_scale, (float)win_w, (float)win_h, buf);
    }

    /* 3. Optional In-Game Quick Menu overlay */
    if (g_overlay_open && ensure_ui_resources()) {
        render_quick_menu_overlay(win_w, win_h, st);
    }

    /* Restore GL state */
    pglViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    if (en_scissor)  pglEnable(GL_SCISSOR_TEST);  else pglDisable(GL_SCISSOR_TEST);
    if (en_depth)    pglEnable(GL_DEPTH_TEST);    else pglDisable(GL_DEPTH_TEST);
    if (en_cull)     pglEnable(GL_CULL_FACE);     else pglDisable(GL_CULL_FACE);
    if (en_blend)    pglEnable(GL_BLEND);         else pglDisable(GL_BLEND);
    if (en_alpha)    pglEnable(GL_ALPHA_TEST);    else pglDisable(GL_ALPHA_TEST);
    if (en_tex2d)    pglEnable(GL_TEXTURE_2D);    else pglDisable(GL_TEXTURE_2D);
    if (en_lighting) pglEnable(GL_LIGHTING);      else pglDisable(GL_LIGHTING);
    if (en_fog)      pglEnable(GL_FOG);           else pglDisable(GL_FOG);
    if (en_vp_arb)   pglEnable(GL_VERTEX_PROGRAM_ARB);
    if (en_fp_arb)   pglEnable(GL_FRAGMENT_PROGRAM_ARB);

    if (pglBlendFuncSeparate) {
        pglBlendFuncSeparate((GLenum)prev_src_rgb, (GLenum)prev_dst_rgb,
                             (GLenum)prev_src_a, (GLenum)prev_dst_a);
    } else {
        pglBlendFunc((GLenum)prev_src_rgb, (GLenum)prev_dst_rgb);
    }

    if (pglBindSampler && prev_sampler0 != 0) pglBindSampler(0, (GLuint)prev_sampler0);
    pglBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
    if (pglActiveTexture) pglActiveTexture((GLenum)prev_active_tex);
    if (pglBindBuffer) {
        if (prev_abuf != 0) pglBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_abuf);
        if (prev_ebuf != 0) pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prev_ebuf);
        if (prev_pbo  != 0) pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)prev_pbo);
    }
    if (pglBindFramebuffer && prev_fbo != 0) pglBindFramebuffer(GL_FRAMEBUFFER_EXT, (GLuint)prev_fbo);
    if (pglUseProgram) pglUseProgram((GLuint)prev_prog);
}

void recomp_postfx_draw_loading(int win_w, int win_h, const char* stage, const char* hint, float progress) {
    if (win_w <= 0 || win_h <= 0) return;
    if (!load_postfx_gl()) return;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    /* Save GL state */
    GLint prev_prog = 0;
    GLint prev_active_tex = GL_TEXTURE0;
    GLint prev_tex0 = 0;
    GLint prev_sampler0 = 0;
    GLint prev_abuf = 0, prev_ebuf = 0, prev_pbo = 0;
    GLint prev_fbo = 0;
    GLint prev_vp[4] = {0, 0, win_w, win_h};
    GLint prev_src_rgb = GL_SRC_ALPHA, prev_dst_rgb = GL_ONE_MINUS_SRC_ALPHA;
    GLint prev_src_a = GL_SRC_ALPHA, prev_dst_a = GL_ONE_MINUS_SRC_ALPHA;

    if (pglUseProgram) pglGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    if (pglActiveTexture) pglGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    if (pglActiveTexture) pglActiveTexture(GL_TEXTURE0);
    pglGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
    if (pglBindSampler) pglGetIntegerv(GL_SAMPLER_BINDING, &prev_sampler0);
    if (pglBindBuffer) {
        pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abuf);
        pglGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev_ebuf);
        pglGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pbo);
    }
    if (pglBindFramebuffer) pglGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev_fbo);
    pglGetIntegerv(GL_VIEWPORT, prev_vp);
    pglGetIntegerv(GL_BLEND_SRC_RGB, &prev_src_rgb);
    pglGetIntegerv(GL_BLEND_DST_RGB, &prev_dst_rgb);
    pglGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_src_a);
    pglGetIntegerv(GL_BLEND_DST_ALPHA, &prev_dst_a);

    GLboolean en_scissor  = pglIsEnabled(GL_SCISSOR_TEST);
    GLboolean en_depth    = pglIsEnabled(GL_DEPTH_TEST);
    GLboolean en_cull     = pglIsEnabled(GL_CULL_FACE);
    GLboolean en_blend    = pglIsEnabled(GL_BLEND);
    GLboolean en_alpha    = pglIsEnabled(GL_ALPHA_TEST);
    GLboolean en_tex2d    = pglIsEnabled(GL_TEXTURE_2D);
    GLboolean en_lighting = pglIsEnabled(GL_LIGHTING);
    GLboolean en_fog      = pglIsEnabled(GL_FOG);
    GLboolean en_vp_arb   = pglIsEnabled(GL_VERTEX_PROGRAM_ARB);
    GLboolean en_fp_arb   = pglIsEnabled(GL_FRAGMENT_PROGRAM_ARB);

    if (pglBindFramebuffer && prev_fbo != 0) pglBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
    if (pglBindSampler && prev_sampler0 != 0) pglBindSampler(0, 0);
    if (pglBindBuffer) {
        if (prev_abuf != 0) pglBindBuffer(GL_ARRAY_BUFFER, 0);
        if (prev_ebuf != 0) pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        if (prev_pbo  != 0) pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    pglViewport(0, 0, win_w, win_h);
    pglDisable(GL_SCISSOR_TEST);
    pglDisable(GL_DEPTH_TEST);
    pglDisable(GL_CULL_FACE);
    pglDisable(GL_ALPHA_TEST);
    pglDisable(GL_LIGHTING);
    pglDisable(GL_FOG);
    if (en_vp_arb) pglDisable(GL_VERTEX_PROGRAM_ARB);
    if (en_fp_arb) pglDisable(GL_FRAGMENT_PROGRAM_ARB);

    if (ensure_ui_resources()) {
        pglEnable(GL_BLEND);
        pglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        pglUseProgram(g_ui_prog);
        if (g_ui_u_tex >= 0) pglUniform1i(g_ui_u_tex, 0);

        float wf = (float)win_w;
        float hf = (float)win_h;
        float s = hf / 720.0f;

        /* Opaque dark background matching launcher palette */
        if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 0);
        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.055f, 0.075f, 0.102f, 1.0f);
        overlay_draw_rect_ndc(0.0f, 0.0f, wf, hf, wf, hf);

        /* Centered loading card */
        float cw = 680.0f * s;
        float ch = 220.0f * s;
        float cx = (wf - cw) * 0.5f;
        float cy = (hf - ch) * 0.5f;

        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.095f, 0.125f, 0.165f, 0.96f);
        overlay_draw_rect_ndc(cx, cy, cw, ch, wf, hf);
        /* Top amber accent bar */
        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.92f, 0.65f, 0.22f, 1.0f);
        overlay_draw_rect_ndc(cx, cy, cw, 4.0f * s, wf, hf);

        /* Progress bar track */
        float bx = cx + 44.0f * s;
        float bw = cw - 88.0f * s;
        float bh = 18.0f * s;
        float by = cy + 124.0f * s;
        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.16f, 0.20f, 0.26f, 1.0f);
        overlay_draw_rect_ndc(bx, by, bw, bh, wf, hf);

        /* Progress fill */
        float fill_w = bw * progress;
        if (fill_w < 8.0f * s) fill_w = 8.0f * s;
        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.92f, 0.65f, 0.22f, 1.0f);
        overlay_draw_rect_ndc(bx, by, fill_w, bh, wf, hf);

        /* Subtle animated highlight inside progress bar */
        uint32_t ticks = SDL_GetTicks();
        float pulse_pos = (float)((ticks / 12u) % 100u) / 100.0f;
        float hw = 72.0f * s;
        float hx = bx + pulse_pos * (fill_w > hw ? (fill_w - hw) : 0.0f);
        if (fill_w > 16.0f * s) {
            float draw_hw = (hx + hw > bx + fill_w) ? (bx + fill_w - hx) : hw;
            if (draw_hw > 2.0f) {
                if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 1.0f, 0.88f, 0.55f, 0.45f);
                overlay_draw_rect_ndc(hx, by, draw_hw, bh, wf, hf);
            }
        }

        /* Text labels */
        pglBindTexture(GL_TEXTURE_2D, g_overlay_font_tex);
        if (g_ui_u_use_tex >= 0) pglUniform1i(g_ui_u_use_tex, 1);

        const char* title = "PSYCHONAUTS";
        float ts = s * 1.15f;
        float tw = overlay_measure_utf8(title, ts);
        if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.96f, 0.72f, 0.28f, 1.0f);
        overlay_draw_utf8_ndc(cx + (cw - tw) * 0.5f, cy + 28.0f * s, ts, wf, hf, title);

        if (stage && *stage) {
            char line[160];
            snprintf(line, sizeof(line), "%s  (%d%%)", stage, (int)(progress * 100.0f + 0.5f));
            float ss = s * 0.82f;
            float sw = overlay_measure_utf8(line, ss);
            if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.93f, 0.95f, 0.98f, 1.0f);
            overlay_draw_utf8_ndc(cx + (cw - sw) * 0.5f, cy + 82.0f * s, ss, wf, hf, line);
        }

        if (hint && *hint) {
            float hs = s * 0.64f;
            float hw_txt = overlay_measure_utf8(hint, hs);
            if (g_ui_u_color >= 0) pglUniform4f(g_ui_u_color, 0.64f, 0.72f, 0.80f, 1.0f);
            overlay_draw_utf8_ndc(cx + (cw - hw_txt) * 0.5f, cy + 166.0f * s, hs, wf, hf, hint);
        }
    }

    /* Restore GL state */
    pglViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    if (en_scissor)  pglEnable(GL_SCISSOR_TEST);  else pglDisable(GL_SCISSOR_TEST);
    if (en_depth)    pglEnable(GL_DEPTH_TEST);    else pglDisable(GL_DEPTH_TEST);
    if (en_cull)     pglEnable(GL_CULL_FACE);     else pglDisable(GL_CULL_FACE);
    if (en_blend)    pglEnable(GL_BLEND);         else pglDisable(GL_BLEND);
    if (en_alpha)    pglEnable(GL_ALPHA_TEST);    else pglDisable(GL_ALPHA_TEST);
    if (en_tex2d)    pglEnable(GL_TEXTURE_2D);    else pglDisable(GL_TEXTURE_2D);
    if (en_lighting) pglEnable(GL_LIGHTING);      else pglDisable(GL_LIGHTING);
    if (en_fog)      pglEnable(GL_FOG);           else pglDisable(GL_FOG);
    if (en_vp_arb)   pglEnable(GL_VERTEX_PROGRAM_ARB);
    if (en_fp_arb)   pglEnable(GL_FRAGMENT_PROGRAM_ARB);

    if (pglBlendFuncSeparate) {
        pglBlendFuncSeparate((GLenum)prev_src_rgb, (GLenum)prev_dst_rgb,
                             (GLenum)prev_src_a, (GLenum)prev_dst_a);
    } else {
        pglBlendFunc((GLenum)prev_src_rgb, (GLenum)prev_dst_rgb);
    }

    if (pglBindSampler && prev_sampler0 != 0) pglBindSampler(0, (GLuint)prev_sampler0);
    pglBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
    if (pglActiveTexture) pglActiveTexture((GLenum)prev_active_tex);
    if (pglBindBuffer) {
        if (prev_abuf != 0) pglBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_abuf);
        if (prev_ebuf != 0) pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prev_ebuf);
        if (prev_pbo  != 0) pglBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)prev_pbo);
    }
    if (pglBindFramebuffer && prev_fbo != 0) pglBindFramebuffer(GL_FRAMEBUFFER_EXT, (GLuint)prev_fbo);
    if (pglUseProgram) pglUseProgram((GLuint)prev_prog);
}

void recomp_postfx_shutdown(void) {
    if (g_fxaa_tex && pglDeleteTextures) {
        pglDeleteTextures(1, &g_fxaa_tex);
        g_fxaa_tex = 0;
    }
    if (g_overlay_font_tex && pglDeleteTextures) {
        pglDeleteTextures(1, &g_overlay_font_tex);
        g_overlay_font_tex = 0;
    }
    if (g_fxaa_prog && pglDeleteProgram) {
        pglDeleteProgram(g_fxaa_prog);
        g_fxaa_prog = 0;
    }
    if (g_ui_prog && pglDeleteProgram) {
        pglDeleteProgram(g_ui_prog);
        g_ui_prog = 0;
    }
    g_fxaa_tex_w = 0;
    g_fxaa_tex_h = 0;
    g_fxaa_attempted = false;
    g_ui_attempted = false;
}

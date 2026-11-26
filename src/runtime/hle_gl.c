#include "runtime/cpu.h"
#include "runtime/frame_profile.h"
#include "runtime/memory.h"
#include "runtime/settings.h"

#ifndef RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT
#define RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FEu
#endif
#ifndef RECOMP_GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define RECOMP_GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FFu
#endif

#ifdef __SWITCH__
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <switch.h>
#define GL_GLEXT_PROTOTYPES
static void* host_gl_get_proc(const char* name) {
    return (void*)eglGetProcAddress(name);
}
static inline uint64_t gl_prof_tick(void) { return armGetSystemTick(); }
static inline double gl_prof_ticks_to_ms(uint64_t t) { return (double)t / 19200.0; }
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_opengl_glext.h>
static void* host_gl_get_proc(const char* name) {
    return SDL_GL_GetProcAddress(name);
}
static inline uint64_t gl_prof_tick(void) { return SDL_GetPerformanceCounter(); }
static inline double gl_prof_ticks_to_ms(uint64_t t) {
    return (double)t * 1000.0 / (double)SDL_GetPerformanceFrequency();
}
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static inline uint32_t arg32(CPUContext* ctx, int idx) {
    return mem_r32(ctx->esp + (uint32_t)(4 * (idx + 1)));
}

static inline float argf32(CPUContext* ctx, int idx) {
    return mem_rf32(ctx->esp + (uint32_t)(4 * (idx + 1)));
}

static inline double argf64(CPUContext* ctx, int byte_off) {
    return mem_rf64(ctx->esp + (uint32_t)(4 + byte_off));
}

static inline const void* ptr32(uint32_t p) {
    return p ? (const void*)(g_mem_base + p) : NULL;
}

static inline void* mptr32(uint32_t p) {
    return p ? (void*)(g_mem_base + p) : NULL;
}

/* ---------------- Buffer Binding, Size & Mapping State ---------------- */

#define MAX_GL_BUFFERS 16384
#define MAX_MAPPED_SLOTS 8

static uint32_t g_buf_sizes[MAX_GL_BUFFERS];
static GLuint g_bound_array_buf = 0;
static GLuint g_bound_elem_buf = 0;
static GLuint g_bound_pbo_unpack = 0;
static GLuint g_bound_pbo_pack = 0;
static GLuint g_bound_other_buf = 0;
static bool   g_array_buf_valid = false;
static bool   g_elem_buf_valid = false;
static bool   g_pbo_unpack_valid = false;
static bool   g_pbo_pack_valid = false;

/* Redundant GL state filter cache */
#define GL_CACHE_MAX_TEX_UNITS 32
#define GL_CACHE_MAX_VAA 16
#define MAX_SAMPLER_TRACK 4096
typedef struct CacheVapEntry {
    bool     valid;
    GLuint   buf;
    GLint    size;
    GLenum   type;
    GLboolean norm;
    GLsizei  stride;
    uint32_t ptr;
} CacheVapEntry;

static GLuint g_cache_prog = 0xFFFFFFFFu;
static GLenum g_cache_active_tex = 0;
static GLuint g_cache_tex[3][GL_CACHE_MAX_TEX_UNITS];
static bool   g_cache_tex_valid[3][GL_CACHE_MAX_TEX_UNITS];
static GLuint g_cache_sampler[GL_CACHE_MAX_TEX_UNITS];
static bool   g_cache_sampler_valid[GL_CACHE_MAX_TEX_UNITS];
static GLenum g_cache_blend_src = 0, g_cache_blend_dst = 0;
static GLenum g_cache_blend_eq = 0;
static GLenum g_cache_alpha_fn = 0;
static float  g_cache_alpha_ref = -1.0f;
static int8_t g_cache_depth_mask = -1;
static GLenum g_cache_depth_func = 0;
static GLenum g_cache_cull_face = 0;
static int8_t g_cache_cmask[4] = {-1, -1, -1, -1};
static int8_t g_cache_caps[20] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
static int8_t g_cache_tex_enable[GL_CACHE_MAX_TEX_UNITS][3];
static bool   g_cache_poly_off_valid = false;
static float  g_cache_poly_off_f = 0.0f, g_cache_poly_off_u = 0.0f;
static GLenum g_cache_poly_mode_face = 0, g_cache_poly_mode = 0;
static GLenum g_cache_shade_model = 0;
static bool   g_cache_stencil_mask_valid = false;
static GLuint g_cache_stencil_mask = 0;
static GLenum g_cache_stencil_fn = 0;
static GLint  g_cache_stencil_ref = 0;
static GLuint g_cache_stencil_rmask = 0;
static GLenum g_cache_stencil_op[3] = {0, 0, 0};
static bool   g_cache_vp_valid = false;
static GLint  g_cache_vp[4] = {0, 0, 0, 0};
static bool   g_cache_sc_valid = false;
static GLint  g_cache_sc[4] = {0, 0, 0, 0};
static bool   g_cache_fbo_valid = false;

#define MAX_TEX_PARAM_TRACK 8192
static int32_t g_cache_tparam_val[MAX_TEX_PARAM_TRACK][6];
static uint8_t g_cache_tparam_valid[MAX_TEX_PARAM_TRACK];

/* 2-way set-associative uniform caches (512 u4fv slots, 256 u1i, 128 u1iv, 128 u4iv) */
#define U4FV_CACHE_SETS 256u
#define U4FV_CACHE_MAX_COUNT 64
typedef struct U4fvCacheSlot {
    GLuint   prog;
    GLint    loc;
    GLsizei  count;
    uint32_t gen;
    float    data[U4FV_CACHE_MAX_COUNT * 4];
} U4fvCacheSlot;
static U4fvCacheSlot g_u4fv_cache[U4FV_CACHE_SETS][2];
static uint8_t g_u4fv_way_rr[U4FV_CACHE_SETS];
static uint32_t g_u4fv_cache_gen = 1u;

#define U1I_CACHE_SETS 128u
typedef struct U1iCacheSlot {
    GLuint   prog;
    GLint    loc;
    GLint    val;
    uint32_t gen;
} U1iCacheSlot;
static U1iCacheSlot g_u1i_cache[U1I_CACHE_SETS][2];
static uint8_t g_u1i_way_rr[U1I_CACHE_SETS];

#define U1IV_CACHE_SETS 64u
#define U1IV_CACHE_MAX_COUNT 16
typedef struct U1ivCacheSlot {
    GLuint   prog;
    GLint    loc;
    GLsizei  count;
    uint32_t gen;
    int32_t  data[U1IV_CACHE_MAX_COUNT];
} U1ivCacheSlot;
static U1ivCacheSlot g_u1iv_cache[U1IV_CACHE_SETS][2];
static uint8_t g_u1iv_way_rr[U1IV_CACHE_SETS];

#define U4IV_CACHE_SETS 64u
#define U4IV_CACHE_MAX_COUNT 16
typedef struct U4ivCacheSlot {
    GLuint   prog;
    GLint    loc;
    GLsizei  count;
    uint32_t gen;
    int32_t  data[U4IV_CACHE_MAX_COUNT * 4];
} U4ivCacheSlot;
static U4ivCacheSlot g_u4iv_cache[U4IV_CACHE_SETS][2];
static uint8_t g_u4iv_way_rr[U4IV_CACHE_SETS];

static inline void u4fv_cache_invalidate(void) {
    g_u4fv_cache_gen++;
    if (g_u4fv_cache_gen == 0u) g_u4fv_cache_gen = 1u;
}
typedef struct ClientVapEntry {
    GLint     size;
    GLenum    type;
    GLboolean norm;
    GLsizei   stride;
    uint32_t  gptr;
    uint32_t  is_arb;
} ClientVapEntry;

static uint32_t g_cache_vaa_known = 0;
static uint32_t g_cache_vaa_enabled = 0;
static uint32_t g_client_vap_mask = 0;
static CacheVapEntry g_cache_vap[GL_CACHE_MAX_VAA];
static ClientVapEntry g_client_vap[GL_CACHE_MAX_VAA];

static inline uint32_t gl_type_size_bytes(GLenum type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1u;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2u;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4u;
        case GL_DOUBLE: return 8u;
        default: return 4u;
    }
}
static int32_t g_cache_sparam_val[MAX_SAMPLER_TRACK][6];
static uint8_t g_cache_sparam_valid[MAX_SAMPLER_TRACK];

static inline int sampler_param_slot(GLenum pname) {
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: return 0;
        case GL_TEXTURE_MAG_FILTER: return 1;
        case GL_TEXTURE_WRAP_S:     return 2;
        case GL_TEXTURE_WRAP_T:     return 3;
        case GL_TEXTURE_WRAP_R:     return 4;
        case RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT: return 5;
        default: return -1;
    }
}

static inline int gl_cap_slot(GLenum cap) {
    switch (cap) {
        case GL_BLEND: return 0;
        case GL_DEPTH_TEST: return 1;
        case GL_CULL_FACE: return 2;
        case GL_SCISSOR_TEST: return 3;
        case GL_ALPHA_TEST: return 4;
        case GL_STENCIL_TEST: return 5;
        case GL_POLYGON_OFFSET_FILL: return 6;
        case 0x0B60u /* GL_FOG */: return 7;
        case 0x0B50u /* GL_LIGHTING */: return 8;
        case 0x8642u /* GL_VERTEX_PROGRAM_POINT_SIZE */: return 9;
        case 0x0B57u /* GL_COLOR_MATERIAL */: return 10;
        case 0x0BA1u /* GL_NORMALIZE */: return 11;
        case 0x3000u /* GL_CLIP_PLANE0 */: return 12;
        case 0x3001u /* GL_CLIP_PLANE1 */: return 13;
        case 0x3002u /* GL_CLIP_PLANE2 */: return 14;
        case 0x3003u /* GL_CLIP_PLANE3 */: return 15;
        case 0x3004u /* GL_CLIP_PLANE4 */: return 16;
        case 0x3005u /* GL_CLIP_PLANE5 */: return 17;
        case 0x8861u /* GL_POINT_SPRITE */: return 18;
        case 0x809Du /* GL_MULTISAMPLE */: return 19;
        default: return -1;
    }
}

static inline int gl_tex_enable_slot(GLenum cap) {
    switch (cap) {
        case GL_TEXTURE_2D: return 0;
        case 0x8513u /* GL_TEXTURE_CUBE_MAP */: return 1;
        case 0x806Fu /* GL_TEXTURE_3D */: return 2;
        default: return -1;
    }
}

void recomp_gl_invalidate_state_cache(void) {
    g_array_buf_valid = false;
    g_elem_buf_valid = false;
    g_pbo_unpack_valid = false;
    g_pbo_pack_valid = false;
    g_cache_prog = 0xFFFFFFFFu;
    g_cache_active_tex = 0;
    memset(g_cache_tex_valid, 0, sizeof(g_cache_tex_valid));
    memset(g_cache_sampler_valid, 0, sizeof(g_cache_sampler_valid));
    g_cache_blend_src = 0; g_cache_blend_dst = 0;
    g_cache_blend_eq = 0;
    g_cache_alpha_fn = 0; g_cache_alpha_ref = -1.0f;
    g_cache_depth_mask = -1;
    g_cache_depth_func = 0;
    g_cache_cull_face = 0;
    g_cache_cmask[0] = -1;
    memset(g_cache_caps, -1, sizeof(g_cache_caps));
    memset(g_cache_tex_enable, 0, sizeof(g_cache_tex_enable));
    g_cache_poly_off_valid = false;
    g_cache_poly_mode_face = 0; g_cache_poly_mode = 0;
    g_cache_shade_model = 0;
    g_cache_stencil_mask_valid = false;
    g_cache_stencil_fn = 0;
    g_cache_stencil_op[0] = 0;
    g_cache_vp_valid = false;
    g_cache_sc_valid = false;
    g_cache_fbo_valid = false;
    u4fv_cache_invalidate();
    g_cache_vaa_known = 0;
    g_client_vap_mask = 0;
    for (int i = 0; i < GL_CACHE_MAX_VAA; i++) g_cache_vap[i].valid = false;
}

/* Default-framebuffer dynamic resolution scaling (Switch Docked <-> Handheld hot-swap) */
#ifndef GL_FRAMEBUFFER_EXT
#define GL_FRAMEBUFFER_EXT 0x8D40u
#endif
#ifndef GL_READ_FRAMEBUFFER_EXT
#define GL_READ_FRAMEBUFFER_EXT 0x8CA8u
#endif
#ifndef GL_DRAW_FRAMEBUFFER_EXT
#define GL_DRAW_FRAMEBUFFER_EXT 0x8CA9u
#endif

static GLuint  g_bound_draw_fbo = 0;
static GLuint  g_bound_read_fbo = 0;
static int     g_bb_guest_w = 0;
static int     g_bb_guest_h = 0;
static int     g_bb_host_w = 0;
static int     g_bb_host_h = 0;
static bool    g_has_last_vp = false;
static GLint   g_last_vp_x = 0, g_last_vp_y = 0;
static GLsizei g_last_vp_w = 0, g_last_vp_h = 0;
static bool    g_has_last_sc = false;
static GLint   g_last_sc_x = 0, g_last_sc_y = 0;
static GLsizei g_last_sc_w = 0, g_last_sc_h = 0;

static inline bool bb_scale_active(void) {
    return g_bb_guest_w > 0 && g_bb_guest_h > 0 &&
           g_bb_host_w > 0 && g_bb_host_h > 0 &&
           (g_bb_host_w != g_bb_guest_w || g_bb_host_h != g_bb_guest_h);
}

static inline GLint scale_bb_coord(GLint v, int num, int den) {
    int64_t p = (int64_t)v * (int64_t)num;
    return (GLint)((p >= 0 ? (p + den / 2) : (p - den / 2)) / den);
}

static void scale_bb_rect(GLint x, GLint y, GLsizei w, GLsizei h,
                          GLint* ox, GLint* oy, GLsizei* ow, GLsizei* oh) {
    if (!bb_scale_active()) {
        *ox = x; *oy = y; *ow = w; *oh = h;
        return;
    }
    GLint x0 = scale_bb_coord(x, g_bb_host_w, g_bb_guest_w);
    GLint y0 = scale_bb_coord(y, g_bb_host_h, g_bb_guest_h);
    GLint x1 = scale_bb_coord(x + (GLint)w, g_bb_host_w, g_bb_guest_w);
    GLint y1 = scale_bb_coord(y + (GLint)h, g_bb_host_h, g_bb_guest_h);
    *ox = x0;
    *oy = y0;
    *ow = (x1 > x0) ? (GLsizei)(x1 - x0) : 0;
    *oh = (y1 > y0) ? (GLsizei)(y1 - y0) : 0;
}

typedef struct MappedBufSlot {
    bool active;
    GLenum target;
    GLuint buf_id;
    uint32_t offset;
    uint32_t length;
    uint32_t capacity;
    uint32_t guest_ptr;
    bool flushed_explicit;
    void* real_map;
} MappedBufSlot;

static MappedBufSlot g_mapped_slots[MAX_MAPPED_SLOTS];

static GLuint* target_binding_ptr(GLenum target) {
    switch (target) {
        case GL_ARRAY_BUFFER: return &g_bound_array_buf;
        case GL_ELEMENT_ARRAY_BUFFER: return &g_bound_elem_buf;
        case GL_PIXEL_UNPACK_BUFFER: return &g_bound_pbo_unpack;
        case GL_PIXEL_PACK_BUFFER: return &g_bound_pbo_pack;
        default: return &g_bound_other_buf;
    }
}

static MappedBufSlot* find_mapped(GLenum target) {
    for (int i = 0; i < MAX_MAPPED_SLOTS; i++) {
        if (g_mapped_slots[i].active && g_mapped_slots[i].target == target)
            return &g_mapped_slots[i];
    }
    return NULL;
}

/* ---------------- Dynamic Host GL Pointers ---------------- */

typedef void (APIENTRY *PFN_glDebugMessageCallbackARB_t)(void*, const void*);
typedef void (APIENTRY *PFN_glBufferParameteriAPPLE_t)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glFlushMappedBufferRange_t)(GLenum, GLintptr, GLsizeiptr);
typedef void (APIENTRY *PFN_glProgramLocalParameterI4ivNV_t)(GLenum, GLuint, const GLint*);
typedef void (APIENTRY *PFN_glAlphaFunc_t)(GLenum, GLclampf);
typedef void (APIENTRY *PFN_glBindTexture_t)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glBlendFunc_t)(GLenum, GLenum);
typedef void (APIENTRY *PFN_glClear_t)(GLbitfield);
typedef void (APIENTRY *PFN_glClearColor_t)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (APIENTRY *PFN_glClearDepth_t)(GLclampd);
typedef void (APIENTRY *PFN_glClearStencil_t)(GLint);
typedef void (APIENTRY *PFN_glColorMask_t)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (APIENTRY *PFN_glCullFace_t)(GLenum);
typedef void (APIENTRY *PFN_glDeleteTextures_t)(GLsizei, const GLuint*);
typedef void (APIENTRY *PFN_glDepthFunc_t)(GLenum);
typedef void (APIENTRY *PFN_glDepthMask_t)(GLboolean);
typedef void (APIENTRY *PFN_glDepthRange_t)(GLclampd, GLclampd);
typedef void (APIENTRY *PFN_glDisable_t)(GLenum);
typedef void (APIENTRY *PFN_glDrawArrays_t)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_glDrawBuffer_t)(GLenum);
typedef void (APIENTRY *PFN_glEnable_t)(GLenum);
typedef void (APIENTRY *PFN_glFogfv_t)(GLenum, const GLfloat*);
typedef void (APIENTRY *PFN_glGenTextures_t)(GLsizei, GLuint*);
typedef GLenum (APIENTRY *PFN_glGetError_t)(void);
typedef void (APIENTRY *PFN_glGetFloatv_t)(GLenum, GLfloat*);
typedef void (APIENTRY *PFN_glGetIntegerv_t)(GLenum, GLint*);
typedef const GLubyte* (APIENTRY *PFN_glGetString_t)(GLenum);
typedef void (APIENTRY *PFN_glGetTexImage_t)(GLenum, GLint, GLenum, GLenum, GLvoid*);
typedef void (APIENTRY *PFN_glPixelStorei_t)(GLenum, GLint);
typedef void (APIENTRY *PFN_glPointSize_t)(GLfloat);
typedef void (APIENTRY *PFN_glPolygonMode_t)(GLenum, GLenum);
typedef void (APIENTRY *PFN_glPolygonOffset_t)(GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glScissor_t)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glShadeModel_t)(GLenum);
typedef void (APIENTRY *PFN_glStencilFunc_t)(GLenum, GLint, GLuint);
typedef void (APIENTRY *PFN_glStencilMask_t)(GLuint);
typedef void (APIENTRY *PFN_glStencilOp_t)(GLenum, GLenum, GLenum);
typedef void (APIENTRY *PFN_glTexEnvf_t)(GLenum, GLenum, GLfloat);
typedef void (APIENTRY *PFN_glTexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
typedef void (APIENTRY *PFN_glTexParameterf_t)(GLenum, GLenum, GLfloat);
typedef void (APIENTRY *PFN_glTexParameterfv_t)(GLenum, GLenum, const GLfloat*);
typedef void (APIENTRY *PFN_glTexParameteri_t)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glTexSubImage2D_t)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid*);
typedef void (APIENTRY *PFN_glViewport_t)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glGenVertexArrays_t)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_glBindVertexArray_t)(GLuint);
typedef void (APIENTRY *PFN_glDeleteVertexArrays_t)(GLsizei, const GLuint*);

#define GL_PROC_LIST(X) \
    X(glActiveTexture, PFNGLACTIVETEXTUREPROC) \
    X(glAlphaFunc, PFN_glAlphaFunc_t) \
    X(glAttachObjectARB, PFNGLATTACHOBJECTARBPROC) \
    X(glAttachShader, PFNGLATTACHSHADERPROC) \
    X(glBeginQueryARB, PFNGLBEGINQUERYARBPROC) \
    X(glBindBufferARB, PFNGLBINDBUFFERARBPROC) \
    X(glBindFramebufferEXT, PFNGLBINDFRAMEBUFFEREXTPROC) \
    X(glBindProgramARB, PFNGLBINDPROGRAMARBPROC) \
    X(glBindRenderbufferEXT, PFNGLBINDRENDERBUFFEREXTPROC) \
    X(glBindSampler, PFNGLBINDSAMPLERPROC) \
    X(glBindTexture, PFN_glBindTexture_t) \
    X(glBlendEquation, PFNGLBLENDEQUATIONPROC) \
    X(glBlendFunc, PFN_glBlendFunc_t) \
    X(glBlitFramebufferEXT, PFNGLBLITFRAMEBUFFEREXTPROC) \
    X(glBufferDataARB, PFNGLBUFFERDATAARBPROC) \
    X(glBufferParameteriAPPLE, PFN_glBufferParameteriAPPLE_t) \
    X(glBufferSubDataARB, PFNGLBUFFERSUBDATAARBPROC) \
    X(glCheckFramebufferStatusEXT, PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC) \
    X(glClear, PFN_glClear_t) \
    X(glClearColor, PFN_glClearColor_t) \
    X(glClearDepth, PFN_glClearDepth_t) \
    X(glClearStencil, PFN_glClearStencil_t) \
    X(glColorMask, PFN_glColorMask_t) \
    X(glCompileShader, PFNGLCOMPILESHADERPROC) \
    X(glCompileShaderARB, PFNGLCOMPILESHADERARBPROC) \
    X(glCompressedTexImage2D, PFNGLCOMPRESSEDTEXIMAGE2DPROC) \
    X(glCompressedTexImage2DARB, PFNGLCOMPRESSEDTEXIMAGE2DARBPROC) \
    X(glCompressedTexImage3D, PFNGLCOMPRESSEDTEXIMAGE3DPROC) \
    X(glCompressedTexSubImage2D, PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC) \
    X(glCompressedTexSubImage3D, PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC) \
    X(glCreateProgram, PFNGLCREATEPROGRAMPROC) \
    X(glCreateProgramObjectARB, PFNGLCREATEPROGRAMOBJECTARBPROC) \
    X(glCreateShader, PFNGLCREATESHADERPROC) \
    X(glCreateShaderObjectARB, PFNGLCREATESHADEROBJECTARBPROC) \
    X(glCullFace, PFN_glCullFace_t) \
    X(glDebugMessageCallbackARB, PFN_glDebugMessageCallbackARB_t) \
    X(glDeleteBuffersARB, PFNGLDELETEBUFFERSARBPROC) \
    X(glDeleteFramebuffersEXT, PFNGLDELETEFRAMEBUFFERSEXTPROC) \
    X(glDeleteObjectARB, PFNGLDELETEOBJECTARBPROC) \
    X(glDeleteProgram, PFNGLDELETEPROGRAMPROC) \
    X(glDeleteProgramsARB, PFNGLDELETEPROGRAMSARBPROC) \
    X(glDeleteQueriesARB, PFNGLDELETEQUERIESARBPROC) \
    X(glDeleteRenderbuffersEXT, PFNGLDELETERENDERBUFFERSEXTPROC) \
    X(glDeleteSamplers, PFNGLDELETESAMPLERSPROC) \
    X(glDeleteShader, PFNGLDELETESHADERPROC) \
    X(glDeleteTextures, PFN_glDeleteTextures_t) \
    X(glDepthFunc, PFN_glDepthFunc_t) \
    X(glDepthMask, PFN_glDepthMask_t) \
    X(glDepthRange, PFN_glDepthRange_t) \
    X(glDisable, PFN_glDisable_t) \
    X(glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC) \
    X(glDisableVertexAttribArrayARB, PFNGLDISABLEVERTEXATTRIBARRAYARBPROC) \
    X(glDrawArrays, PFN_glDrawArrays_t) \
    X(glDrawBuffer, PFN_glDrawBuffer_t) \
    X(glDrawRangeElements, PFNGLDRAWRANGEELEMENTSPROC) \
    X(glEnable, PFN_glEnable_t) \
    X(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
    X(glEnableVertexAttribArrayARB, PFNGLENABLEVERTEXATTRIBARRAYARBPROC) \
    X(glEndQueryARB, PFNGLENDQUERYARBPROC) \
    X(glFlushMappedBufferRange, PFN_glFlushMappedBufferRange_t) \
    X(glFlushMappedBufferRangeAPPLE, PFN_glFlushMappedBufferRange_t) \
    X(glFogfv, PFN_glFogfv_t) \
    X(glFramebufferRenderbufferEXT, PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC) \
    X(glFramebufferTexture2DEXT, PFNGLFRAMEBUFFERTEXTURE2DEXTPROC) \
    X(glGenBuffersARB, PFNGLGENBUFFERSARBPROC) \
    X(glGenFramebuffersEXT, PFNGLGENFRAMEBUFFERSEXTPROC) \
    X(glGenProgramsARB, PFNGLGENPROGRAMSARBPROC) \
    X(glGenQueriesARB, PFNGLGENQUERIESARBPROC) \
    X(glGenRenderbuffersEXT, PFNGLGENRENDERBUFFERSEXTPROC) \
    X(glGenSamplers, PFNGLGENSAMPLERSPROC) \
    X(glGenTextures, PFN_glGenTextures_t) \
    X(glGenerateMipmapEXT, PFNGLGENERATEMIPMAPEXTPROC) \
    X(glGetAttribLocation, PFNGLGETATTRIBLOCATIONPROC) \
    X(glGetAttribLocationARB, PFNGLGETATTRIBLOCATIONARBPROC) \
    X(glGetCompressedTexImage, PFNGLGETCOMPRESSEDTEXIMAGEPROC) \
    X(glGetError, PFN_glGetError_t) \
    X(glGetFloatv, PFN_glGetFloatv_t) \
    X(glGetInfoLogARB, PFNGLGETINFOLOGARBPROC) \
    X(glGetIntegerv, PFN_glGetIntegerv_t) \
    X(glGetObjectParameterivARB, PFNGLGETOBJECTPARAMETERIVARBPROC) \
    X(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC) \
    X(glGetProgramiv, PFNGLGETPROGRAMIVPROC) \
    X(glGetProgramivARB, PFNGLGETPROGRAMIVARBPROC) \
    X(glGetQueryObjectuivARB, PFNGLGETQUERYOBJECTUIVARBPROC) \
    X(glGetShaderiv, PFNGLGETSHADERIVPROC) \
    X(glGetString, PFN_glGetString_t) \
    X(glGetTexImage, PFN_glGetTexImage_t) \
    X(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC) \
    X(glGetUniformLocationARB, PFNGLGETUNIFORMLOCATIONARBPROC) \
    X(glLinkProgram, PFNGLLINKPROGRAMPROC) \
    X(glLinkProgramARB, PFNGLLINKPROGRAMARBPROC) \
    X(glMapBufferARB, PFNGLMAPBUFFERARBPROC) \
    X(glMapBufferRange, PFNGLMAPBUFFERRANGEPROC) \
    X(glPixelStorei, PFN_glPixelStorei_t) \
    X(glPointParameterfARB, PFNGLPOINTPARAMETERFARBPROC) \
    X(glPointParameterfvARB, PFNGLPOINTPARAMETERFVARBPROC) \
    X(glPointSize, PFN_glPointSize_t) \
    X(glPolygonMode, PFN_glPolygonMode_t) \
    X(glPolygonOffset, PFN_glPolygonOffset_t) \
    X(glProgramLocalParameter4fvARB, PFNGLPROGRAMLOCALPARAMETER4FVARBPROC) \
    X(glProgramLocalParameterI4ivNV, PFN_glProgramLocalParameterI4ivNV_t) \
    X(glProgramStringARB, PFNGLPROGRAMSTRINGARBPROC) \
    X(glRenderbufferStorageEXT, PFNGLRENDERBUFFERSTORAGEEXTPROC) \
    X(glSamplerParameterf, PFNGLSAMPLERPARAMETERFPROC) \
    X(glSamplerParameterfv, PFNGLSAMPLERPARAMETERFVPROC) \
    X(glSamplerParameteri, PFNGLSAMPLERPARAMETERIPROC) \
    X(glScissor, PFN_glScissor_t) \
    X(glShadeModel, PFN_glShadeModel_t) \
    X(glShaderSource, PFNGLSHADERSOURCEPROC) \
    X(glShaderSourceARB, PFNGLSHADERSOURCEARBPROC) \
    X(glStencilFunc, PFN_glStencilFunc_t) \
    X(glStencilMask, PFN_glStencilMask_t) \
    X(glStencilOp, PFN_glStencilOp_t) \
    X(glTexEnvf, PFN_glTexEnvf_t) \
    X(glTexImage2D, PFN_glTexImage2D_t) \
    X(glTexImage3D, PFNGLTEXIMAGE3DPROC) \
    X(glTexParameterf, PFN_glTexParameterf_t) \
    X(glTexParameterfv, PFN_glTexParameterfv_t) \
    X(glTexParameteri, PFN_glTexParameteri_t) \
    X(glTexSubImage2D, PFN_glTexSubImage2D_t) \
    X(glTexSubImage3D, PFNGLTEXSUBIMAGE3DPROC) \
    X(glUniform1i, PFNGLUNIFORM1IPROC) \
    X(glUniform1iARB, PFNGLUNIFORM1IARBPROC) \
    X(glUniform1iv, PFNGLUNIFORM1IVPROC) \
    X(glUniform1ivARB, PFNGLUNIFORM1IVARBPROC) \
    X(glUniform4fv, PFNGLUNIFORM4FVPROC) \
    X(glUniform4fvARB, PFNGLUNIFORM4FVARBPROC) \
    X(glUniform4iv, PFNGLUNIFORM4IVPROC) \
    X(glUniform4ivARB, PFNGLUNIFORM4IVARBPROC) \
    X(glUnmapBufferARB, PFNGLUNMAPBUFFERARBPROC) \
    X(glUseProgram, PFNGLUSEPROGRAMPROC) \
    X(glUseProgramObjectARB, PFNGLUSEPROGRAMOBJECTARBPROC) \
    X(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC) \
    X(glVertexAttribPointerARB, PFNGLVERTEXATTRIBPOINTERARBPROC) \
    X(glViewport, PFN_glViewport_t)

#define DECLARE_PFN(name, type) static type pfn_##name = NULL;
GL_PROC_LIST(DECLARE_PFN)
#undef DECLARE_PFN
static PFN_glGenVertexArrays_t    pfn_glGenVertexArrays = NULL;
static PFN_glBindVertexArray_t    pfn_glBindVertexArray = NULL;
static PFN_glDeleteVertexArrays_t pfn_glDeleteVertexArrays = NULL;

static bool g_gl_procs_loaded = false;
#ifndef __SWITCH__
static uint64_t g_null_gl_start = 0;
extern uint64_t recomp_sdl_get_frame_count(void);
static inline bool null_gl_active(void) {
    return g_null_gl_start > 0 && recomp_sdl_get_frame_count() >= g_null_gl_start;
}
#endif

static void ensure_gl_loaded(void) {
    if (g_gl_procs_loaded) return;
    g_gl_procs_loaded = true;
#ifndef __SWITCH__
    {
        const char* ng = getenv("RECOMP_NULL_GL");
        if (ng && atoll(ng) > 0) g_null_gl_start = (uint64_t)atoll(ng);
    }
#endif
    #define LOAD_PFN(name, type) pfn_##name = (type)host_gl_get_proc(#name);
    GL_PROC_LIST(LOAD_PFN)
    #undef LOAD_PFN
    pfn_glGenVertexArrays = (PFN_glGenVertexArrays_t)host_gl_get_proc("glGenVertexArrays");
    if (!pfn_glGenVertexArrays) pfn_glGenVertexArrays = (PFN_glGenVertexArrays_t)host_gl_get_proc("glGenVertexArraysARB");
    pfn_glBindVertexArray = (PFN_glBindVertexArray_t)host_gl_get_proc("glBindVertexArray");
    if (!pfn_glBindVertexArray) pfn_glBindVertexArray = (PFN_glBindVertexArray_t)host_gl_get_proc("glBindVertexArrayARB");
    pfn_glDeleteVertexArrays = (PFN_glDeleteVertexArrays_t)host_gl_get_proc("glDeleteVertexArrays");
    if (!pfn_glDeleteVertexArrays) pfn_glDeleteVertexArrays = (PFN_glDeleteVertexArrays_t)host_gl_get_proc("glDeleteVertexArraysARB");
#ifndef __SWITCH__
    /* Core GL 1.x fallback in case getProcAddress omits static GL symbols */
    if (!pfn_glAlphaFunc) pfn_glAlphaFunc = glAlphaFunc;
    if (!pfn_glBindTexture) pfn_glBindTexture = glBindTexture;
    if (!pfn_glBlendFunc) pfn_glBlendFunc = glBlendFunc;
    if (!pfn_glClear) pfn_glClear = glClear;
    if (!pfn_glClearColor) pfn_glClearColor = glClearColor;
    if (!pfn_glClearDepth) pfn_glClearDepth = glClearDepth;
    if (!pfn_glClearStencil) pfn_glClearStencil = glClearStencil;
    if (!pfn_glColorMask) pfn_glColorMask = glColorMask;
    if (!pfn_glCullFace) pfn_glCullFace = glCullFace;
    if (!pfn_glDeleteTextures) pfn_glDeleteTextures = glDeleteTextures;
    if (!pfn_glDepthFunc) pfn_glDepthFunc = glDepthFunc;
    if (!pfn_glDepthMask) pfn_glDepthMask = glDepthMask;
    if (!pfn_glDepthRange) pfn_glDepthRange = glDepthRange;
    if (!pfn_glDisable) pfn_glDisable = glDisable;
    if (!pfn_glDrawArrays) pfn_glDrawArrays = glDrawArrays;
    if (!pfn_glDrawBuffer) pfn_glDrawBuffer = glDrawBuffer;
    if (!pfn_glEnable) pfn_glEnable = glEnable;
    if (!pfn_glFogfv) pfn_glFogfv = glFogfv;
    if (!pfn_glGenTextures) pfn_glGenTextures = glGenTextures;
    if (!pfn_glGetError) pfn_glGetError = glGetError;
    if (!pfn_glGetFloatv) pfn_glGetFloatv = glGetFloatv;
    if (!pfn_glGetIntegerv) pfn_glGetIntegerv = glGetIntegerv;
    if (!pfn_glGetString) pfn_glGetString = glGetString;
    if (!pfn_glGetTexImage) pfn_glGetTexImage = glGetTexImage;
    if (!pfn_glPixelStorei) pfn_glPixelStorei = glPixelStorei;
    if (!pfn_glPointSize) pfn_glPointSize = glPointSize;
    if (!pfn_glPolygonMode) pfn_glPolygonMode = glPolygonMode;
    if (!pfn_glPolygonOffset) pfn_glPolygonOffset = glPolygonOffset;
    if (!pfn_glScissor) pfn_glScissor = glScissor;
    if (!pfn_glShadeModel) pfn_glShadeModel = glShadeModel;
    if (!pfn_glStencilFunc) pfn_glStencilFunc = glStencilFunc;
    if (!pfn_glStencilMask) pfn_glStencilMask = glStencilMask;
    if (!pfn_glStencilOp) pfn_glStencilOp = glStencilOp;
    if (!pfn_glTexEnvf) pfn_glTexEnvf = glTexEnvf;
    if (!pfn_glTexImage2D) pfn_glTexImage2D = glTexImage2D;
    if (!pfn_glTexParameterf) pfn_glTexParameterf = glTexParameterf;
    if (!pfn_glTexParameterfv) pfn_glTexParameterfv = glTexParameterfv;
    if (!pfn_glTexParameteri) pfn_glTexParameteri = glTexParameteri;
    if (!pfn_glTexSubImage2D) pfn_glTexSubImage2D = glTexSubImage2D;
    if (!pfn_glViewport) pfn_glViewport = glViewport;
#endif
}

/* ---------------- Async GL Worker Thread ---------------- */
void recomp_gl_set_backbuffer_scale(int guest_w, int guest_h, int host_w, int host_h);
static float query_hw_max_aniso(void);
#include "runtime/gl_thread_impl.h"

/* ---------------- Thunk Implementations ---------------- */

void recomp_gl_active_texture(uint32_t unit) {
    if (g_cache_active_tex == unit) return;
    g_cache_active_tex = unit;
    if (g_glt_active) { glq_push_1u(GLCMD_ACTIVE_TEXTURE, unit); return; }
    if (pfn_glActiveTexture) pfn_glActiveTexture(unit);
}
static void glw_glActiveTexture(CPUContext* c) {
    recomp_gl_active_texture(arg32(c,0));
}
static void glw_glAlphaFunc(CPUContext* c) {
    GLenum fn = arg32(c,0); GLclampf ref = argf32(c,1);
    if (g_cache_alpha_fn == fn && g_cache_alpha_ref == ref) return;
    g_cache_alpha_fn = fn; g_cache_alpha_ref = ref;
    if (g_glt_active) {
        GlCmd1U1F* cmd = (GlCmd1U1F*)glq_alloc(sizeof(GlCmd1U1F));
        cmd->h.op = GLCMD_ALPHA_FUNC; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd1U1F) + 7u) & ~7u;
        cmd->a = fn; cmd->f0 = ref; glq_commit(); return;
    }
    pfn_glAlphaFunc(fn, ref);
}
static void glw_glAttachObjectARB_dir(CPUContext* c) { if (pfn_glAttachObjectARB) pfn_glAttachObjectARB(arg32(c,0), arg32(c,1)); }
static void glw_glAttachObjectARB(CPUContext* c) { if (g_glt_active) { glq_sync_call(glw_glAttachObjectARB_dir, c, "glw_glAttachObjectARB_dir"); return; } glw_glAttachObjectARB_dir(c); }
static void glw_glAttachShader_dir(CPUContext* c) { if (pfn_glAttachShader) pfn_glAttachShader(arg32(c,0), arg32(c,1)); }
static void glw_glAttachShader(CPUContext* c) { if (g_glt_active) { glq_sync_call(glw_glAttachShader_dir, c, "glw_glAttachShader_dir"); return; } glw_glAttachShader_dir(c); }
static void glw_glBeginQueryARB(CPUContext* c) {
    GLenum t = arg32(c,0); GLuint id = arg32(c,1);
    if (g_glt_active) { glq_push_2u(GLCMD_BEGIN_QUERY, t, id); return; }
    if (pfn_glBeginQueryARB) pfn_glBeginQueryARB(t, id);
}

void recomp_gl_bind_buffer(uint32_t target, uint32_t buf) {
    if (__builtin_expect(target == GL_ARRAY_BUFFER, 1)) {
        if (g_array_buf_valid && g_bound_array_buf == buf) return;
        g_bound_array_buf = buf;
        g_array_buf_valid = true;
        if (__builtin_expect(g_glt_active, 1)) { glq_bind_array_buffer(buf); return; }
        if (pfn_glBindBufferARB) pfn_glBindBufferARB(GL_ARRAY_BUFFER, buf);
        return;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        if (g_elem_buf_valid && g_bound_elem_buf == buf) return;
        g_bound_elem_buf = buf;
        g_elem_buf_valid = true;
        if (__builtin_expect(g_glt_active, 1)) { glq_bind_elem_buffer(buf); return; }
        if (pfn_glBindBufferARB) pfn_glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER, buf);
        return;
    }
    if (target == GL_PIXEL_UNPACK_BUFFER) {
        if (g_pbo_unpack_valid && g_bound_pbo_unpack == buf) return;
        g_bound_pbo_unpack = buf;
        g_pbo_unpack_valid = true;
    } else if (target == GL_PIXEL_PACK_BUFFER) {
        if (g_pbo_pack_valid && g_bound_pbo_pack == buf) return;
        g_bound_pbo_pack = buf;
        g_pbo_pack_valid = true;
    } else {
        *target_binding_ptr(target) = buf;
    }
    if (__builtin_expect(g_glt_active, 1)) { glq_push_2u(GLCMD_BIND_BUFFER, target, buf); return; }
    if (pfn_glBindBufferARB) pfn_glBindBufferARB(target, buf);
}
static void glw_glBindBufferARB(CPUContext* c) {
    recomp_gl_bind_buffer(arg32(c,0), arg32(c,1));
}

static void glw_glBindFramebufferEXT(CPUContext* c) {
    GLenum target = arg32(c, 0);
    GLuint fbo = arg32(c, 1);
    if (target == GL_FRAMEBUFFER_EXT) {
        if (g_cache_fbo_valid && g_bound_draw_fbo == fbo && g_bound_read_fbo == fbo) return;
        g_bound_draw_fbo = fbo;
        g_bound_read_fbo = fbo;
        g_cache_fbo_valid = true;
    } else if (target == GL_DRAW_FRAMEBUFFER_EXT) {
        if (g_cache_fbo_valid && g_bound_draw_fbo == fbo) return;
        g_bound_draw_fbo = fbo;
    } else if (target == GL_READ_FRAMEBUFFER_EXT) {
        if (g_cache_fbo_valid && g_bound_read_fbo == fbo) return;
        g_bound_read_fbo = fbo;
    }
    if (g_glt_active) { glq_push_2u(GLCMD_BIND_FRAMEBUFFER, target, fbo); return; }
    if (pfn_glBindFramebufferEXT) pfn_glBindFramebufferEXT(target, fbo);
}
static void glw_glBindProgramARB(CPUContext* c) {
    GLenum t = arg32(c,0); GLuint id = arg32(c,1);
    if (g_glt_active) { glq_push_2u(GLCMD_BIND_PROGRAM_ARB, t, id); return; }
    if (pfn_glBindProgramARB) pfn_glBindProgramARB(t, id);
}
static void glw_glBindRenderbufferEXT(CPUContext* c) {
    GLenum t = arg32(c,0); GLuint id = arg32(c,1);
    if (g_glt_active) { glq_push_2u(GLCMD_BIND_RENDERBUFFER, t, id); return; }
    if (pfn_glBindRenderbufferEXT) pfn_glBindRenderbufferEXT(t, id);
}
static void glw_glBindSampler(CPUContext* c) {
    GLuint unit = arg32(c,0), s = arg32(c,1);
    if (unit < GL_CACHE_MAX_TEX_UNITS) {
        if (g_cache_sampler_valid[unit] && g_cache_sampler[unit] == s) return;
        g_cache_sampler[unit] = s;
        g_cache_sampler_valid[unit] = true;
    }
    if (g_glt_active) { glq_push_2u(GLCMD_BIND_SAMPLER, unit, s); return; }
    if (pfn_glBindSampler) pfn_glBindSampler(unit, s);
}
void recomp_gl_bind_texture(uint32_t target, uint32_t tex) {
    int tslot = __builtin_expect(target == GL_TEXTURE_2D, 1) ? 0 : gl_tex_enable_slot(target);
    if (__builtin_expect(tslot >= 0 && g_cache_active_tex >= GL_TEXTURE0, 1)) {
        uint32_t u = g_cache_active_tex - GL_TEXTURE0;
        if (__builtin_expect(u < GL_CACHE_MAX_TEX_UNITS, 1)) {
            if (g_cache_tex_valid[tslot][u] && g_cache_tex[tslot][u] == tex) return;
            g_cache_tex[tslot][u] = tex;
            g_cache_tex_valid[tslot][u] = true;
        }
    }
    if (__builtin_expect(g_glt_active, 1)) { glq_push_2u(GLCMD_BIND_TEXTURE, target, tex); return; }
    pfn_glBindTexture(target, tex);
}
static void glw_glBindTexture(CPUContext* c) {
    recomp_gl_bind_texture(arg32(c,0), arg32(c,1));
}
static void glw_glBlendEquation(CPUContext* c) {
    GLenum m = arg32(c,0);
    if (g_cache_blend_eq == m) return;
    g_cache_blend_eq = m;
    if (g_glt_active) { glq_push_1u(GLCMD_BLEND_EQUATION, m); return; }
    if (pfn_glBlendEquation) pfn_glBlendEquation(m);
}
static void glw_glBlendFunc(CPUContext* c) {
    GLenum sf = arg32(c,0), df = arg32(c,1);
    if (g_cache_blend_src == sf && g_cache_blend_dst == df) return;
    g_cache_blend_src = sf; g_cache_blend_dst = df;
    if (g_glt_active) { glq_push_2u(GLCMD_BLEND_FUNC, sf, df); return; }
    pfn_glBlendFunc(sf, df);
}

static void glw_glBlitFramebufferEXT(CPUContext* c) {
#ifndef __SWITCH__
    if (null_gl_active()) return;
#endif
    if (!pfn_glBlitFramebufferEXT) return;
    GLint sx0 = (GLint)arg32(c,0), sy0 = (GLint)arg32(c,1);
    GLint sx1 = (GLint)arg32(c,2), sy1 = (GLint)arg32(c,3);
    GLint dx0 = (GLint)arg32(c,4), dy0 = (GLint)arg32(c,5);
    GLint dx1 = (GLint)arg32(c,6), dy1 = (GLint)arg32(c,7);
    if (bb_scale_active()) {
        if (g_bound_read_fbo == 0) {
            sx0 = scale_bb_coord(sx0, g_bb_host_w, g_bb_guest_w);
            sy0 = scale_bb_coord(sy0, g_bb_host_h, g_bb_guest_h);
            sx1 = scale_bb_coord(sx1, g_bb_host_w, g_bb_guest_w);
            sy1 = scale_bb_coord(sy1, g_bb_host_h, g_bb_guest_h);
        }
        if (g_bound_draw_fbo == 0) {
            dx0 = scale_bb_coord(dx0, g_bb_host_w, g_bb_guest_w);
            dy0 = scale_bb_coord(dy0, g_bb_host_h, g_bb_guest_h);
            dx1 = scale_bb_coord(dx1, g_bb_host_w, g_bb_guest_w);
            dy1 = scale_bb_coord(dy1, g_bb_host_h, g_bb_guest_h);
        }
    }
    if (g_glt_active) {
        GlCmd10I* cmd = (GlCmd10I*)glq_alloc(sizeof(GlCmd10I));
        cmd->h.op = GLCMD_BLIT_FRAMEBUFFER; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd10I) + 7u) & ~7u;
        cmd->v[0]=sx0; cmd->v[1]=sy0; cmd->v[2]=sx1; cmd->v[3]=sy1;
        cmd->v[4]=dx0; cmd->v[5]=dy0; cmd->v[6]=dx1; cmd->v[7]=dy1;
        cmd->v[8]=(int32_t)arg32(c,8); cmd->v[9]=(int32_t)arg32(c,9);
        glq_commit();
        return;
    }
    pfn_glBlitFramebufferEXT(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, arg32(c,8), arg32(c,9));
}

static void glw_glBufferDataARB_dir(CPUContext* c) {
    GLenum target = arg32(c,0);
    GLsizeiptr sz = (GLsizeiptr)(uint32_t)arg32(c,1);
    if (pfn_glBufferDataARB) pfn_glBufferDataARB(target, sz, ptr32(arg32(c,2)), arg32(c,3));
}
static void glw_glBufferDataARB(CPUContext* c) {
    GLenum target = arg32(c,0);
    uint32_t sz = arg32(c,1);
    uint32_t data_p = arg32(c,2);
    GLenum usage = arg32(c,3);
    GLuint buf = *target_binding_ptr(target);
    if (buf < MAX_GL_BUFFERS) g_buf_sizes[buf] = sz;
    if (g_glt_active) {
        if (target == GL_ARRAY_BUFFER) glq_flush_array_unbind();
        else if (target == GL_ELEMENT_ARRAY_BUFFER) glq_flush_elem_unbind();
        if (data_p == 0) {
            GlCmdBufData* cmd = (GlCmdBufData*)glq_alloc(sizeof(GlCmdBufData));
            cmd->h.op = GLCMD_BUFFER_DATA; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmdBufData) + 7u) & ~7u;
            cmd->target = target; cmd->data_size = sz; cmd->usage = usage; cmd->has_data = 0;
            glq_commit();
            return;
        }
        if (sz <= GLQ_MAX_INLINE_DATA) {
            uint32_t total = (sizeof(GlCmdBufData) + sz + 7u) & ~7u;
            GlCmdBufData* cmd = (GlCmdBufData*)glq_alloc(total);
            cmd->h.op = GLCMD_BUFFER_DATA; cmd->h.flags = 0; cmd->h.size = total;
            cmd->target = target; cmd->data_size = sz; cmd->usage = usage; cmd->has_data = 1;
            memcpy(cmd + 1, ptr32(data_p), sz);
            glq_commit();
            return;
        }
        glq_sync_call(glw_glBufferDataARB_dir, c, "glw_glBufferDataARB_dir");
        return;
    }
    glw_glBufferDataARB_dir(c);
}

static void glw_glBufferParameteriAPPLE(CPUContext* c) {
    GLenum t = arg32(c,0), p = arg32(c,1); GLint v = (GLint)arg32(c,2);
    if (g_glt_active) {
        if (t == GL_ARRAY_BUFFER) glq_flush_array_unbind();
        else if (t == GL_ELEMENT_ARRAY_BUFFER) glq_flush_elem_unbind();
        glq_push_3u(GLCMD_BUFFER_PARAM_APPLE, t, p, (uint32_t)v);
        return;
    }
    if (pfn_glBufferParameteriAPPLE) pfn_glBufferParameteriAPPLE(t, p, v);
}

static void glw_glBufferSubDataARB(CPUContext* c) {
    GLenum target = arg32(c,0);
    int32_t off = (int32_t)arg32(c,1);
    uint32_t sz = arg32(c,2);
    const void* src = ptr32(arg32(c,3));
    if (g_glt_active) {
        if (src && sz > 0) glq_push_buf_sub_data(target, off, sz, src);
        return;
    }
    if (pfn_glBufferSubDataARB) pfn_glBufferSubDataARB(target, (GLintptr)off, (GLsizeiptr)sz, src);
}

static void glw_glCheckFramebufferStatusEXT_dir(CPUContext* c) {
    c->eax = pfn_glCheckFramebufferStatusEXT ? pfn_glCheckFramebufferStatusEXT(arg32(c,0)) : GL_FRAMEBUFFER_COMPLETE_EXT;
}
static void glw_glCheckFramebufferStatusEXT(CPUContext* c) {
    if (g_glt_active) { c->eax = GL_FRAMEBUFFER_COMPLETE_EXT; return; }
    glw_glCheckFramebufferStatusEXT_dir(c);
}

static void glw_glClear(CPUContext* c) {
    GLbitfield m = arg32(c,0);
    if (g_glt_active) { glq_push_1u(GLCMD_CLEAR, m); return; }
    pfn_glClear(m);
}
static void glw_glClearColor(CPUContext* c) {
    float r=argf32(c,0), g=argf32(c,1), b=argf32(c,2), a=argf32(c,3);
    if (g_glt_active) {
        GlCmd4F* cmd = (GlCmd4F*)glq_alloc(sizeof(GlCmd4F));
        cmd->h.op = GLCMD_CLEAR_COLOR; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd4F) + 7u) & ~7u;
        cmd->f[0]=r; cmd->f[1]=g; cmd->f[2]=b; cmd->f[3]=a; glq_commit(); return;
    }
    pfn_glClearColor(r, g, b, a);
}
static void glw_glClearDepth(CPUContext* c) {
    double d = argf64(c,0);
    if (g_glt_active) {
        GlCmd1D* cmd = (GlCmd1D*)glq_alloc(sizeof(GlCmd1D));
        cmd->h.op = GLCMD_CLEAR_DEPTH; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd1D) + 7u) & ~7u;
        cmd->d0 = d; glq_commit(); return;
    }
    pfn_glClearDepth(d);
}
static void glw_glClearStencil(CPUContext* c) {
    GLint s = (GLint)arg32(c,0);
    if (g_glt_active) { glq_push_1u(GLCMD_CLEAR_STENCIL, (uint32_t)s); return; }
    pfn_glClearStencil(s);
}
static void glw_glColorMask(CPUContext* c) {
    GLboolean r = (GLboolean)arg32(c,0), g = (GLboolean)arg32(c,1), b = (GLboolean)arg32(c,2), a = (GLboolean)arg32(c,3);
    if (g_cache_cmask[0] == (int8_t)(r != 0) && g_cache_cmask[1] == (int8_t)(g != 0) &&
        g_cache_cmask[2] == (int8_t)(b != 0) && g_cache_cmask[3] == (int8_t)(a != 0)) return;
    g_cache_cmask[0] = (int8_t)(r != 0); g_cache_cmask[1] = (int8_t)(g != 0);
    g_cache_cmask[2] = (int8_t)(b != 0); g_cache_cmask[3] = (int8_t)(a != 0);
    if (g_glt_active) { glq_push_4u(GLCMD_COLOR_MASK, r, g, b, a); return; }
    pfn_glColorMask(r, g, b, a);
}
extern bool recomp_sdl_is_boot_loading(void);
extern void recomp_sdl_notify_boot_progress(int stage, int count);
static int g_boot_tex_count = 0;
static int g_boot_shader_count = 0;

#define GLW_SYNC(name, body) \
static void glw_##name##_dir(CPUContext* c) { body } \
static void glw_##name(CPUContext* c) { if (g_glt_active) { glq_sync_call(glw_##name##_dir, c, #name); return; } glw_##name##_dir(c); }

GLW_SYNC(glCompileShader, {
    if (pfn_glCompileShader) pfn_glCompileShader(arg32(c,0));
    if (recomp_sdl_is_boot_loading()) recomp_sdl_notify_boot_progress(2, ++g_boot_shader_count);
})
GLW_SYNC(glCompileShaderARB, {
    if (pfn_glCompileShaderARB) pfn_glCompileShaderARB(arg32(c,0));
    if (recomp_sdl_is_boot_loading()) recomp_sdl_notify_boot_progress(2, ++g_boot_shader_count);
})

static inline const void* pbo_or_ptr(uint32_t p32) {
    return (g_bound_pbo_unpack != 0) ? (const void*)(uintptr_t)p32 : ptr32(p32);
}

GLW_SYNC(glCompressedTexImage2D, {
    GLint level = (GLint)arg32(c,1);
    if (pfn_glCompressedTexImage2D)
        pfn_glCompressedTexImage2D(arg32(c,0), level, arg32(c,2), (GLsizei)arg32(c,3),
                                   (GLsizei)arg32(c,4), (GLint)arg32(c,5), (GLsizei)arg32(c,6), pbo_or_ptr(arg32(c,7)));
    if (level == 0 && recomp_sdl_is_boot_loading()) recomp_sdl_notify_boot_progress(1, ++g_boot_tex_count);
})

GLW_SYNC(glCompressedTexImage2DARB, {
    GLint level = (GLint)arg32(c,1);
    if (pfn_glCompressedTexImage2DARB)
        pfn_glCompressedTexImage2DARB(arg32(c,0), level, arg32(c,2), (GLsizei)arg32(c,3),
                                      (GLsizei)arg32(c,4), (GLint)arg32(c,5), (GLsizei)arg32(c,6), pbo_or_ptr(arg32(c,7)));
    if (level == 0 && recomp_sdl_is_boot_loading()) recomp_sdl_notify_boot_progress(1, ++g_boot_tex_count);
})

GLW_SYNC(glCompressedTexImage3D, {
    if (pfn_glCompressedTexImage3D)
        pfn_glCompressedTexImage3D(arg32(c,0), (GLint)arg32(c,1), arg32(c,2), (GLsizei)arg32(c,3),
                                   (GLsizei)arg32(c,4), (GLsizei)arg32(c,5), (GLint)arg32(c,6),
                                   (GLsizei)arg32(c,7), pbo_or_ptr(arg32(c,8)));
})

GLW_SYNC(glCompressedTexSubImage2D, {
    if (pfn_glCompressedTexSubImage2D)
        pfn_glCompressedTexSubImage2D(arg32(c,0), (GLint)arg32(c,1), (GLint)arg32(c,2), (GLint)arg32(c,3),
                                      (GLsizei)arg32(c,4), (GLsizei)arg32(c,5), arg32(c,6),
                                      (GLsizei)arg32(c,7), pbo_or_ptr(arg32(c,8)));
})

GLW_SYNC(glCompressedTexSubImage3D, {
    if (pfn_glCompressedTexSubImage3D)
        pfn_glCompressedTexSubImage3D(arg32(c,0), (GLint)arg32(c,1), (GLint)arg32(c,2), (GLint)arg32(c,3),
                                      (GLint)arg32(c,4), (GLsizei)arg32(c,5), (GLsizei)arg32(c,6),
                                      (GLsizei)arg32(c,7), arg32(c,8), (GLsizei)arg32(c,9), pbo_or_ptr(arg32(c,10)));
})

GLW_SYNC(glCreateProgram, { c->eax = pfn_glCreateProgram ? pfn_glCreateProgram() : 0; })
GLW_SYNC(glCreateProgramObjectARB, { c->eax = pfn_glCreateProgramObjectARB ? (uint32_t)pfn_glCreateProgramObjectARB() : 0; })
GLW_SYNC(glCreateShader, { c->eax = pfn_glCreateShader ? pfn_glCreateShader(arg32(c,0)) : 0; })
GLW_SYNC(glCreateShaderObjectARB, { c->eax = pfn_glCreateShaderObjectARB ? (uint32_t)pfn_glCreateShaderObjectARB(arg32(c,0)) : 0; })
static void glw_glCullFace(CPUContext* c) {
    GLenum mode = arg32(c,0);
    if (g_cache_cull_face == mode) return;
    g_cache_cull_face = mode;
    if (g_glt_active) { glq_push_1u(GLCMD_CULL_FACE, mode); return; }
    pfn_glCullFace(mode);
}
static void glw_glDebugMessageCallbackARB(CPUContext* c) { (void)c; /* Suppress guest->host callback pointer mismatch */ }
GLW_SYNC(glDeleteBuffersARB, {
    GLsizei n = (GLsizei)arg32(c,0);
    const GLuint* bufs = (const GLuint*)ptr32(arg32(c,1));
    if (bufs && n > 0) {
        for (GLsizei i = 0; i < n; i++) {
            GLuint id = bufs[i];
            if (!id) continue;
            if (g_bound_array_buf == id) g_bound_array_buf = 0;
            if (g_bound_elem_buf == id)  g_bound_elem_buf = 0;
            if (g_bound_pbo_unpack == id) g_bound_pbo_unpack = 0;
            if (g_bound_pbo_pack == id)   g_bound_pbo_pack = 0;
            if (g_glt_log_array_buf == id) g_glt_log_array_buf = 0;
            if (g_glt_log_elem_buf == id)  g_glt_log_elem_buf = 0;
            for (int a = 0; a < GLT_MAX_VAA; a++) {
                if (g_glt_log_attrs[a].valid && g_glt_log_attrs[a].buf == id) {
                    memset(&g_glt_log_attrs[a], 0, sizeof(g_glt_log_attrs[a]));
                }
            }
        }
    }
    g_array_buf_valid = false;
    g_elem_buf_valid = false;
    g_pbo_unpack_valid = false;
    g_pbo_pack_valid = false;
    g_glq_array_binding_known = false;
    g_glq_elem_binding_known = false;
    g_glq_array_unbind_pending = false;
    g_glq_elem_unbind_pending = false;
    glt_vao_cache_invalidate_all();
    for (int i = 0; i < GL_CACHE_MAX_VAA; i++) g_cache_vap[i].valid = false;
    if (pfn_glDeleteBuffersARB) pfn_glDeleteBuffersARB(n, bufs);
})
GLW_SYNC(glDeleteFramebuffersEXT, {
    GLsizei n = (GLsizei)arg32(c,0);
    const GLuint* fbos = (const GLuint*)ptr32(arg32(c,1));
    if (fbos && n > 0) {
        for (GLsizei i = 0; i < n; i++) {
            if (fbos[i] && fbos[i] == g_bound_draw_fbo) { g_bound_draw_fbo = 0; g_cache_fbo_valid = false; }
            if (fbos[i] && fbos[i] == g_bound_read_fbo) { g_bound_read_fbo = 0; g_cache_fbo_valid = false; }
        }
    }
    if (pfn_glDeleteFramebuffersEXT) pfn_glDeleteFramebuffersEXT(n, fbos);
})
GLW_SYNC(glDeleteObjectARB, { g_cache_prog = 0xFFFFFFFFu; u4fv_cache_invalidate(); if (pfn_glDeleteObjectARB) pfn_glDeleteObjectARB((GLhandleARB)arg32(c,0)); })
GLW_SYNC(glDeleteProgram, { g_cache_prog = 0xFFFFFFFFu; u4fv_cache_invalidate(); if (pfn_glDeleteProgram) pfn_glDeleteProgram(arg32(c,0)); })
GLW_SYNC(glDeleteProgramsARB, { if (pfn_glDeleteProgramsARB) pfn_glDeleteProgramsARB((GLsizei)arg32(c,0), (const GLuint*)ptr32(arg32(c,1))); })
GLW_SYNC(glDeleteQueriesARB, { if (pfn_glDeleteQueriesARB) pfn_glDeleteQueriesARB((GLsizei)arg32(c,0), (const GLuint*)ptr32(arg32(c,1))); })
GLW_SYNC(glDeleteRenderbuffersEXT, { if (pfn_glDeleteRenderbuffersEXT) pfn_glDeleteRenderbuffersEXT((GLsizei)arg32(c,0), (const GLuint*)ptr32(arg32(c,1))); })
GLW_SYNC(glDeleteSamplers, {
    GLsizei n = (GLsizei)arg32(c,0);
    const GLuint* ss = (const GLuint*)ptr32(arg32(c,1));
    if (ss && n > 0) {
        for (GLsizei i = 0; i < n; i++) {
            if (ss[i] < MAX_SAMPLER_TRACK) g_cache_sparam_valid[ss[i]] = 0;
        }
    }
    memset(g_cache_sampler_valid, 0, sizeof(g_cache_sampler_valid));
    if (pfn_glDeleteSamplers) pfn_glDeleteSamplers(n, ss);
})
GLW_SYNC(glDeleteShader, { if (pfn_glDeleteShader) pfn_glDeleteShader(arg32(c,0)); })
GLW_SYNC(glDeleteTextures, {
    GLsizei n = (GLsizei)arg32(c,0);
    const GLuint* ts = (const GLuint*)ptr32(arg32(c,1));
    if (ts && n > 0) {
        for (GLsizei i = 0; i < n; i++) {
            if (ts[i] < MAX_TEX_PARAM_TRACK) g_cache_tparam_valid[ts[i]] = 0;
        }
    }
    memset(g_cache_tex_valid, 0, sizeof(g_cache_tex_valid));
    pfn_glDeleteTextures(n, ts);
})
static void glw_glDepthFunc(CPUContext* c) {
    GLenum fn = arg32(c,0);
    if (g_cache_depth_func == fn) return;
    g_cache_depth_func = fn;
    if (g_glt_active) { glq_push_1u(GLCMD_DEPTH_FUNC, fn); return; }
    pfn_glDepthFunc(fn);
}
static void glw_glDepthMask(CPUContext* c) {
    GLboolean m = (GLboolean)arg32(c,0);
    int8_t v = (m != 0) ? 1 : 0;
    if (g_cache_depth_mask == v) return;
    g_cache_depth_mask = v;
    if (g_glt_active) { glq_push_1u(GLCMD_DEPTH_MASK, m); return; }
    pfn_glDepthMask(m);
}
static void glw_glDepthRange(CPUContext* c) {
    double n = argf64(c,0), f = argf64(c,8);
    if (g_glt_active) {
        GlCmd2D* cmd = (GlCmd2D*)glq_alloc(sizeof(GlCmd2D));
        cmd->h.op = GLCMD_DEPTH_RANGE; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd2D) + 7u) & ~7u;
        cmd->d0 = n; cmd->d1 = f; glq_commit(); return;
    }
    pfn_glDepthRange(n, f);
}
void recomp_gl_disable(uint32_t cap) {
    int s = gl_cap_slot(cap);
    if (s >= 0) {
        if (g_cache_caps[s] == 0) return;
        g_cache_caps[s] = 0;
    } else {
        int ts = gl_tex_enable_slot(cap);
        if (ts >= 0 && g_cache_active_tex >= GL_TEXTURE0) {
            uint32_t unit = (uint32_t)(g_cache_active_tex - GL_TEXTURE0);
            if (unit < GL_CACHE_MAX_TEX_UNITS) {
                if (g_cache_tex_enable[unit][ts] == 1) return;
                g_cache_tex_enable[unit][ts] = 1;
            }
        }
    }
    if (g_glt_active) { glq_push_1u(GLCMD_DISABLE, cap); return; }
    pfn_glDisable(cap);
}
static void glw_glDisable(CPUContext* c) {
    recomp_gl_disable(arg32(c,0));
}
static void glw_glDisableVertexAttribArray(CPUContext* c) {
    GLuint idx = arg32(c,0);
    if (idx < 32u) {
        uint32_t bit = 1u << idx;
        if ((g_cache_vaa_known & bit) && !(g_cache_vaa_enabled & bit)) return;
        g_cache_vaa_known |= bit;
        g_cache_vaa_enabled &= ~bit;
    }
    if (g_glt_active) { glq_push_2u(GLCMD_DISABLE_VAA, idx, 0); return; }
    if (pfn_glDisableVertexAttribArray) pfn_glDisableVertexAttribArray(idx);
}
void recomp_gl_disable_vaa_arb(uint32_t idx) {
    if (idx < 32u) {
        uint32_t bit = 1u << idx;
        if ((g_cache_vaa_known & bit) && !(g_cache_vaa_enabled & bit)) return;
        g_cache_vaa_known |= bit;
        g_cache_vaa_enabled &= ~bit;
    }
    if (g_glt_active) { glq_push_2u(GLCMD_DISABLE_VAA, idx, 1); return; }
    if (pfn_glDisableVertexAttribArrayARB) pfn_glDisableVertexAttribArrayARB(idx);
}
static void glw_glDisableVertexAttribArrayARB(CPUContext* c) {
    recomp_gl_disable_vaa_arb(arg32(c,0));
}
static void glw_glDrawArrays_dir(CPUContext* c) {
    pfn_glDrawArrays(arg32(c,0), (GLint)arg32(c,1), (GLsizei)arg32(c,2));
}
static void glw_glDrawArrays(CPUContext* c) {
#ifndef __SWITCH__
    if (null_gl_active()) return;
#endif
    GLenum mode = arg32(c,0); GLint first = (GLint)arg32(c,1); GLsizei count = (GLsizei)arg32(c,2);
    if (g_glt_active) {
        uint32_t active_client = g_client_vap_mask & g_cache_vaa_enabled;
        if (active_client != 0) {
            if (count <= 0 || first < 0) return;
            uint32_t num_attrs = 0;
            uint32_t attr_idxs[GL_CACHE_MAX_VAA];
            uint32_t attr_bytes[GL_CACHE_MAX_VAA];
            uint32_t attr_eff_stride[GL_CACHE_MAX_VAA];
            uint32_t total_data = 0;
            for (uint32_t i = 0; i < GL_CACHE_MAX_VAA; i++) {
                if (!(active_client & (1u << i))) continue;
                ClientVapEntry* cv = &g_client_vap[i];
                uint32_t elem_sz = (uint32_t)cv->size * gl_type_size_bytes(cv->type);
                uint32_t eff = (cv->stride > 0) ? (uint32_t)cv->stride : elem_sz;
                uint32_t bytes = (uint32_t)count * eff;
                attr_idxs[num_attrs] = i;
                attr_eff_stride[num_attrs] = eff;
                attr_bytes[num_attrs] = bytes;
                total_data += (bytes + 7u) & ~7u;
                num_attrs++;
            }
            uint32_t hdr_sz = (uint32_t)(sizeof(GlCmdDrawArraysClient) + num_attrs * sizeof(GlCmdClientAttrDesc));
            uint32_t total_sz = (hdr_sz + total_data + 7u) & ~7u;
            if (total_sz <= GLQ_MAX_INLINE_DATA) {
                glq_flush_array_unbind();
                GlCmdDrawArraysClient* cmd = (GlCmdDrawArraysClient*)glq_alloc(total_sz);
                cmd->h.op = GLCMD_DRAW_ARRAYS_CLIENT; cmd->h.flags = 0; cmd->h.size = total_sz;
                cmd->mode = mode; cmd->count = count; cmd->num_attrs = num_attrs; cmd->_pad = 0;
                GlCmdClientAttrDesc* descs = (GlCmdClientAttrDesc*)(cmd + 1);
                uint32_t cur_off = hdr_sz;
                uint8_t* base = (uint8_t*)cmd;
                for (uint32_t a = 0; a < num_attrs; a++) {
                    uint32_t idx = attr_idxs[a];
                    ClientVapEntry* cv = &g_client_vap[idx];
                    descs[a].index = idx;
                    descs[a].size = cv->size;
                    descs[a].type = cv->type;
                    descs[a].normalized = cv->norm;
                    descs[a].stride = (int32_t)attr_eff_stride[a];
                    descs[a].is_arb = cv->is_arb;
                    descs[a].data_off = cur_off;
                    descs[a]._pad = 0;
                    const uint8_t* src = g_mem_base + cv->gptr + (uint32_t)first * attr_eff_stride[a];
                    memcpy(base + cur_off, src, attr_bytes[a]);
                    cur_off += (attr_bytes[a] + 7u) & ~7u;
                }
                glq_commit();
                return;
            }
            glq_sync_call(glw_glDrawArrays_dir, c, "glDrawArrays_client_big");
            return;
        }
        glq_push_3u(GLCMD_DRAW_ARRAYS, mode, (uint32_t)first, (uint32_t)count);
        return;
    }
    pfn_glDrawArrays(mode, first, count);
}
static void glw_glDrawBuffer(CPUContext* c) {
    GLenum m = arg32(c,0);
    if (g_glt_active) { glq_push_1u(GLCMD_DRAW_BUFFER, m); return; }
    pfn_glDrawBuffer(m);
}

static void glw_glDrawRangeElements_dir(CPUContext* c) {
    uint32_t idx_p = arg32(c,5);
    const void* indices = (g_bound_elem_buf != 0) ? (const void*)(uintptr_t)idx_p : ptr32(idx_p);
    if (pfn_glDrawRangeElements) {
        pfn_glDrawRangeElements(arg32(c,0), arg32(c,1), arg32(c,2), (GLsizei)arg32(c,3), arg32(c,4), indices);
    }
}
void recomp_gl_draw_range_elements(CPUContext* c, uint32_t mode, uint32_t start, uint32_t end, uint32_t count, uint32_t type, uint32_t indices) {
#ifndef __SWITCH__
    if (null_gl_active()) return;
#endif
    if (g_glt_active) {
        if (g_bound_elem_buf != 0 && (g_client_vap_mask & g_cache_vaa_enabled) == 0) {
            glq_push_6u(GLCMD_DRAW_RANGE_ELEMENTS, mode, start, end, count, type, indices);
            return;
        }
        /* Fallback sync: needs args on guest stack at esp+4..24 */
        uint32_t saved_esp = c->esp;
        c->esp -= 28u;
        mem_w32(c->esp + 4u, mode); mem_w32(c->esp + 8u, start); mem_w32(c->esp + 12u, end);
        mem_w32(c->esp + 16u, count); mem_w32(c->esp + 20u, type); mem_w32(c->esp + 24u, indices);
        glq_sync_call(glw_glDrawRangeElements_dir, c, "glw_glDrawRangeElements_dir");
        c->esp = saved_esp;
        return;
    }
    const void* idx_ptr = (g_bound_elem_buf != 0) ? (const void*)(uintptr_t)indices : ptr32(indices);
    if (pfn_glDrawRangeElements) pfn_glDrawRangeElements(mode, start, end, (GLsizei)count, type, idx_ptr);
}

static void glw_glDrawRangeElements(CPUContext* c) {
#ifndef __SWITCH__
    if (null_gl_active()) return;
#endif
    if (g_glt_active) {
        if (g_bound_elem_buf != 0 && (g_client_vap_mask & g_cache_vaa_enabled) == 0) {
            glq_push_6u(GLCMD_DRAW_RANGE_ELEMENTS, arg32(c,0), arg32(c,1), arg32(c,2), arg32(c,3), arg32(c,4), arg32(c,5));
            return;
        }
        glq_sync_call(glw_glDrawRangeElements_dir, c, "glw_glDrawRangeElements_dir");
        return;
    }
    glw_glDrawRangeElements_dir(c);
}

void recomp_gl_enable(uint32_t cap) {
    int s = gl_cap_slot(cap);
    if (s >= 0) {
        if (g_cache_caps[s] == 1) return;
        g_cache_caps[s] = 1;
    } else {
        int ts = gl_tex_enable_slot(cap);
        if (ts >= 0 && g_cache_active_tex >= GL_TEXTURE0) {
            uint32_t unit = (uint32_t)(g_cache_active_tex - GL_TEXTURE0);
            if (unit < GL_CACHE_MAX_TEX_UNITS) {
                if (g_cache_tex_enable[unit][ts] == 2) return;
                g_cache_tex_enable[unit][ts] = 2;
            }
        }
    }
    if (g_glt_active) { glq_push_1u(GLCMD_ENABLE, cap); return; }
    pfn_glEnable(cap);
}
static void glw_glEnable(CPUContext* c) {
    recomp_gl_enable(arg32(c,0));
}
static void glw_glEnableVertexAttribArray(CPUContext* c) {
    GLuint idx = arg32(c,0);
    if (idx < 32u) {
        uint32_t bit = 1u << idx;
        if ((g_cache_vaa_known & bit) && (g_cache_vaa_enabled & bit)) return;
        g_cache_vaa_known |= bit;
        g_cache_vaa_enabled |= bit;
    }
    if (g_glt_active) { glq_push_2u(GLCMD_ENABLE_VAA, idx, 0); return; }
    if (pfn_glEnableVertexAttribArray) pfn_glEnableVertexAttribArray(idx);
}
void recomp_gl_enable_vaa_arb(uint32_t idx) {
    if (idx < 32u) {
        uint32_t bit = 1u << idx;
        if ((g_cache_vaa_known & bit) && (g_cache_vaa_enabled & bit)) return;
        g_cache_vaa_known |= bit;
        g_cache_vaa_enabled |= bit;
    }
    if (g_glt_active) { glq_push_2u(GLCMD_ENABLE_VAA, idx, 1); return; }
    if (pfn_glEnableVertexAttribArrayARB) pfn_glEnableVertexAttribArrayARB(idx);
}
static void glw_glEnableVertexAttribArrayARB(CPUContext* c) {
    recomp_gl_enable_vaa_arb(arg32(c,0));
}
static void glw_glEndQueryARB(CPUContext* c) {
    GLenum t = arg32(c,0);
    if (g_glt_active) { glq_push_1u(GLCMD_END_QUERY, t); return; }
    if (pfn_glEndQueryARB) pfn_glEndQueryARB(t);
}

static void flush_mapped_common(GLenum target, uint32_t off, uint32_t len) {
    MappedBufSlot* s = find_mapped(target);
    if (!s) return;
    s->flushed_explicit = true;
    if (len == 0) return;
    if (g_glt_active && !s->real_map) {
        glq_push_buf_sub_data(target, (int32_t)(s->offset + off), len, g_mem_base + s->guest_ptr + off);
        return;
    }
    if (s->real_map && pfn_glFlushMappedBufferRange) {
        memcpy((uint8_t*)s->real_map + off, g_mem_base + s->guest_ptr + off, len);
        pfn_glFlushMappedBufferRange(target, (GLintptr)off, (GLsizeiptr)len);
    } else if (pfn_glBufferSubDataARB) {
        pfn_glBufferSubDataARB(target, (GLintptr)(s->offset + off), (GLsizeiptr)len, g_mem_base + s->guest_ptr + off);
    }
}

static void glw_glFlushMappedBufferRange(CPUContext* c) {
    flush_mapped_common(arg32(c,0), arg32(c,1), arg32(c,2));
}

static void glw_glFlushMappedBufferRangeAPPLE(CPUContext* c) {
    flush_mapped_common(arg32(c,0), arg32(c,1), arg32(c,2));
}

static bool g_cache_fog_color_valid = false;
static uint32_t g_cache_fog_color[4];
static void glw_glFogfv(CPUContext* c) {
    GLenum p = arg32(c,0); const GLfloat* v = (const GLfloat*)ptr32(arg32(c,1));
    if (p == GL_FOG_COLOR && v) {
        uint32_t cur[4];
        memcpy(cur, v, 16);
        if (g_cache_fog_color_valid &&
            g_cache_fog_color[0] == cur[0] && g_cache_fog_color[1] == cur[1] &&
            g_cache_fog_color[2] == cur[2] && g_cache_fog_color[3] == cur[3]) {
            return;
        }
        memcpy(g_cache_fog_color, cur, 16);
        g_cache_fog_color_valid = true;
    }
    if (g_glt_active) {
        GlCmd1U4F* cmd = (GlCmd1U4F*)glq_alloc(sizeof(GlCmd1U4F));
        cmd->h.op = GLCMD_FOGFV; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd1U4F) + 7u) & ~7u;
        cmd->a = p;
        if (v) { cmd->f[0]=v[0]; cmd->f[1]=(p==GL_FOG_COLOR)?v[1]:0.f; cmd->f[2]=(p==GL_FOG_COLOR)?v[2]:0.f; cmd->f[3]=(p==GL_FOG_COLOR)?v[3]:0.f; }
        else   { cmd->f[0]=cmd->f[1]=cmd->f[2]=cmd->f[3]=0.f; }
        glq_commit(); return;
    }
    pfn_glFogfv(p, v);
}
static void glw_glFramebufferRenderbufferEXT(CPUContext* c) {
    GLenum t=arg32(c,0), a=arg32(c,1), rbt=arg32(c,2); GLuint rb=arg32(c,3);
    if (g_glt_active) { glq_push_4u(GLCMD_FB_RENDERBUFFER, t, a, rbt, rb); return; }
    if (pfn_glFramebufferRenderbufferEXT) pfn_glFramebufferRenderbufferEXT(t, a, rbt, rb);
}
static void glw_glFramebufferTexture2DEXT(CPUContext* c) {
    GLenum t=arg32(c,0), a=arg32(c,1), tt=arg32(c,2); GLuint tex=arg32(c,3); GLint lv=(GLint)arg32(c,4);
    if (g_glt_active) { glq_push_5u(GLCMD_FB_TEXTURE2D, t, a, tt, tex, (uint32_t)lv); return; }
    if (pfn_glFramebufferTexture2DEXT) pfn_glFramebufferTexture2DEXT(t, a, tt, tex, lv);
}
GLW_SYNC(glGenBuffersARB, { if (pfn_glGenBuffersARB) pfn_glGenBuffersARB((GLsizei)arg32(c,0), (GLuint*)mptr32(arg32(c,1))); })
GLW_SYNC(glGenFramebuffersEXT, { if (pfn_glGenFramebuffersEXT) pfn_glGenFramebuffersEXT((GLsizei)arg32(c,0), (GLuint*)mptr32(arg32(c,1))); })
GLW_SYNC(glGenProgramsARB, { if (pfn_glGenProgramsARB) pfn_glGenProgramsARB((GLsizei)arg32(c,0), (GLuint*)mptr32(arg32(c,1))); })
GLW_SYNC(glGenQueriesARB, { if (pfn_glGenQueriesARB) pfn_glGenQueriesARB((GLsizei)arg32(c,0), (GLuint*)mptr32(arg32(c,1))); })
GLW_SYNC(glGenRenderbuffersEXT, { if (pfn_glGenRenderbuffersEXT) pfn_glGenRenderbuffersEXT((GLsizei)arg32(c,0), (GLuint*)mptr32(arg32(c,1))); })
GLW_SYNC(glGenSamplers, { if (pfn_glGenSamplers) pfn_glGenSamplers((GLsizei)arg32(c,0), (GLuint*)mptr32(arg32(c,1))); })
GLW_SYNC(glGenTextures, { pfn_glGenTextures((GLsizei)arg32(c,0), (GLuint*)mptr32(arg32(c,1))); })
static void glw_glGenerateMipmapEXT(CPUContext* c) {
    GLenum t = arg32(c,0);
    if (g_glt_active) { glq_push_1u(GLCMD_GENERATE_MIPMAP, t); return; }
    if (pfn_glGenerateMipmapEXT) pfn_glGenerateMipmapEXT(t);
}
GLW_SYNC(glGetAttribLocation, { c->eax = pfn_glGetAttribLocation ? (uint32_t)pfn_glGetAttribLocation(arg32(c,0), (const GLchar*)ptr32(arg32(c,1))) : 0xFFFFFFFFu; })
GLW_SYNC(glGetAttribLocationARB, { c->eax = pfn_glGetAttribLocationARB ? (uint32_t)pfn_glGetAttribLocationARB((GLhandleARB)arg32(c,0), (const GLcharARB*)ptr32(arg32(c,1))) : 0xFFFFFFFFu; })
GLW_SYNC(glGetCompressedTexImage, { if (pfn_glGetCompressedTexImage) pfn_glGetCompressedTexImage(arg32(c,0), (GLint)arg32(c,1), mptr32(arg32(c,2))); })
static void glw_glGetError(CPUContext* c) {
    if (g_glt_active) { c->eax = GL_NO_ERROR; return; }
    c->eax = pfn_glGetError();
}
GLW_SYNC(glGetInfoLogARB, { if (pfn_glGetInfoLogARB) pfn_glGetInfoLogARB((GLhandleARB)arg32(c,0), (GLsizei)arg32(c,1), (GLsizei*)mptr32(arg32(c,2)), (GLcharARB*)mptr32(arg32(c,3))); })
GLW_SYNC(glGetIntegerv, { pfn_glGetIntegerv(arg32(c,0), (GLint*)mptr32(arg32(c,1))); })
GLW_SYNC(glGetObjectParameterivARB, { if (pfn_glGetObjectParameterivARB) pfn_glGetObjectParameterivARB((GLhandleARB)arg32(c,0), arg32(c,1), (GLint*)mptr32(arg32(c,2))); })
GLW_SYNC(glGetProgramInfoLog, { if (pfn_glGetProgramInfoLog) pfn_glGetProgramInfoLog(arg32(c,0), (GLsizei)arg32(c,1), (GLsizei*)mptr32(arg32(c,2)), (GLchar*)mptr32(arg32(c,3))); })
GLW_SYNC(glGetProgramiv, { if (pfn_glGetProgramiv) pfn_glGetProgramiv(arg32(c,0), arg32(c,1), (GLint*)mptr32(arg32(c,2))); })
GLW_SYNC(glGetProgramivARB, { if (pfn_glGetProgramivARB) pfn_glGetProgramivARB(arg32(c,0), arg32(c,1), (GLint*)mptr32(arg32(c,2))); })
GLW_SYNC(glGetQueryObjectuivARB, { if (pfn_glGetQueryObjectuivARB) pfn_glGetQueryObjectuivARB(arg32(c,0), arg32(c,1), (GLuint*)mptr32(arg32(c,2))); })
GLW_SYNC(glGetShaderiv, { if (pfn_glGetShaderiv) pfn_glGetShaderiv(arg32(c,0), arg32(c,1), (GLint*)mptr32(arg32(c,2))); })

typedef struct GlStrCache { GLenum name; uint32_t gptr; } GlStrCache;
static GlStrCache g_gl_strs[16];

static void glw_glGetString_dir(CPUContext* c) {
    GLenum name = arg32(c,0);
    const char* s = (const char*)pfn_glGetString(name);
    if (!s) { c->eax = 0; return; }
    uint32_t gp = guest_strdup(s);
    for (int i = 0; i < 16; i++) {
        if (!g_gl_strs[i].gptr) {
            g_gl_strs[i].name = name;
            g_gl_strs[i].gptr = gp;
            break;
        }
    }
    c->eax = gp;
}
static void glw_glGetString(CPUContext* c) {
    GLenum name = arg32(c,0);
    for (int i = 0; i < 16; i++) {
        if (g_gl_strs[i].name == name && g_gl_strs[i].gptr) {
            c->eax = g_gl_strs[i].gptr;
            return;
        }
    }
    if (g_glt_active) { glq_sync_call(glw_glGetString_dir, c, "glw_glGetString_dir"); return; }
    glw_glGetString_dir(c);
}

GLW_SYNC(glGetTexImage, { pfn_glGetTexImage(arg32(c,0), (GLint)arg32(c,1), arg32(c,2), arg32(c,3), mptr32(arg32(c,4))); })
GLW_SYNC(glGetUniformLocation, { c->eax = pfn_glGetUniformLocation ? (uint32_t)pfn_glGetUniformLocation(arg32(c,0), (const GLchar*)ptr32(arg32(c,1))) : 0xFFFFFFFFu; })
GLW_SYNC(glGetUniformLocationARB, { c->eax = pfn_glGetUniformLocationARB ? (uint32_t)pfn_glGetUniformLocationARB((GLhandleARB)arg32(c,0), (const GLcharARB*)ptr32(arg32(c,1))) : 0xFFFFFFFFu; })
GLW_SYNC(glLinkProgram, {
    u4fv_cache_invalidate();
    if (pfn_glLinkProgram) pfn_glLinkProgram(arg32(c,0));
    if (recomp_sdl_is_boot_loading()) recomp_sdl_notify_boot_progress(2, ++g_boot_shader_count);
})
GLW_SYNC(glLinkProgramARB, {
    u4fv_cache_invalidate();
    if (pfn_glLinkProgramARB) pfn_glLinkProgramARB((GLhandleARB)arg32(c,0));
    if (recomp_sdl_is_boot_loading()) recomp_sdl_notify_boot_progress(2, ++g_boot_shader_count);
})

static void map_buf_common(CPUContext* c, GLenum target, uint32_t offset, uint32_t length, GLbitfield access, bool is_range) {
    GLuint buf = *target_binding_ptr(target);
    if (length == 0 && !is_range) {
        length = (buf < MAX_GL_BUFFERS) ? g_buf_sizes[buf] : 0;
    }
    if (length == 0) {
        c->eax = 0;
        return;
    }
    MappedBufSlot* slot = NULL;
    for (int i = 0; i < MAX_MAPPED_SLOTS; i++) {
        if (!g_mapped_slots[i].active && g_mapped_slots[i].target == target) {
            slot = &g_mapped_slots[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < MAX_MAPPED_SLOTS; i++) {
            if (!g_mapped_slots[i].active) { slot = &g_mapped_slots[i]; break; }
        }
    }
    if (!slot) slot = &g_mapped_slots[0];

    if (slot->capacity < length || !slot->guest_ptr) {
        if (slot->guest_ptr) {
            guest_free(slot->guest_ptr);
        }
        uint32_t new_cap = (length < 65536u) ? 65536u : ((length + 4095u) & ~4095u);
        slot->guest_ptr = guest_malloc(new_cap);
        slot->capacity = slot->guest_ptr ? new_cap : 0u;
    }
    uint32_t gptr = slot->guest_ptr;
    if (!gptr) {
        c->eax = 0;
        return;
    }
    bool need_read = is_range ? ((access & GL_MAP_READ_BIT) != 0) : (access != GL_WRITE_ONLY_ARB);
    void* real_map = NULL;
    if (!g_glt_active) {
        if (is_range && pfn_glMapBufferRange) {
            real_map = pfn_glMapBufferRange(target, (GLintptr)offset, (GLsizeiptr)length, access);
            if (real_map && need_read) {
                memcpy(g_mem_base + gptr, real_map, length);
            }
        } else if (!is_range && pfn_glMapBufferARB) {
            real_map = pfn_glMapBufferARB(target, access);
            if (real_map && need_read) {
                memcpy(g_mem_base + gptr, real_map, length);
            }
        }
    }
    /* When g_glt_active && !need_read: zero-sync write-only staging!
     * Data will be streamed via GLCMD_BUFFER_SUB_DATA on flush/unmap. */
    slot->active = true;
    slot->target = target;
    slot->buf_id = buf;
    slot->offset = offset;
    slot->length = length;
    slot->flushed_explicit = false;
    slot->real_map = real_map;
    c->eax = gptr;
}

static void glw_glMapBufferARB_dir(CPUContext* c) {
    map_buf_common(c, arg32(c,0), 0, 0, arg32(c,1), false);
}
static void glw_glMapBufferARB(CPUContext* c) {
    if (g_glt_active && arg32(c,1) != GL_WRITE_ONLY_ARB) {
        /* Readback map: sync onto GL thread */
        bool saved = g_glt_active; g_glt_active = false;
        glq_sync_call(glw_glMapBufferARB_dir, c, "glw_glMapBufferARB_dir");
        g_glt_active = saved;
        return;
    }
    map_buf_common(c, arg32(c,0), 0, 0, arg32(c,1), false);
}

static void glw_glMapBufferRange_dir(CPUContext* c) {
    map_buf_common(c, arg32(c,0), arg32(c,1), arg32(c,2), arg32(c,3), true);
}
static void glw_glMapBufferRange(CPUContext* c) {
    if (g_glt_active && (arg32(c,3) & GL_MAP_READ_BIT)) {
        bool saved = g_glt_active; g_glt_active = false;
        glq_sync_call(glw_glMapBufferRange_dir, c, "glw_glMapBufferRange_dir");
        g_glt_active = saved;
        return;
    }
    map_buf_common(c, arg32(c,0), arg32(c,1), arg32(c,2), arg32(c,3), true);
}

static void glw_glUnmapBufferARB_dir(CPUContext* c) {
    GLenum target = arg32(c,0);
    MappedBufSlot* s = find_mapped(target);
    if (!s) {
        c->eax = pfn_glUnmapBufferARB ? (uint32_t)pfn_glUnmapBufferARB(target) : GL_TRUE;
        return;
    }
    if (!s->flushed_explicit) {
        if (s->real_map) {
            memcpy(s->real_map, g_mem_base + s->guest_ptr, s->length);
        } else if (pfn_glBufferSubDataARB) {
            pfn_glBufferSubDataARB(target, (GLintptr)s->offset, (GLsizeiptr)s->length, g_mem_base + s->guest_ptr);
        }
    }
    GLboolean ok = GL_TRUE;
    if (s->real_map && pfn_glUnmapBufferARB) {
        ok = pfn_glUnmapBufferARB(target);
    }
    s->active = false;
    c->eax = (uint32_t)ok;
}
static void glw_glUnmapBufferARB(CPUContext* c) {
    if (g_glt_active) {
        GLenum target = arg32(c,0);
        MappedBufSlot* s = find_mapped(target);
        if (s && !s->real_map) {
            if (!s->flushed_explicit && s->length > 0) {
                glq_push_buf_sub_data(target, (int32_t)s->offset, s->length, g_mem_base + s->guest_ptr);
            }
            s->active = false;
            c->eax = GL_TRUE;
            return;
        }
        glq_sync_call(glw_glUnmapBufferARB_dir, c, "glw_glUnmapBufferARB_dir");
        return;
    }
    glw_glUnmapBufferARB_dir(c);
}

static void glw_glPixelStorei(CPUContext* c) {
    GLenum p=arg32(c,0); GLint v=(GLint)arg32(c,1);
    if (g_glt_active) { glq_push_2u(GLCMD_PIXEL_STOREI, p, (uint32_t)v); return; }
    pfn_glPixelStorei(p, v);
}
static void glw_glPointParameterfARB(CPUContext* c) {
    GLenum p=arg32(c,0); GLfloat v=argf32(c,1);
    if (g_glt_active) {
        GlCmd1U1F* cmd = (GlCmd1U1F*)glq_alloc(sizeof(GlCmd1U1F));
        cmd->h.op = GLCMD_POINT_PARAM_F; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd1U1F) + 7u) & ~7u;
        cmd->a = p; cmd->f0 = v; glq_commit(); return;
    }
    if (pfn_glPointParameterfARB) pfn_glPointParameterfARB(p, v);
}
static void glw_glPointParameterfvARB(CPUContext* c) {
    GLenum p=arg32(c,0); const GLfloat* v=(const GLfloat*)ptr32(arg32(c,1));
    if (g_glt_active) {
        GlCmd1U4F* cmd = (GlCmd1U4F*)glq_alloc(sizeof(GlCmd1U4F));
        cmd->h.op = GLCMD_POINT_PARAM_FV; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd1U4F) + 7u) & ~7u;
        cmd->a = p;
        if (v) { cmd->f[0]=v[0]; cmd->f[1]=v[1]; cmd->f[2]=v[2]; cmd->f[3]=0.f; }
        else   { cmd->f[0]=cmd->f[1]=cmd->f[2]=cmd->f[3]=0.f; }
        glq_commit(); return;
    }
    if (pfn_glPointParameterfvARB) pfn_glPointParameterfvARB(p, v);
}
static void glw_glPointSize(CPUContext* c) {
    GLfloat sz = argf32(c,0);
    if (g_glt_active) {
        GlCmd1F* cmd = (GlCmd1F*)glq_alloc(sizeof(GlCmd1F));
        cmd->h.op = GLCMD_POINT_SIZE; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd1F) + 7u) & ~7u;
        cmd->f0 = sz; glq_commit(); return;
    }
    pfn_glPointSize(sz);
}
static void glw_glPolygonMode(CPUContext* c) {
    GLenum f=arg32(c,0), m=arg32(c,1);
    if (g_cache_poly_mode_face == f && g_cache_poly_mode == m) return;
    g_cache_poly_mode_face = f; g_cache_poly_mode = m;
    if (g_glt_active) { glq_push_2u(GLCMD_POLYGON_MODE, f, m); return; }
    pfn_glPolygonMode(f, m);
}
static void glw_glPolygonOffset(CPUContext* c) {
    GLfloat f=argf32(c,0), u=argf32(c,1);
    if (g_cache_poly_off_valid && g_cache_poly_off_f == f && g_cache_poly_off_u == u) return;
    g_cache_poly_off_valid = true; g_cache_poly_off_f = f; g_cache_poly_off_u = u;
    if (g_glt_active) {
        GlCmd2F* cmd = (GlCmd2F*)glq_alloc(sizeof(GlCmd2F));
        cmd->h.op = GLCMD_POLYGON_OFFSET; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd2F) + 7u) & ~7u;
        cmd->f0 = f; cmd->f1 = u; glq_commit(); return;
    }
    pfn_glPolygonOffset(f, u);
}
static void glw_glProgramLocalParameter4fvARB(CPUContext* c) {
    GLenum t=arg32(c,0); GLuint idx=arg32(c,1); const GLfloat* v=(const GLfloat*)ptr32(arg32(c,2));
    if (g_glt_active) {
        GlCmd2U4F* cmd = (GlCmd2U4F*)glq_alloc(sizeof(GlCmd2U4F));
        cmd->h.op = GLCMD_PROGRAM_LOCAL_4FV; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd2U4F) + 7u) & ~7u;
        cmd->a = t; cmd->b = idx;
        if (v) memcpy(cmd->f, v, 16); else memset(cmd->f, 0, 16);
        glq_commit(); return;
    }
    if (pfn_glProgramLocalParameter4fvARB) pfn_glProgramLocalParameter4fvARB(t, idx, v);
}
static void glw_glProgramLocalParameterI4ivNV(CPUContext* c) {
    GLenum t=arg32(c,0); GLuint idx=arg32(c,1); const GLint* v=(const GLint*)ptr32(arg32(c,2));
    if (g_glt_active) {
        GlCmd2U4I* cmd = (GlCmd2U4I*)glq_alloc(sizeof(GlCmd2U4I));
        cmd->h.op = GLCMD_PROGRAM_LOCAL_I4IV; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd2U4I) + 7u) & ~7u;
        cmd->a = t; cmd->b = idx;
        if (v) memcpy(cmd->v, v, 16); else memset(cmd->v, 0, 16);
        glq_commit(); return;
    }
    if (pfn_glProgramLocalParameterI4ivNV) pfn_glProgramLocalParameterI4ivNV(t, idx, v);
}
GLW_SYNC(glProgramStringARB, {
    if (pfn_glProgramStringARB) pfn_glProgramStringARB(arg32(c,0), arg32(c,1), (GLsizei)arg32(c,2), ptr32(arg32(c,3)));
    if (recomp_sdl_is_boot_loading()) recomp_sdl_notify_boot_progress(2, ++g_boot_shader_count);
})
GLW_SYNC(glRenderbufferStorageEXT, { if (pfn_glRenderbufferStorageEXT) pfn_glRenderbufferStorageEXT(arg32(c,0), arg32(c,1), (GLsizei)arg32(c,2), (GLsizei)arg32(c,3)); })
static bool g_sampler_mip_tracked[MAX_SAMPLER_TRACK];
static bool g_last_tex2d_mip = false;

static float query_hw_max_aniso(void) {
    static float g_max_hw_aniso = -1.0f;
    if (g_max_hw_aniso >= 1.0f) return g_max_hw_aniso;
    g_max_hw_aniso = 1.0f;
    if (pfn_glGetFloatv) {
        GLfloat v = 1.0f;
        if (pfn_glGetError) (void)pfn_glGetError();
        pfn_glGetFloatv(RECOMP_GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &v);
        GLenum err = pfn_glGetError ? pfn_glGetError() : GL_NO_ERROR;
        if (err == GL_NO_ERROR && v >= 1.0f) {
            g_max_hw_aniso = (v > 16.0f) ? 16.0f : (float)v;
        }
    }
    fprintf(stderr, "[gl] Max supported anisotropy: %.1fx (configured: %dx)\n",
            g_max_hw_aniso, recomp_settings_get()->aniso_level);
    return g_max_hw_aniso;
}

static float target_aniso_level(void) {
    int want = recomp_settings_get()->aniso_level;
    if (want <= 0) return 0.0f;
    float max_hw = query_hw_max_aniso();
    if (max_hw <= 1.0f) return 0.0f;
    return ((float)want < max_hw) ? (float)want : max_hw;
}

static GLint enhance_min_filter_aniso(GLint f, bool* out_is_mip, float* out_apply) {
    bool is_mip = (f >= 0x2700 && f <= 0x2703);
    if (out_is_mip) *out_is_mip = is_mip;
    if (!is_mip) {
        if (out_apply) *out_apply = 0.0f;
        return f;
    }
    float apply = target_aniso_level();
    if (out_apply) *out_apply = apply;
    if (apply > 1.0f && f == GL_LINEAR_MIPMAP_NEAREST) {
        return GL_LINEAR_MIPMAP_LINEAR;
    }
    return f;
}

static inline void emit_sampler_param_f(GLuint s, GLenum p, GLfloat v) {
    if (g_glt_active) { glq_push_2u1f(GLCMD_SAMPLER_PARAM_F, s, p, v); return; }
    if (pfn_glSamplerParameterf) pfn_glSamplerParameterf(s, p, v);
}
static inline void emit_sampler_param_i(GLuint s, GLenum p, GLint v) {
    if (g_glt_active) { glq_push_3u(GLCMD_SAMPLER_PARAM_I, s, p, (uint32_t)v); return; }
    if (pfn_glSamplerParameteri) pfn_glSamplerParameteri(s, p, v);
}

static void glw_glSamplerParameterf(CPUContext* c) {
    if (!pfn_glSamplerParameterf) return;
    GLuint sampler = arg32(c,0);
    GLenum pname = arg32(c,1);
    GLfloat param = argf32(c,2);
    if (pname == RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT) {
        bool is_mip = (sampler >= MAX_SAMPLER_TRACK) || g_sampler_mip_tracked[sampler];
        if (is_mip) {
            float apply = target_aniso_level();
            if (apply > 1.0f && apply > param) param = apply;
        }
    } else if (pname == GL_TEXTURE_MIN_FILTER) {
        bool is_mip = false;
        float apply = 0.0f;
        GLint ip = enhance_min_filter_aniso((GLint)param, &is_mip, &apply);
        if (sampler < MAX_SAMPLER_TRACK) g_sampler_mip_tracked[sampler] = is_mip;
        param = (GLfloat)ip;
        if (apply > 1.0f) {
            uint32_t abits; memcpy(&abits, &apply, 4);
            if (sampler >= MAX_SAMPLER_TRACK || !(g_cache_sparam_valid[sampler] & (1u << 5)) || (uint32_t)g_cache_sparam_val[sampler][5] != abits) {
                if (sampler < MAX_SAMPLER_TRACK) { g_cache_sparam_val[sampler][5] = (int32_t)abits; g_cache_sparam_valid[sampler] |= (1u << 5); }
                emit_sampler_param_f(sampler, RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT, apply);
            }
        }
    }
    int slot = sampler_param_slot(pname);
    if (slot >= 0 && sampler < MAX_SAMPLER_TRACK) {
        int32_t key;
        if (slot == 5) { memcpy(&key, &param, 4); } else { key = (int32_t)param; }
        if ((g_cache_sparam_valid[sampler] & (1u << slot)) && g_cache_sparam_val[sampler][slot] == key) return;
        g_cache_sparam_val[sampler][slot] = key;
        g_cache_sparam_valid[sampler] |= (uint8_t)(1u << slot);
    }
    emit_sampler_param_f(sampler, pname, param);
}
static void glw_glSamplerParameterfv(CPUContext* c) {
    if (!pfn_glSamplerParameterfv) return;
    GLuint s=arg32(c,0); GLenum p=arg32(c,1); const GLfloat* v=(const GLfloat*)ptr32(arg32(c,2));
    if (g_glt_active) {
        GlCmd2U4F* cmd = (GlCmd2U4F*)glq_alloc(sizeof(GlCmd2U4F));
        cmd->h.op = GLCMD_SAMPLER_PARAM_FV; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd2U4F) + 7u) & ~7u;
        cmd->a = s; cmd->b = p;
        if (v) memcpy(cmd->f, v, 16); else memset(cmd->f, 0, 16);
        glq_commit(); return;
    }
    pfn_glSamplerParameterfv(s, p, v);
}
static void glw_glSamplerParameteri(CPUContext* c) {
    if (!pfn_glSamplerParameteri) return;
    GLuint sampler = arg32(c,0);
    GLenum pname = arg32(c,1);
    GLint param = (GLint)arg32(c,2);
    if (pname == GL_TEXTURE_MIN_FILTER) {
        bool is_mip = false;
        float apply = 0.0f;
        param = enhance_min_filter_aniso(param, &is_mip, &apply);
        if (sampler < MAX_SAMPLER_TRACK) g_sampler_mip_tracked[sampler] = is_mip;
        if (apply > 1.0f && pfn_glSamplerParameterf) {
            uint32_t abits; memcpy(&abits, &apply, 4);
            if (sampler >= MAX_SAMPLER_TRACK || !(g_cache_sparam_valid[sampler] & (1u << 5)) || (uint32_t)g_cache_sparam_val[sampler][5] != abits) {
                if (sampler < MAX_SAMPLER_TRACK) { g_cache_sparam_val[sampler][5] = (int32_t)abits; g_cache_sparam_valid[sampler] |= (1u << 5); }
                emit_sampler_param_f(sampler, RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT, apply);
            }
        }
    }
    int slot = sampler_param_slot(pname);
    if (slot >= 0 && slot < 5 && sampler < MAX_SAMPLER_TRACK) {
        if ((g_cache_sparam_valid[sampler] & (1u << slot)) && g_cache_sparam_val[sampler][slot] == (int32_t)param) return;
        g_cache_sparam_val[sampler][slot] = (int32_t)param;
        g_cache_sparam_valid[sampler] |= (uint8_t)(1u << slot);
    }
    emit_sampler_param_i(sampler, pname, param);
}
static void glw_glScissor(CPUContext* c) {
    GLint x = (GLint)arg32(c,0);
    GLint y = (GLint)arg32(c,1);
    GLsizei w = (GLsizei)arg32(c,2);
    GLsizei h = (GLsizei)arg32(c,3);
    if (g_bound_draw_fbo == 0) {
        g_has_last_sc = true;
        g_last_sc_x = x; g_last_sc_y = y; g_last_sc_w = w; g_last_sc_h = h;
        scale_bb_rect(x, y, w, h, &x, &y, &w, &h);
    }
    if (g_cache_sc_valid && g_cache_sc[0] == x && g_cache_sc[1] == y && g_cache_sc[2] == (GLint)w && g_cache_sc[3] == (GLint)h) return;
    g_cache_sc_valid = true; g_cache_sc[0] = x; g_cache_sc[1] = y; g_cache_sc[2] = (GLint)w; g_cache_sc[3] = (GLint)h;
    if (g_glt_active) { glq_push_4u(GLCMD_SCISSOR, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h); return; }
    pfn_glScissor(x, y, w, h);
}
static void glw_glShadeModel(CPUContext* c) {
    GLenum m = arg32(c,0);
    if (g_cache_shade_model == m) return;
    g_cache_shade_model = m;
    if (g_glt_active) { glq_push_1u(GLCMD_SHADE_MODEL, m); return; }
    pfn_glShadeModel(m);
}

static void shader_source_common(CPUContext* c, bool is_arb) {
    GLuint shader = arg32(c,0);
    GLsizei count = (GLsizei)arg32(c,1);
    uint32_t strs_p = arg32(c,2);
    const GLint* lengths = (const GLint*)ptr32(arg32(c,3));
    if (count <= 0 || count > 256 || !strs_p) return;
    const GLchar* host_strs[256];
    for (GLsizei i = 0; i < count; i++) {
        uint32_t sp = mem_r32(strs_p + (uint32_t)(i * 4));
        host_strs[i] = (const GLchar*)ptr32(sp);
    }
    if (is_arb) {
        if (pfn_glShaderSourceARB) pfn_glShaderSourceARB((GLhandleARB)shader, count, host_strs, lengths);
    } else {
        if (pfn_glShaderSource) pfn_glShaderSource(shader, count, host_strs, lengths);
    }
}

GLW_SYNC(glShaderSource, { shader_source_common(c, false); })
GLW_SYNC(glShaderSourceARB, { shader_source_common(c, true); })

static void glw_glStencilFunc(CPUContext* c) {
    GLenum f=arg32(c,0); GLint r=(GLint)arg32(c,1); GLuint m=arg32(c,2);
    if (g_cache_stencil_fn == f && g_cache_stencil_ref == r && g_cache_stencil_rmask == m) return;
    g_cache_stencil_fn = f; g_cache_stencil_ref = r; g_cache_stencil_rmask = m;
    if (g_glt_active) { glq_push_3u(GLCMD_STENCIL_FUNC, f, (uint32_t)r, m); return; }
    pfn_glStencilFunc(f, r, m);
}
static void glw_glStencilMask(CPUContext* c) {
    GLuint m=arg32(c,0);
    if (g_cache_stencil_mask_valid && g_cache_stencil_mask == m) return;
    g_cache_stencil_mask_valid = true; g_cache_stencil_mask = m;
    if (g_glt_active) { glq_push_1u(GLCMD_STENCIL_MASK, m); return; }
    pfn_glStencilMask(m);
}
static void glw_glStencilOp(CPUContext* c) {
    GLenum a=arg32(c,0), b=arg32(c,1), d=arg32(c,2);
    if (g_cache_stencil_op[0] == a && g_cache_stencil_op[1] == b && g_cache_stencil_op[2] == d) return;
    g_cache_stencil_op[0] = a; g_cache_stencil_op[1] = b; g_cache_stencil_op[2] = d;
    if (g_glt_active) { glq_push_3u(GLCMD_STENCIL_OP, a, b, d); return; }
    pfn_glStencilOp(a, b, d);
}
static void glw_glTexEnvf(CPUContext* c) {
    GLenum t=arg32(c,0), p=arg32(c,1); GLfloat v=argf32(c,2);
    if (g_glt_active) { glq_push_2u1f(GLCMD_TEX_ENVF, t, p, v); return; }
    pfn_glTexEnvf(t, p, v);
}

GLW_SYNC(glTexImage2D, {
    pfn_glTexImage2D(arg32(c,0), (GLint)arg32(c,1), (GLint)arg32(c,2), (GLsizei)arg32(c,3),
                     (GLsizei)arg32(c,4), (GLint)arg32(c,5), arg32(c,6), arg32(c,7), pbo_or_ptr(arg32(c,8)));
})

GLW_SYNC(glTexImage3D, {
    if (pfn_glTexImage3D)
        pfn_glTexImage3D(arg32(c,0), (GLint)arg32(c,1), (GLint)arg32(c,2), (GLsizei)arg32(c,3),
                         (GLsizei)arg32(c,4), (GLsizei)arg32(c,5), (GLint)arg32(c,6),
                         arg32(c,7), arg32(c,8), pbo_or_ptr(arg32(c,9)));
})

static inline GLuint current_bound_tex_for_target(GLenum target) {
    int tslot = gl_tex_enable_slot(target);
    if (tslot >= 0 && g_cache_active_tex >= GL_TEXTURE0) {
        uint32_t u = g_cache_active_tex - GL_TEXTURE0;
        if (u < GL_CACHE_MAX_TEX_UNITS && g_cache_tex_valid[tslot][u]) {
            return g_cache_tex[tslot][u];
        }
    }
    return 0;
}

static void glw_glTexParameterf(CPUContext* c) {
    GLenum target = arg32(c,0);
    GLenum pname = arg32(c,1);
    GLfloat param = argf32(c,2);
    GLuint tex = current_bound_tex_for_target(target);
    if (target == GL_TEXTURE_2D && pname == RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT && g_last_tex2d_mip) {
        float apply = target_aniso_level();
        if (apply > 1.0f && apply > param) param = apply;
    } else if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER) {
        bool is_mip = false;
        float apply = 0.0f;
        param = (GLfloat)enhance_min_filter_aniso((GLint)param, &is_mip, &apply);
        g_last_tex2d_mip = is_mip;
        if (apply > 1.0f) {
            uint32_t abits; memcpy(&abits, &apply, 4);
            if (tex == 0 || tex >= MAX_TEX_PARAM_TRACK || !(g_cache_tparam_valid[tex] & (1u << 5)) || (uint32_t)g_cache_tparam_val[tex][5] != abits) {
                if (tex > 0 && tex < MAX_TEX_PARAM_TRACK) { g_cache_tparam_val[tex][5] = (int32_t)abits; g_cache_tparam_valid[tex] |= (1u << 5); }
                if (g_glt_active) glq_push_2u1f(GLCMD_TEX_PARAM_F, GL_TEXTURE_2D, RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT, apply);
                else pfn_glTexParameterf(GL_TEXTURE_2D, RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT, apply);
            }
        }
    }
    int slot = sampler_param_slot(pname);
    if (slot >= 0 && tex > 0 && tex < MAX_TEX_PARAM_TRACK) {
        int32_t key;
        if (slot == 5) { memcpy(&key, &param, 4); } else { key = (int32_t)param; }
        if ((g_cache_tparam_valid[tex] & (1u << slot)) && g_cache_tparam_val[tex][slot] == key) return;
        g_cache_tparam_val[tex][slot] = key;
        g_cache_tparam_valid[tex] |= (uint8_t)(1u << slot);
    }
    if (g_glt_active) { glq_push_2u1f(GLCMD_TEX_PARAM_F, target, pname, param); return; }
    pfn_glTexParameterf(target, pname, param);
}
static void glw_glTexParameterfv(CPUContext* c) {
    GLenum t=arg32(c,0), p=arg32(c,1); const GLfloat* v=(const GLfloat*)ptr32(arg32(c,2));
    if (g_glt_active) {
        GlCmd2U4F* cmd = (GlCmd2U4F*)glq_alloc(sizeof(GlCmd2U4F));
        cmd->h.op = GLCMD_TEX_PARAM_FV; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmd2U4F) + 7u) & ~7u;
        cmd->a = t; cmd->b = p;
        if (v) memcpy(cmd->f, v, 16); else memset(cmd->f, 0, 16);
        glq_commit(); return;
    }
    pfn_glTexParameterfv(t, p, v);
}
static void glw_glTexParameteri(CPUContext* c) {
    GLenum target = arg32(c,0);
    GLenum pname = arg32(c,1);
    GLint param = (GLint)arg32(c,2);
    GLuint tex = current_bound_tex_for_target(target);
    if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER) {
        bool is_mip = false;
        float apply = 0.0f;
        param = enhance_min_filter_aniso(param, &is_mip, &apply);
        g_last_tex2d_mip = is_mip;
        if (apply > 1.0f && pfn_glTexParameterf) {
            uint32_t abits; memcpy(&abits, &apply, 4);
            if (tex == 0 || tex >= MAX_TEX_PARAM_TRACK || !(g_cache_tparam_valid[tex] & (1u << 5)) || (uint32_t)g_cache_tparam_val[tex][5] != abits) {
                if (tex > 0 && tex < MAX_TEX_PARAM_TRACK) { g_cache_tparam_val[tex][5] = (int32_t)abits; g_cache_tparam_valid[tex] |= (1u << 5); }
                if (g_glt_active) glq_push_2u1f(GLCMD_TEX_PARAM_F, GL_TEXTURE_2D, RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT, apply);
                else pfn_glTexParameterf(GL_TEXTURE_2D, RECOMP_GL_TEXTURE_MAX_ANISOTROPY_EXT, apply);
            }
        }
    }
    int slot = sampler_param_slot(pname);
    if (slot >= 0 && slot < 5 && tex > 0 && tex < MAX_TEX_PARAM_TRACK) {
        if ((g_cache_tparam_valid[tex] & (1u << slot)) && g_cache_tparam_val[tex][slot] == (int32_t)param) return;
        g_cache_tparam_val[tex][slot] = (int32_t)param;
        g_cache_tparam_valid[tex] |= (uint8_t)(1u << slot);
    }
    if (g_glt_active) { glq_push_3u(GLCMD_TEX_PARAM_I, target, pname, (uint32_t)param); return; }
    pfn_glTexParameteri(target, pname, param);
}

GLW_SYNC(glTexSubImage2D, {
    pfn_glTexSubImage2D(arg32(c,0), (GLint)arg32(c,1), (GLint)arg32(c,2), (GLint)arg32(c,3),
                        (GLsizei)arg32(c,4), (GLsizei)arg32(c,5), arg32(c,6), arg32(c,7), pbo_or_ptr(arg32(c,8)));
})

GLW_SYNC(glTexSubImage3D, {
    if (pfn_glTexSubImage3D)
        pfn_glTexSubImage3D(arg32(c,0), (GLint)arg32(c,1), (GLint)arg32(c,2), (GLint)arg32(c,3),
                            (GLint)arg32(c,4), (GLsizei)arg32(c,5), (GLsizei)arg32(c,6),
                            (GLsizei)arg32(c,7), arg32(c,8), arg32(c,9), pbo_or_ptr(arg32(c,10)));
})

static inline void push_uniform_v(uint16_t op, GLint loc, GLsizei count, uint32_t is_arb, const void* src, uint32_t elem_sz) {
    if (count <= 0 || !src) return;
    uint32_t bytes = (uint32_t)count * elem_sz;
    uint32_t total = (sizeof(GlCmdUniformV) + bytes + 7u) & ~7u;
    GlCmdUniformV* cmd = (GlCmdUniformV*)glq_alloc(total);
    cmd->h.op = op; cmd->h.flags = 0; cmd->h.size = total;
    cmd->loc = loc; cmd->count = count; cmd->is_arb = is_arb;
    memcpy(cmd + 1, src, bytes);
    glq_commit();
}

static inline bool u1i_cache_skip(GLint loc, GLint val) {
    if (g_cache_prog == 0xFFFFFFFFu || loc < 0) return false;
    uint32_t set = ((g_cache_prog * 2654435761u) ^ ((uint32_t)loc * 40503u)) & (U1I_CACHE_SETS - 1u);
    for (int w = 0; w < 2; w++) {
        U1iCacheSlot* s = &g_u1i_cache[set][w];
        if (s->gen == g_u4fv_cache_gen && s->prog == g_cache_prog && s->loc == loc) {
            if (s->val == val) return true;
            s->val = val;
            return false;
        }
    }
    int way = g_u1i_way_rr[set] & 1;
    g_u1i_way_rr[set] ^= 1;
    U1iCacheSlot* s = &g_u1i_cache[set][way];
    s->prog = g_cache_prog; s->loc = loc; s->val = val; s->gen = g_u4fv_cache_gen;
    return false;
}

static inline bool u1iv_cache_skip(GLint loc, GLsizei count, const GLint* v) {
    if (g_cache_prog == 0xFFFFFFFFu || loc < 0 || count <= 0 || count > U1IV_CACHE_MAX_COUNT || !v)
        return false;
    uint32_t set = ((g_cache_prog * 2654435761u) ^ ((uint32_t)loc * 40503u)) & (U1IV_CACHE_SETS - 1u);
    size_t bytes = (size_t)count * 4u;
    for (int w = 0; w < 2; w++) {
        U1ivCacheSlot* s = &g_u1iv_cache[set][w];
        if (s->gen == g_u4fv_cache_gen && s->prog == g_cache_prog && s->loc == loc) {
            if (s->count == count && memcmp(s->data, v, bytes) == 0) return true;
            s->count = count;
            memcpy(s->data, v, bytes);
            return false;
        }
    }
    int way = g_u1iv_way_rr[set] & 1;
    g_u1iv_way_rr[set] ^= 1;
    U1ivCacheSlot* s = &g_u1iv_cache[set][way];
    s->prog = g_cache_prog; s->loc = loc; s->count = count; s->gen = g_u4fv_cache_gen;
    memcpy(s->data, v, bytes);
    return false;
}

static inline bool u4iv_cache_skip(GLint loc, GLsizei count, const GLint* v) {
    if (g_cache_prog == 0xFFFFFFFFu || loc < 0 || count <= 0 || count > U4IV_CACHE_MAX_COUNT || !v)
        return false;
    uint32_t set = ((g_cache_prog * 2654435761u) ^ ((uint32_t)loc * 40503u)) & (U4IV_CACHE_SETS - 1u);
    size_t bytes = (size_t)count * 16u;
    for (int w = 0; w < 2; w++) {
        U4ivCacheSlot* s = &g_u4iv_cache[set][w];
        if (s->gen == g_u4fv_cache_gen && s->prog == g_cache_prog && s->loc == loc) {
            if (s->count == count && memcmp(s->data, v, bytes) == 0) return true;
            s->count = count;
            memcpy(s->data, v, bytes);
            return false;
        }
    }
    int way = g_u4iv_way_rr[set] & 1;
    g_u4iv_way_rr[set] ^= 1;
    U4ivCacheSlot* s = &g_u4iv_cache[set][way];
    s->prog = g_cache_prog; s->loc = loc; s->count = count; s->gen = g_u4fv_cache_gen;
    memcpy(s->data, v, bytes);
    return false;
}

static void glw_glUniform1i(CPUContext* c) {
    GLint loc=(GLint)arg32(c,0), v=(GLint)arg32(c,1);
    if (u1i_cache_skip(loc, v)) return;
    if (g_glt_active) { glq_push_3u(GLCMD_UNIFORM_1I, (uint32_t)loc, (uint32_t)v, 0); return; }
    if (pfn_glUniform1i) pfn_glUniform1i(loc, v);
}
static void glw_glUniform1iARB(CPUContext* c) {
    GLint loc=(GLint)arg32(c,0), v=(GLint)arg32(c,1);
    if (u1i_cache_skip(loc, v)) return;
    if (g_glt_active) { glq_push_3u(GLCMD_UNIFORM_1I, (uint32_t)loc, (uint32_t)v, 1); return; }
    if (pfn_glUniform1iARB) pfn_glUniform1iARB(loc, v);
}
static void glw_glUniform1iv(CPUContext* c) {
    GLint loc=(GLint)arg32(c,0); GLsizei cnt=(GLsizei)arg32(c,1); const GLint* v=(const GLint*)ptr32(arg32(c,2));
    if (u1iv_cache_skip(loc, cnt, v)) return;
    if (g_glt_active) { push_uniform_v(GLCMD_UNIFORM_1IV, loc, cnt, 0, v, 4u); return; }
    if (pfn_glUniform1iv) pfn_glUniform1iv(loc, cnt, v);
}
void recomp_gl_uniform1iv_arb(int32_t loc, int32_t cnt, const void* v) {
    if (u1iv_cache_skip(loc, cnt, (const GLint*)v)) return;
    if (g_glt_active) { push_uniform_v(GLCMD_UNIFORM_1IV, loc, cnt, 1, v, 4u); return; }
    if (pfn_glUniform1ivARB) pfn_glUniform1ivARB(loc, cnt, (const GLint*)v);
}
static void glw_glUniform1ivARB(CPUContext* c) {
    recomp_gl_uniform1iv_arb((GLint)arg32(c,0), (GLsizei)arg32(c,1), ptr32(arg32(c,2)));
}
static inline bool u4fv_cache_skip(GLint loc, GLsizei count, const GLfloat* v) {
    if (__builtin_expect(g_cache_prog == 0xFFFFFFFFu || loc < 0 || count <= 0 || count > U4FV_CACHE_MAX_COUNT || !v, 0))
        return false;
    uint32_t set = ((g_cache_prog * 2654435761u) ^ ((uint32_t)loc * 40503u)) & (U4FV_CACHE_SETS - 1u);
    size_t bytes = (size_t)count * 16u;
    uint64_t v0;
    memcpy(&v0, v, 8u);
    for (int w = 0; w < 2; w++) {
        U4fvCacheSlot* s = &g_u4fv_cache[set][w];
        if (s->gen == g_u4fv_cache_gen && s->prog == g_cache_prog && s->loc == loc) {
            if (s->count == count) {
                uint64_t s0;
                memcpy(&s0, s->data, 8u);
                if (s0 == v0 && memcmp(s->data, v, bytes) == 0) {
                    return true;
                }
            }
            s->count = count;
            memcpy(s->data, v, bytes);
            return false;
        }
    }
    int way = g_u4fv_way_rr[set] & 1;
    g_u4fv_way_rr[set] ^= 1;
    U4fvCacheSlot* s = &g_u4fv_cache[set][way];
    s->prog = g_cache_prog;
    s->loc = loc;
    s->count = count;
    s->gen = g_u4fv_cache_gen;
    memcpy(s->data, v, bytes);
    return false;
}
static void glw_glUniform4fv(CPUContext* c) {
    GLint loc=(GLint)arg32(c,0); GLsizei cnt=(GLsizei)arg32(c,1); const GLfloat* v=(const GLfloat*)ptr32(arg32(c,2));
    if (u4fv_cache_skip(loc, cnt, v)) return;
    if (g_glt_active) { push_uniform_v(GLCMD_UNIFORM_4FV, loc, cnt, 0, v, 16u); return; }
    if (pfn_glUniform4fv) pfn_glUniform4fv(loc, cnt, v);
}
void recomp_gl_uniform4fv_arb(int32_t loc, int32_t cnt, const void* v) {
    if (u4fv_cache_skip(loc, cnt, (const GLfloat*)v)) return;
    if (g_glt_active) { push_uniform_v(GLCMD_UNIFORM_4FV, loc, cnt, 1, v, 16u); return; }
    if (pfn_glUniform4fvARB) pfn_glUniform4fvARB(loc, cnt, (const GLfloat*)v);
}
static void glw_glUniform4fvARB(CPUContext* c) {
    recomp_gl_uniform4fv_arb((GLint)arg32(c,0), (GLsizei)arg32(c,1), ptr32(arg32(c,2)));
}
static void glw_glUniform4iv(CPUContext* c) {
    GLint loc=(GLint)arg32(c,0); GLsizei cnt=(GLsizei)arg32(c,1); const GLint* v=(const GLint*)ptr32(arg32(c,2));
    if (u4iv_cache_skip(loc, cnt, v)) return;
    if (g_glt_active) { push_uniform_v(GLCMD_UNIFORM_4IV, loc, cnt, 0, v, 16u); return; }
    if (pfn_glUniform4iv) pfn_glUniform4iv(loc, cnt, v);
}
void recomp_gl_uniform4iv_arb(int32_t loc, int32_t cnt, const void* v) {
    if (u4iv_cache_skip(loc, cnt, (const GLint*)v)) return;
    if (g_glt_active) { push_uniform_v(GLCMD_UNIFORM_4IV, loc, cnt, 1, v, 16u); return; }
    if (pfn_glUniform4ivARB) pfn_glUniform4ivARB(loc, cnt, (const GLint*)v);
}
static void glw_glUniform4ivARB(CPUContext* c) {
    recomp_gl_uniform4iv_arb((GLint)arg32(c,0), (GLsizei)arg32(c,1), ptr32(arg32(c,2)));
}
static void glw_glUseProgram(CPUContext* c) {
    GLuint prog = arg32(c,0);
    if (g_cache_prog == prog) return;
    g_cache_prog = prog;
    if (g_glt_active) { glq_push_2u(GLCMD_USE_PROGRAM, prog, 0); return; }
    if (pfn_glUseProgram) pfn_glUseProgram(prog);
}
static void glw_glUseProgramObjectARB(CPUContext* c) {
    GLhandleARB prog = (GLhandleARB)arg32(c,0);
    if (g_cache_prog == (GLuint)prog) return;
    g_cache_prog = (GLuint)prog;
    if (g_glt_active) { glq_push_2u(GLCMD_USE_PROGRAM, (GLuint)prog, 1); return; }
    if (pfn_glUseProgramObjectARB) pfn_glUseProgramObjectARB(prog);
}

static void glw_glVertexAttribPointer_dir(CPUContext* c) {
    uint32_t p = arg32(c,5);
    const void* ptr = (g_bound_array_buf != 0) ? (const void*)(uintptr_t)p : ptr32(p);
    if (pfn_glVertexAttribPointer)
        pfn_glVertexAttribPointer(arg32(c,0), (GLint)arg32(c,1), arg32(c,2), (GLboolean)arg32(c,3), (GLsizei)arg32(c,4), ptr);
}
static void glw_glVertexAttribPointerARB_dir(CPUContext* c) {
    uint32_t p = arg32(c,5);
    const void* ptr = (g_bound_array_buf != 0) ? (const void*)(uintptr_t)p : ptr32(p);
    if (pfn_glVertexAttribPointerARB)
        pfn_glVertexAttribPointerARB(arg32(c,0), (GLint)arg32(c,1), arg32(c,2), (GLboolean)arg32(c,3), (GLsizei)arg32(c,4), ptr);
}
static inline void vap_common_direct(GLuint idx, GLint size, GLenum type, GLboolean norm, GLsizei stride, uint32_t p, uint32_t is_arb) {
    if (idx < GL_CACHE_MAX_VAA) {
        if (g_bound_array_buf != 0) {
            g_client_vap_mask &= ~(1u << idx);
            CacheVapEntry* e = &g_cache_vap[idx];
            if (e->valid && e->buf == g_bound_array_buf && e->size == size &&
                e->type == type && e->norm == norm && e->stride == stride && e->ptr == p) {
                return;
            }
            e->valid = true; e->buf = g_bound_array_buf; e->size = size;
            e->type = type; e->norm = norm; e->stride = stride; e->ptr = p;
        } else {
            g_cache_vap[idx].valid = false;
            if (p != 0) {
                g_client_vap_mask |= (1u << idx);
                g_client_vap[idx].size = size;
                g_client_vap[idx].type = type;
                g_client_vap[idx].norm = norm;
                g_client_vap[idx].stride = stride;
                g_client_vap[idx].gptr = p;
                g_client_vap[idx].is_arb = is_arb;
            } else {
                g_client_vap_mask &= ~(1u << idx);
            }
        }
    }
    if (g_glt_active) {
        glq_flush_array_unbind();
        GlCmdVAP* cmd = (GlCmdVAP*)glq_alloc(sizeof(GlCmdVAP));
        cmd->h.op = GLCMD_VERTEX_ATTRIB_PTR; cmd->h.flags = 0; cmd->h.size = (sizeof(GlCmdVAP) + 7u) & ~7u;
        cmd->index = idx; cmd->size = size; cmd->type = type;
        cmd->normalized = norm; cmd->stride = stride; cmd->is_arb = is_arb;
        cmd->ptr = (g_bound_array_buf != 0) ? (uint64_t)p : (uint64_t)(uintptr_t)ptr32(p);
        glq_commit();
        return;
    }
    const void* ptr = (g_bound_array_buf != 0) ? (const void*)(uintptr_t)p : ptr32(p);
    if (is_arb) {
        if (pfn_glVertexAttribPointerARB) pfn_glVertexAttribPointerARB(idx, size, type, norm, stride, ptr);
    } else {
        if (pfn_glVertexAttribPointer) pfn_glVertexAttribPointer(idx, size, type, norm, stride, ptr);
    }
}

void recomp_gl_vap_arb(uint32_t idx, int32_t size, uint32_t type, uint8_t norm, int32_t stride, uint32_t p) {
    vap_common_direct((GLuint)idx, (GLint)size, (GLenum)type, (GLboolean)norm, (GLsizei)stride, p, 1u);
}

static inline void vap_common(CPUContext* c, uint32_t is_arb) {
    vap_common_direct(arg32(c,0), (GLint)arg32(c,1), arg32(c,2), (GLboolean)arg32(c,3), (GLsizei)arg32(c,4), arg32(c,5), is_arb);
}

static void glw_glVertexAttribPointer(CPUContext* c) { vap_common(c, 0); }
static void glw_glVertexAttribPointerARB(CPUContext* c) { vap_common(c, 1); }

static void glw_glViewport(CPUContext* c) {
    GLint x = (GLint)arg32(c,0);
    GLint y = (GLint)arg32(c,1);
    GLsizei w = (GLsizei)arg32(c,2);
    GLsizei h = (GLsizei)arg32(c,3);
    if (g_bound_draw_fbo == 0) {
        g_has_last_vp = true;
        g_last_vp_x = x; g_last_vp_y = y; g_last_vp_w = w; g_last_vp_h = h;
        scale_bb_rect(x, y, w, h, &x, &y, &w, &h);
    }
    if (g_cache_vp_valid && g_cache_vp[0] == x && g_cache_vp[1] == y && g_cache_vp[2] == (GLint)w && g_cache_vp[3] == (GLint)h) return;
    g_cache_vp_valid = true; g_cache_vp[0] = x; g_cache_vp[1] = y; g_cache_vp[2] = (GLint)w; g_cache_vp[3] = (GLint)h;
    if (g_glt_active) { glq_push_4u(GLCMD_VIEWPORT, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h); return; }
    pfn_glViewport(x, y, w, h);
}

void recomp_gl_set_backbuffer_scale(int guest_w, int guest_h, int host_w, int host_h) {
    g_bb_guest_w = guest_w;
    g_bb_guest_h = guest_h;
    g_bb_host_w = host_w;
    g_bb_host_h = host_h;
    g_cache_vp_valid = false;
    g_cache_sc_valid = false;
    if (g_bound_draw_fbo == 0 && pfn_glViewport && host_w > 0 && host_h > 0) {
        if (g_has_last_vp) {
            GLint x, y; GLsizei w, h;
            scale_bb_rect(g_last_vp_x, g_last_vp_y, g_last_vp_w, g_last_vp_h, &x, &y, &w, &h);
            pfn_glViewport(x, y, w, h);
        } else {
            pfn_glViewport(0, 0, (GLsizei)host_w, (GLsizei)host_h);
        }
        if (g_has_last_sc && pfn_glScissor) {
            GLint x, y; GLsizei w, h;
            scale_bb_rect(g_last_sc_x, g_last_sc_y, g_last_sc_w, g_last_sc_h, &x, &y, &w, &h);
            pfn_glScissor(x, y, w, h);
        }
    }
}

/* ---------------- Dispatch Table ---------------- */

typedef struct GlEntry {
    const char* name;
    RecompFn fn;
    bool core_always;
} GlEntry;

#define GL_TABLE_LIST(ENTRY) \
    ENTRY(glActiveTexture, true) \
    ENTRY(glAlphaFunc, true) \
    ENTRY(glAttachObjectARB, false) \
    ENTRY(glAttachShader, false) \
    ENTRY(glBeginQueryARB, false) \
    ENTRY(glBindBufferARB, false) \
    ENTRY(glBindFramebufferEXT, false) \
    ENTRY(glBindProgramARB, false) \
    ENTRY(glBindRenderbufferEXT, false) \
    ENTRY(glBindSampler, false) \
    ENTRY(glBindTexture, true) \
    ENTRY(glBlendEquation, true) \
    ENTRY(glBlendFunc, true) \
    ENTRY(glBlitFramebufferEXT, false) \
    ENTRY(glBufferDataARB, false) \
    ENTRY(glBufferParameteriAPPLE, false) \
    ENTRY(glBufferSubDataARB, false) \
    ENTRY(glCheckFramebufferStatusEXT, false) \
    ENTRY(glClear, true) \
    ENTRY(glClearColor, true) \
    ENTRY(glClearDepth, true) \
    ENTRY(glClearStencil, true) \
    ENTRY(glColorMask, true) \
    ENTRY(glCompileShader, false) \
    ENTRY(glCompileShaderARB, false) \
    ENTRY(glCompressedTexImage2D, false) \
    ENTRY(glCompressedTexImage2DARB, false) \
    ENTRY(glCompressedTexImage3D, false) \
    ENTRY(glCompressedTexSubImage2D, false) \
    ENTRY(glCompressedTexSubImage3D, false) \
    ENTRY(glCreateProgram, false) \
    ENTRY(glCreateProgramObjectARB, false) \
    ENTRY(glCreateShader, false) \
    ENTRY(glCreateShaderObjectARB, false) \
    ENTRY(glCullFace, true) \
    ENTRY(glDebugMessageCallbackARB, false) \
    ENTRY(glDeleteBuffersARB, false) \
    ENTRY(glDeleteFramebuffersEXT, false) \
    ENTRY(glDeleteObjectARB, false) \
    ENTRY(glDeleteProgram, false) \
    ENTRY(glDeleteProgramsARB, false) \
    ENTRY(glDeleteQueriesARB, false) \
    ENTRY(glDeleteRenderbuffersEXT, false) \
    ENTRY(glDeleteSamplers, false) \
    ENTRY(glDeleteShader, false) \
    ENTRY(glDeleteTextures, true) \
    ENTRY(glDepthFunc, true) \
    ENTRY(glDepthMask, true) \
    ENTRY(glDepthRange, true) \
    ENTRY(glDisable, true) \
    ENTRY(glDisableVertexAttribArray, false) \
    ENTRY(glDisableVertexAttribArrayARB, false) \
    ENTRY(glDrawArrays, true) \
    ENTRY(glDrawBuffer, true) \
    ENTRY(glDrawRangeElements, true) \
    ENTRY(glEnable, true) \
    ENTRY(glEnableVertexAttribArray, false) \
    ENTRY(glEnableVertexAttribArrayARB, false) \
    ENTRY(glEndQueryARB, false) \
    ENTRY(glFlushMappedBufferRange, false) \
    ENTRY(glFlushMappedBufferRangeAPPLE, false) \
    ENTRY(glFogfv, true) \
    ENTRY(glFramebufferRenderbufferEXT, false) \
    ENTRY(glFramebufferTexture2DEXT, false) \
    ENTRY(glGenBuffersARB, false) \
    ENTRY(glGenFramebuffersEXT, false) \
    ENTRY(glGenProgramsARB, false) \
    ENTRY(glGenQueriesARB, false) \
    ENTRY(glGenRenderbuffersEXT, false) \
    ENTRY(glGenSamplers, false) \
    ENTRY(glGenTextures, true) \
    ENTRY(glGenerateMipmapEXT, false) \
    ENTRY(glGetAttribLocation, false) \
    ENTRY(glGetAttribLocationARB, false) \
    ENTRY(glGetCompressedTexImage, false) \
    ENTRY(glGetError, true) \
    ENTRY(glGetInfoLogARB, false) \
    ENTRY(glGetIntegerv, true) \
    ENTRY(glGetObjectParameterivARB, false) \
    ENTRY(glGetProgramInfoLog, false) \
    ENTRY(glGetProgramiv, false) \
    ENTRY(glGetProgramivARB, false) \
    ENTRY(glGetQueryObjectuivARB, false) \
    ENTRY(glGetShaderiv, false) \
    ENTRY(glGetString, true) \
    ENTRY(glGetTexImage, true) \
    ENTRY(glGetUniformLocation, false) \
    ENTRY(glGetUniformLocationARB, false) \
    ENTRY(glLinkProgram, false) \
    ENTRY(glLinkProgramARB, false) \
    ENTRY(glMapBufferARB, false) \
    ENTRY(glMapBufferRange, false) \
    ENTRY(glPixelStorei, true) \
    ENTRY(glPointParameterfARB, false) \
    ENTRY(glPointParameterfvARB, false) \
    ENTRY(glPointSize, true) \
    ENTRY(glPolygonMode, true) \
    ENTRY(glPolygonOffset, true) \
    ENTRY(glProgramLocalParameter4fvARB, false) \
    ENTRY(glProgramLocalParameterI4ivNV, false) \
    ENTRY(glProgramStringARB, false) \
    ENTRY(glRenderbufferStorageEXT, false) \
    ENTRY(glSamplerParameterf, false) \
    ENTRY(glSamplerParameterfv, false) \
    ENTRY(glSamplerParameteri, false) \
    ENTRY(glScissor, true) \
    ENTRY(glShadeModel, true) \
    ENTRY(glShaderSource, false) \
    ENTRY(glShaderSourceARB, false) \
    ENTRY(glStencilFunc, true) \
    ENTRY(glStencilMask, true) \
    ENTRY(glStencilOp, true) \
    ENTRY(glTexEnvf, true) \
    ENTRY(glTexImage2D, true) \
    ENTRY(glTexImage3D, false) \
    ENTRY(glTexParameterf, true) \
    ENTRY(glTexParameterfv, true) \
    ENTRY(glTexParameteri, true) \
    ENTRY(glTexSubImage2D, true) \
    ENTRY(glTexSubImage3D, false) \
    ENTRY(glUniform1i, false) \
    ENTRY(glUniform1iARB, false) \
    ENTRY(glUniform1iv, false) \
    ENTRY(glUniform1ivARB, false) \
    ENTRY(glUniform4fv, false) \
    ENTRY(glUniform4fvARB, false) \
    ENTRY(glUniform4iv, false) \
    ENTRY(glUniform4ivARB, false) \
    ENTRY(glUnmapBufferARB, false) \
    ENTRY(glUseProgram, false) \
    ENTRY(glUseProgramObjectARB, false) \
    ENTRY(glVertexAttribPointer, false) \
    ENTRY(glVertexAttribPointerARB, false) \
    ENTRY(glViewport, true)

#define DEF_GL_ENUM(n, core) GLIDX_##n,
enum {
    GL_TABLE_LIST(DEF_GL_ENUM)
    GLIDX_COUNT
};
#undef DEF_GL_ENUM

static uint64_t g_gl_prof_ticks[GLIDX_COUNT];
static uint64_t g_gl_prof_calls[GLIDX_COUNT];
static uint64_t g_gl_win_ticks = 0;
static uint64_t g_gl_win_calls = 0;
static uint64_t g_gl_win_total = 0;
static uint64_t g_gl_win_postfx = 0;
static uint64_t g_gl_win_swap = 0;
static uint64_t g_gl_win_icalls = 0;
static uint32_t g_gl_win_frames = 0;
static bool     g_perf_inited = false;

#ifdef RECOMP_GL_PROF
#define DEF_GL_WRAP(n, core) \
static void glwrap_##n(CPUContext* __restrict__ c, uint8_t* __restrict__ _m) { \
    (void)_m; \
    uint64_t _t0 = gl_prof_tick(); \
    glw_##n(c); \
    uint64_t _dt = gl_prof_tick() - _t0; \
    g_gl_prof_ticks[GLIDX_##n] += _dt; \
    g_gl_prof_calls[GLIDX_##n]++; \
    g_gl_win_ticks += _dt; \
    g_gl_win_calls++; \
    c->esp += 4u; \
}
#else
#define DEF_GL_WRAP(n, core) \
static void glwrap_##n(CPUContext* __restrict__ c, uint8_t* __restrict__ _m) { \
    (void)_m; \
    glw_##n(c); \
    c->esp += 4u; \
}
#endif
GL_TABLE_LIST(DEF_GL_WRAP)
#undef DEF_GL_WRAP

#define ENTRY(n, core) { #n, glwrap_##n, core },
static const GlEntry g_gl_table[] = {
    GL_TABLE_LIST(ENTRY)
};
#undef ENTRY

uint64_t g_phase_ticks[4] = {0, 0, 0, 0};
uint64_t g_recomp_frame_ticks[RECOMP_TIME_COUNT];
uint32_t g_recomp_geometry_calls;
uint64_t recomp_phase_now(void) { return gl_prof_tick(); }

#ifdef RECOMP_SIM_DETAIL_PROF
enum { SIM_TARGET_SLOTS = 64 };
typedef struct SimTargetTiming {
    uint32_t address;
    uint32_t calls;
    uint64_t ticks;
} SimTargetTiming;

bool g_recomp_sample_sim;
_Thread_local bool g_recomp_sample_collision;
static uint32_t g_sim_sample_frames;
static SimTargetTiming g_sim_target_timings[RECOMP_SIM_PHASE_COUNT][SIM_TARGET_SLOTS];

void recomp_profile_sim_target(enum RecompSimPhase phase, uint32_t addr, uint64_t ticks) {
    SimTargetTiming* table = g_sim_target_timings[phase];
    uint32_t index = (addr * 2654435761u) >> 26;
    for (uint32_t probe = 0; probe < SIM_TARGET_SLOTS; ++probe) {
        SimTargetTiming* row = &table[(index + probe) % SIM_TARGET_SLOTS];
        if (row->address != 0 && row->address != addr) continue;
        row->address = addr;
        row->calls++;
        row->ticks += ticks;
        return;
    }
}

static int fmt_sim_targets(char* dst, size_t cap) {
    static const char* const labels[RECOMP_SIM_PHASE_COUNT] = {
        [RECOMP_SIM_ACTIONS] = "actions",
        [RECOMP_SIM_PHYSICS] = "physics",
        [RECOMP_SIM_ANIMATION] = "animation",
        [RECOMP_SIM_COLLISION] = "collision",
        [RECOMP_SIM_COLLISION_DETAIL] = "collision_detail",
        [RECOMP_SIM_PLAYER] = "player",
        [RECOMP_SIM_RENDER] = "render",
        [RECOMP_SIM_FRAME] = "frame",
    };
    double sample_frames = g_sim_sample_frames;
    if (sample_frames == 0) return 0;
    int off = 0;
    for (int phase = 0; phase < RECOMP_SIM_PHASE_COUNT; ++phase) {
        SimTargetTiming* table = g_sim_target_timings[phase];
        SimTargetTiming top[6] = {{0}};
        for (int row = 0; row < SIM_TARGET_SLOTS; ++row) {
            SimTargetTiming item = table[row];
            if (item.calls == 0) continue;
            for (int k = 0; k < 6; ++k) {
                if (item.ticks > top[k].ticks) {
                    for (int j = 5; j > k; --j) top[j] = top[j - 1];
                    top[k] = item;
                    break;
                }
            }
        }
        if (top[0].calls == 0) continue;
        if ((size_t)off < cap)
            off += snprintf(dst + off, cap - (size_t)off, "  %s sample (%u frames)", labels[phase], g_sim_sample_frames);
        for (int i = 0; i < 6; ++i) {
            if (top[i].calls == 0) break;
            if ((size_t)off < cap)
                off += snprintf(dst + off, cap - (size_t)off, " %08x=%.2fms/%.1f", top[i].address,
                                gl_prof_ticks_to_ms(top[i].ticks) / sample_frames, top[i].calls / sample_frames);
        }
        if ((size_t)off < cap) { dst[off++] = '\n'; dst[off] = '\0'; }
    }
    return off;
}
#endif

#define PERF_RING_SIZE 65536u
static char g_perf_ring[PERF_RING_SIZE];
static uint32_t g_perf_head = 0;
static uint32_t g_perf_tail = 0;
static pthread_mutex_t g_perf_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_perf_cv  = PTHREAD_COND_INITIALIZER;
static bool g_perf_thread_started = false;

static void* perf_log_worker(void* arg) {
    (void)arg;
#ifdef __SWITCH__
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x4))) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7);
    }
    const char* perf_path = "sdmc:/switch/psychonauts/perf.log";
#else
    const char* perf_path = "/tmp/pshy_perf.log";
#endif
    char chunk[16384];
    for (;;) {
        pthread_mutex_lock(&g_perf_mtx);
        while (g_perf_head == g_perf_tail) {
            pthread_cond_wait(&g_perf_cv, &g_perf_mtx);
        }
        uint32_t avail = g_perf_head - g_perf_tail;
        if (avail > sizeof(chunk)) avail = sizeof(chunk);
        uint32_t idx = g_perf_tail & (PERF_RING_SIZE - 1u);
        uint32_t first = PERF_RING_SIZE - idx;
        if (first > avail) first = avail;
        memcpy(chunk, g_perf_ring + idx, first);
        if (avail > first) {
            memcpy(chunk + first, g_perf_ring, avail - first);
        }
        g_perf_tail += avail;
        pthread_mutex_unlock(&g_perf_mtx);

        FILE* fp = fopen(perf_path, g_perf_inited ? "a" : "w");
        g_perf_inited = true;
        if (fp) {
            fwrite(chunk, 1, avail, fp);
            fclose(fp);
        }
    }
    return NULL;
}

static void perf_log_enqueue(const char* buf, size_t len) {
    if (len == 0 || len >= PERF_RING_SIZE) return;
    pthread_mutex_lock(&g_perf_mtx);
    if (!g_perf_thread_started) {
        g_perf_thread_started = true;
        pthread_t pt;
        if (pthread_create(&pt, NULL, perf_log_worker, NULL) == 0) {
            pthread_detach(pt);
        }
    }
    uint32_t used = g_perf_head - g_perf_tail;
    if (PERF_RING_SIZE - used > (uint32_t)len) {
        uint32_t idx = g_perf_head & (PERF_RING_SIZE - 1u);
        uint32_t first = PERF_RING_SIZE - idx;
        if (first > (uint32_t)len) first = (uint32_t)len;
        memcpy(g_perf_ring + idx, buf, first);
        if (len > first) {
            memcpy(g_perf_ring, buf + first, len - first);
        }
        g_perf_head += (uint32_t)len;
        pthread_cond_signal(&g_perf_cv);
    }
    pthread_mutex_unlock(&g_perf_mtx);
}

void recomp_gl_prof_end_frame(uint64_t frame_num, uint64_t total_ticks,
                              uint64_t postfx_ticks, uint64_t swap_ticks,
                              uint64_t icalls) {
    g_gl_win_total += total_ticks;
    g_gl_win_postfx += postfx_ticks;
    g_gl_win_swap += swap_ticks;
    g_gl_win_icalls += icalls;
    g_gl_win_frames++;
#ifdef RECOMP_SIM_DETAIL_PROF
    g_sim_sample_frames += g_recomp_sample_sim;
    g_recomp_sample_sim = false;
#endif
    if (g_gl_win_frames < 30u) return;

    uint64_t worker_ticks = atomic_exchange(&g_glq_worker_ticks, 0);
    uint64_t worker_cmd_ticks = atomic_exchange(&g_glq_worker_cmd_ticks, 0);
    uint64_t worker_pfx_ticks = atomic_exchange(&g_glq_worker_pfx_ticks, 0);
    uint64_t worker_swap_ticks = atomic_exchange(&g_glq_worker_swap_ticks, 0);
    uint32_t vao_hits = atomic_exchange(&g_glq_vao_hits, 0);
    uint32_t vao_misses = atomic_exchange(&g_glq_vao_misses, 0);
    uint32_t syncs = atomic_exchange(&g_glq_sync_count, 0);
    char lbuf[4096];
    int off = 0;
    {
        double n = (double)g_gl_win_frames;
        double tot_ms = gl_prof_ticks_to_ms(g_gl_win_total) / n;
        double gl_ms  = gl_prof_ticks_to_ms(g_gl_win_ticks) / n;
        double pfx_ms = gl_prof_ticks_to_ms(g_gl_win_postfx) / n;
        double swp_ms = gl_prof_ticks_to_ms(g_gl_win_swap) / n;
        double gst_ms = tot_ms - gl_ms - pfx_ms - swp_ms;
        double fps = (tot_ms > 0.001) ? (1000.0 / tot_ms) : 0.0;
        double glt_ms = gl_prof_ticks_to_ms(worker_ticks) / n;
        double glt_cmd_ms = gl_prof_ticks_to_ms(worker_cmd_ticks) / n;
        double glt_pfx_ms = gl_prof_ticks_to_ms(worker_pfx_ticks) / n;
        double glt_swap_ms = gl_prof_ticks_to_ms(worker_swap_ticks) / n;
        double p0 = gl_prof_ticks_to_ms(g_phase_ticks[0]) / n;
        double p1 = gl_prof_ticks_to_ms(g_phase_ticks[1]) / n;
        double p2 = gl_prof_ticks_to_ms(g_phase_ticks[2]) / n;
        double p3 = gl_prof_ticks_to_ms(g_phase_ticks[3]) / n;
        off += snprintf(lbuf + off, sizeof(lbuf) - (size_t)off,
                "F=%llu FPS=%.1f tot=%.2fms | guest=%.2fms gl=%.2fms pfx=%.2fms swap=%.2fms | glt=%d glt_ms=%.2f glt_cmd_ms=%.2f glt_pfx_ms=%.2f glt_swap_ms=%.2f vao=%d(h=%.0f m=%.0f) sync=%.1f(%s) | sim=%.1f rsg=%.1f shd=%.1f scr=%.1f"
                " | rsg_n=%.2f request=%.2f pre_mesh=%.2f sort_mesh=%.2f opaque=%.2f trans=%.2f"
                " | sim_scan=%.2f sim_actions=%.2f sim_physics=%.2f affectors=%.2f ring_wait=%.2f swap_wait=%.2f"
                " | array_binds=%.1f array_bind_packets=%.1f icalls/f=%.0f\n",
                (unsigned long long)frame_num, fps, tot_ms, gst_ms, gl_ms, pfx_ms, swp_ms,
                g_glt_active ? 1 : 0, glt_ms, glt_cmd_ms, glt_pfx_ms, glt_swap_ms,
                g_glt_vao_supported ? 1 : 0, (double)vao_hits / n, (double)vao_misses / n,
                (double)syncs / n, syncs ? g_glq_last_sync_name : "-",
                p0, p1, p2, p3, g_recomp_geometry_calls / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_REQUEST_SCENE]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_PREPROCESS_MESHES]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_SORT_MESHES]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_DRAW_OPAQUE]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_DRAW_TRANSPARENT]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_SIM_ENTITY_SCAN]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_SIM_ACTIONS]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_SIM_PHYSICS]) / n,
                gl_prof_ticks_to_ms(g_recomp_frame_ticks[RECOMP_TIME_AFFECTORS]) / n,
                gl_prof_ticks_to_ms(g_glq_space_wait_ticks) / n,
                gl_prof_ticks_to_ms(g_glq_swap_wait_ticks) / n,
                g_glq_array_bind_requests / n, g_glq_array_bind_packets / n,
                (double)g_gl_win_icalls / n);
#ifdef RECOMP_SIM_DETAIL_PROF
        if ((size_t)off < sizeof(lbuf)) {
            off += fmt_sim_targets(lbuf + off, sizeof(lbuf) - (size_t)off);
        }
#endif
        {
            uint32_t c_u4f = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_UNIFORM_4FV], 0, memory_order_relaxed);
            uint32_t c_dre = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_DRAW_RANGE_ELEMENTS], 0, memory_order_relaxed);
            uint32_t c_da  = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_DRAW_ARRAYS], 0, memory_order_relaxed);
            uint32_t c_dac = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_DRAW_ARRAYS_CLIENT], 0, memory_order_relaxed);
            uint32_t c_btx = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_BIND_TEXTURE], 0, memory_order_relaxed);
            uint32_t c_prg = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_USE_PROGRAM], 0, memory_order_relaxed);
            uint32_t c_vap = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_VERTEX_ATTRIB_PTR], 0, memory_order_relaxed);
            uint32_t c_bb  = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_BIND_BUFFER], 0, memory_order_relaxed);
            uint32_t c_ena = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_ENABLE], 0, memory_order_relaxed);
            uint32_t c_dis = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_DISABLE], 0, memory_order_relaxed);
            uint32_t c_eva = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_ENABLE_VAA], 0, memory_order_relaxed);
            uint32_t c_dva = atomic_exchange_explicit(&g_glq_op_counts[GLCMD_DISABLE_VAA], 0, memory_order_relaxed);
            if ((size_t)off < sizeof(lbuf))
                off += snprintf(lbuf + off, sizeof(lbuf) - (size_t)off,
                        "  glcmd/f u4fv=%.0f dre=%.0f da=%.0f dac=%.0f btex=%.0f prog=%.0f vap=%.0f bbuf=%.0f en/dis=%.0f/%.0f vaa=%.0f/%.0f\n",
                        c_u4f / n, c_dre / n, c_da / n, c_dac / n, c_btx / n, c_prg / n, c_vap / n, c_bb / n,
                        c_ena / n, c_dis / n, c_eva / n, c_dva / n);
#ifdef RECOMP_GL_PROF
            {
                static const char* const op_names[GLCMD_COUNT] = {
                    [GLCMD_SYNC_CALL] = "sync", [GLCMD_ACTIVE_TEXTURE] = "atex", [GLCMD_ALPHA_FUNC] = "afnc",
                    [GLCMD_BIND_BUFFER] = "bbuf", [GLCMD_BIND_FRAMEBUFFER] = "bfbo", [GLCMD_BIND_SAMPLER] = "bsmp",
                    [GLCMD_BIND_TEXTURE] = "btex", [GLCMD_BLEND_FUNC] = "bfnc", [GLCMD_BLIT_FRAMEBUFFER] = "blit",
                    [GLCMD_BUFFER_DATA] = "bdat", [GLCMD_BUFFER_SUB_DATA] = "bsub", [GLCMD_CLEAR] = "clr",
                    [GLCMD_COLOR_MASK] = "cmsk", [GLCMD_CULL_FACE] = "cull", [GLCMD_DEPTH_FUNC] = "dfnc",
                    [GLCMD_DEPTH_MASK] = "dmsk", [GLCMD_DISABLE] = "dis", [GLCMD_ENABLE] = "ena",
                    [GLCMD_DISABLE_VAA] = "dvaa", [GLCMD_ENABLE_VAA] = "evaa", [GLCMD_DRAW_ARRAYS] = "da",
                    [GLCMD_DRAW_ARRAYS_CLIENT] = "dac", [GLCMD_DRAW_RANGE_ELEMENTS] = "dre",
                    [GLCMD_SCISSOR] = "scis", [GLCMD_TEX_PARAM_I] = "tpi", [GLCMD_TEX_PARAM_F] = "tpf",
                    [GLCMD_UNIFORM_1I] = "u1i", [GLCMD_UNIFORM_1IV] = "u1iv", [GLCMD_UNIFORM_4FV] = "u4fv",
                    [GLCMD_UNIFORM_4IV] = "u4iv", [GLCMD_USE_PROGRAM] = "prog", [GLCMD_VERTEX_ATTRIB_PTR] = "vap",
                    [GLCMD_VIEWPORT] = "vp", [GLCMD_SWAP] = "swap"
                };
                uint32_t sf = atomic_exchange_explicit(&g_glq_op_sample_frames, 0, memory_order_relaxed);
                uint64_t op_tk[GLCMD_COUNT];
                int ord[GLCMD_COUNT];
                for (int oi = 0; oi < GLCMD_COUNT; oi++) {
                    op_tk[oi] = atomic_exchange_explicit(&g_glq_op_ticks[oi], 0, memory_order_relaxed);
                    ord[oi] = oi;
                }
                if (sf > 0u) {
                    if ((size_t)off < sizeof(lbuf)) off += snprintf(lbuf + off, sizeof(lbuf) - (size_t)off, "  glt_ops(%uf):", sf);
                    for (int k = 0; k < 10; k++) {
                        int best = k;
                        for (int j = k + 1; j < GLCMD_COUNT; j++) {
                            if (op_tk[ord[j]] > op_tk[ord[best]]) best = j;
                        }
                        int tmp = ord[k]; ord[k] = ord[best]; ord[best] = tmp;
                        int oi = ord[k];
                        if (op_tk[oi] == 0) break;
                        const char* nm = (oi < GLCMD_COUNT && op_names[oi]) ? op_names[oi] : "?";
                        if ((size_t)off < sizeof(lbuf))
                            off += snprintf(lbuf + off, sizeof(lbuf) - (size_t)off, " %s=%.2fms", nm, gl_prof_ticks_to_ms(op_tk[oi]) / (double)sf);
                    }
                    if ((size_t)off < sizeof(lbuf)) {
#ifdef __SWITCH__
                        extern _Atomic uint64_t g_nw_wb_ticks;
                        extern _Atomic uint64_t g_nw_fn_ticks;
                        extern _Atomic uint32_t g_nw_wb_count;
                        uint64_t wb_tk = __atomic_exchange_n(&g_nw_wb_ticks, 0, __ATOMIC_RELAXED);
                        uint64_t fn_tk = __atomic_exchange_n(&g_nw_fn_ticks, 0, __ATOMIC_RELAXED);
                        uint32_t wb_cnt = __atomic_exchange_n(&g_nw_wb_count, 0, __ATOMIC_RELAXED);
                        off += snprintf(lbuf + off, sizeof(lbuf) - (size_t)off, " | nw_wb=%.2fms(%u) nw_fn=%.2fms",
                                        gl_prof_ticks_to_ms(wb_tk) / n, wb_cnt, gl_prof_ticks_to_ms(fn_tk) / n);
#endif
                        if ((size_t)off < sizeof(lbuf)) { lbuf[off++] = '\n'; lbuf[off] = '\0'; }
                    }
                }
            }
#endif
        }

        /* Top 10 GL functions by time */
        int order[GLIDX_COUNT];
        for (int i = 0; i < GLIDX_COUNT; i++) order[i] = i;
        for (int k = 0; k < 10 && k < GLIDX_COUNT; k++) {
            int best = k;
            for (int j = k + 1; j < GLIDX_COUNT; j++) {
                if (g_gl_prof_ticks[order[j]] > g_gl_prof_ticks[order[best]]) best = j;
            }
            int tmp = order[k]; order[k] = order[best]; order[best] = tmp;
            int idx = order[k];
            if (g_gl_prof_calls[idx] == 0) break;
            if ((size_t)off < sizeof(lbuf))
                off += snprintf(lbuf + off, sizeof(lbuf) - (size_t)off,
                        "  %-28s %6.2f ms  (%7.1f calls/f)\n",
                        g_gl_table[idx].name,
                        gl_prof_ticks_to_ms(g_gl_prof_ticks[idx]) / n,
                        (double)g_gl_prof_calls[idx] / n);
        }
        if (off > (int)sizeof(lbuf) - 1) off = (int)sizeof(lbuf) - 1;
        if (off > 0) perf_log_enqueue(lbuf, (size_t)off);
    }

    memset(g_gl_prof_ticks, 0, sizeof(g_gl_prof_ticks));
    memset(g_gl_prof_calls, 0, sizeof(g_gl_prof_calls));
    memset(g_phase_ticks, 0, sizeof(g_phase_ticks));
    memset(g_recomp_frame_ticks, 0, sizeof(g_recomp_frame_ticks));
    g_recomp_geometry_calls = 0;
    g_glq_space_wait_ticks = 0;
    g_glq_swap_wait_ticks = 0;
    g_glq_array_bind_requests = 0;
    g_glq_array_bind_packets = 0;
#ifdef RECOMP_SIM_DETAIL_PROF
    memset(g_sim_target_timings, 0, sizeof(g_sim_target_timings));
    g_sim_sample_frames = 0;
#endif
    g_gl_win_ticks = 0;
    g_gl_win_calls = 0;
    g_gl_win_total = 0;
    g_gl_win_postfx = 0;
    g_gl_win_swap = 0;
    g_gl_win_icalls = 0;
    g_gl_win_frames = 0;
}

uint32_t recomp_gl_get_proc_address(const char* name) {
    if (!name) return 0;
    ensure_gl_loaded();
    const size_t n = sizeof(g_gl_table) / sizeof(g_gl_table[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(g_gl_table[i].name, name) == 0) {
            if (!g_gl_table[i].core_always && !host_gl_get_proc(name)) {
                return 0;
            }
            return 0xF0010000u + (uint32_t)(i * 4u);
        }
    }
    fprintf(stderr, "[gl] Warning: unknown GL proc requested: %s\n", name);
    return 0;
}

RecompFn recomp_gl_lookup(uint32_t synth_addr) {
    uint32_t idx = (synth_addr - 0xF0010000u) >> 2;
    const size_t n = sizeof(g_gl_table) / sizeof(g_gl_table[0]);
    if (idx < n) return g_gl_table[idx].fn;
    return NULL;
}

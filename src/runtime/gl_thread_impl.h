/*
 * Dedicated Async OpenGL Worker Thread (Lock-Free SPSC Ring Buffer)
 * Included by src/runtime/hle_gl.c.
 *
 * Moves all Mesa / OpenGL / PostFX / SwapWindow execution off the main guest
 * CPU thread (Core 0) onto a dedicated worker thread (Core 1 on Switch) with
 * a 2 MB stack, avoiding Mesa's unstable mesa_glthread while overlapping GL
 * submission 100% in parallel with recompiled guest CPU execution.
 */
#ifndef RECOMP_GL_THREAD_IMPL_H
#define RECOMP_GL_THREAD_IMPL_H

#ifndef RECOMP_GL_PROF
#define RECOMP_GL_PROF 1
#endif

#include <stdatomic.h>
#include <SDL2/SDL.h>
#include "runtime/postfx.h"

#define GLQ_SIZE (8u * 1024u * 1024u)
#define GLQ_MASK (GLQ_SIZE - 1u)
#define GLQ_MAX_INLINE_DATA (2u * 1024u * 1024u)

enum GlCmdOp {
    GLCMD_WRAP = 0,
    GLCMD_SYNC_CALL,
    GLCMD_ACTIVE_TEXTURE,
    GLCMD_ALPHA_FUNC,
    GLCMD_BEGIN_QUERY,
    GLCMD_END_QUERY,
    GLCMD_BIND_BUFFER,
    GLCMD_BIND_FRAMEBUFFER,
    GLCMD_BIND_PROGRAM_ARB,
    GLCMD_BIND_RENDERBUFFER,
    GLCMD_BIND_SAMPLER,
    GLCMD_BIND_TEXTURE,
    GLCMD_BLEND_EQUATION,
    GLCMD_BLEND_FUNC,
    GLCMD_BLIT_FRAMEBUFFER,
    GLCMD_BUFFER_DATA,
    GLCMD_BUFFER_SUB_DATA,
    GLCMD_BUFFER_PARAM_APPLE,
    GLCMD_CLEAR,
    GLCMD_CLEAR_COLOR,
    GLCMD_CLEAR_DEPTH,
    GLCMD_CLEAR_STENCIL,
    GLCMD_COLOR_MASK,
    GLCMD_CULL_FACE,
    GLCMD_DEPTH_FUNC,
    GLCMD_DEPTH_MASK,
    GLCMD_DEPTH_RANGE,
    GLCMD_DISABLE,
    GLCMD_ENABLE,
    GLCMD_DISABLE_VAA,
    GLCMD_ENABLE_VAA,
    GLCMD_DRAW_ARRAYS,
    GLCMD_DRAW_ARRAYS_CLIENT,
    GLCMD_DRAW_BUFFER,
    GLCMD_DRAW_RANGE_ELEMENTS,
    GLCMD_FOGFV,
    GLCMD_FB_RENDERBUFFER,
    GLCMD_FB_TEXTURE2D,
    GLCMD_GENERATE_MIPMAP,
    GLCMD_PIXEL_STOREI,
    GLCMD_POINT_PARAM_F,
    GLCMD_POINT_PARAM_FV,
    GLCMD_POINT_SIZE,
    GLCMD_POLYGON_MODE,
    GLCMD_POLYGON_OFFSET,
    GLCMD_PROGRAM_LOCAL_4FV,
    GLCMD_PROGRAM_LOCAL_I4IV,
    GLCMD_SAMPLER_PARAM_F,
    GLCMD_SAMPLER_PARAM_I,
    GLCMD_SAMPLER_PARAM_FV,
    GLCMD_SCISSOR,
    GLCMD_SHADE_MODEL,
    GLCMD_STENCIL_FUNC,
    GLCMD_STENCIL_MASK,
    GLCMD_STENCIL_OP,
    GLCMD_TEX_ENVF,
    GLCMD_TEX_PARAM_F,
    GLCMD_TEX_PARAM_I,
    GLCMD_TEX_PARAM_FV,
    GLCMD_UNIFORM_1I,
    GLCMD_UNIFORM_1IV,
    GLCMD_UNIFORM_4FV,
    GLCMD_UNIFORM_4IV,
    GLCMD_USE_PROGRAM,
    GLCMD_VERTEX_ATTRIB_PTR,
    GLCMD_VIEWPORT,
    GLCMD_SWAP,
    GLCMD_SET_SWAP_INTERVAL,
    GLCMD_RESIZE,
    GLCMD_SHUTDOWN,
    GLCMD_COUNT
};

static _Atomic uint32_t g_glq_op_counts[GLCMD_COUNT];
#ifdef RECOMP_GL_PROF
static _Atomic uint64_t g_glq_op_ticks[GLCMD_COUNT];
static _Atomic uint32_t g_glq_op_sample_frames = 0;
#endif

/* Re-enabled: R2 glt_ops proved R1 Camp Vista stall was clr=8.79ms VSync
 * dequeueBuffer (not VAO), and VAO saved 0.78ms in Cabin Spawn (20.32->19.54ms). */
#ifndef GLT_ENABLE_VAO_CACHE
#define GLT_ENABLE_VAO_CACHE 1
#endif

typedef struct GlCmdHdr {
    uint16_t op;
    uint16_t flags;
    uint32_t size; /* total aligned packet size in bytes (multiple of 8) */
} GlCmdHdr;

typedef void (*GlSyncFn)(CPUContext* c);

typedef struct GlCmdSyncCall {
    GlCmdHdr h;
    GlSyncFn fn;
    CPUContext* ctx;
} GlCmdSyncCall;

typedef struct GlCmd1U { GlCmdHdr h; uint32_t a; } GlCmd1U;
typedef struct GlCmd2U { GlCmdHdr h; uint32_t a, b; } GlCmd2U;
typedef struct GlCmd3U { GlCmdHdr h; uint32_t a, b, c; } GlCmd3U;
typedef struct GlCmd4U { GlCmdHdr h; uint32_t a, b, c, d; } GlCmd4U;
typedef struct GlCmd5U { GlCmdHdr h; uint32_t a, b, c, d, e; } GlCmd5U;
typedef struct GlCmd6U { GlCmdHdr h; uint32_t a, b, c, d, e, f; } GlCmd6U;
typedef struct GlCmd10I { GlCmdHdr h; int32_t v[10]; } GlCmd10I;
typedef struct GlCmd1F { GlCmdHdr h; float f0; } GlCmd1F;
typedef struct GlCmd2F { GlCmdHdr h; float f0, f1; } GlCmd2F;
typedef struct GlCmd4F { GlCmdHdr h; float f[4]; } GlCmd4F;
typedef struct GlCmd1U1F { GlCmdHdr h; uint32_t a; float f0; } GlCmd1U1F;
typedef struct GlCmd2U1F { GlCmdHdr h; uint32_t a, b; float f0; } GlCmd2U1F;
typedef struct GlCmd1U4F { GlCmdHdr h; uint32_t a; float f[4]; } GlCmd1U4F;
typedef struct GlCmd2U4F { GlCmdHdr h; uint32_t a, b; float f[4]; } GlCmd2U4F;
typedef struct GlCmd2U4I { GlCmdHdr h; uint32_t a, b; int32_t v[4]; } GlCmd2U4I;
typedef struct GlCmd1D { GlCmdHdr h; double d0; } GlCmd1D;
typedef struct GlCmd2D { GlCmdHdr h; double d0, d1; } GlCmd2D;
typedef struct GlCmdSwap { GlCmdHdr h; int32_t w, h_px; uint64_t frame_id; } GlCmdSwap;
typedef struct GlCmdResize { GlCmdHdr h; int32_t win_w, win_h, guest_w, guest_h, swap_interval; } GlCmdResize;

typedef struct GlCmdBufData {
    GlCmdHdr h;
    uint32_t target;
    uint32_t data_size;
    uint32_t usage;
    uint32_t has_data;
    /* followed by data_size bytes if has_data != 0 */
} GlCmdBufData;

typedef struct GlCmdBufSubData {
    GlCmdHdr h;
    uint32_t target;
    int32_t  offset;
    uint32_t data_size;
    /* followed by data_size bytes */
} GlCmdBufSubData;

typedef struct GlCmdUniformV {
    GlCmdHdr h;
    int32_t  loc;
    int32_t  count;
    uint32_t is_arb;
    /* followed by count * 4 (1iv) or count * 16 (4fv/4iv) bytes */
} GlCmdUniformV;

typedef struct GlCmdClientAttrDesc {
    uint32_t index;
    int32_t  size;
    uint32_t type;
    uint32_t normalized;
    int32_t  stride;
    uint32_t is_arb;
    uint32_t data_off; /* byte offset from start of GlCmdDrawArraysClient */
    uint32_t _pad;
} GlCmdClientAttrDesc;

typedef struct GlCmdDrawArraysClient {
    GlCmdHdr h;
    uint32_t mode;
    int32_t  count;
    uint32_t num_attrs;
    uint32_t _pad;
    /* followed by num_attrs * GlCmdClientAttrDesc, then inline vertex data */
} GlCmdDrawArraysClient;

typedef struct GlCmdVAP {
    GlCmdHdr h;
    uint32_t index;
    int32_t  size;
    uint32_t type;
    uint32_t normalized;
    int32_t  stride;
    uint32_t is_arb;
    uint64_t ptr;
} GlCmdVAP;

/* Ring buffer state */
static uint8_t g_glq_buf[GLQ_SIZE] __attribute__((aligned(64)));
static _Atomic uint32_t g_glq_write __attribute__((aligned(64))) = 0;
static _Atomic uint32_t g_glq_read  __attribute__((aligned(64))) = 0;
static _Atomic int      g_glq_sleeping __attribute__((aligned(64))) = 0;
static _Atomic int      g_glq_sync_done __attribute__((aligned(64))) = 0;
static _Atomic uint64_t g_glq_completed_frame __attribute__((aligned(64))) = 0;
static _Atomic uint64_t g_glq_last_pfx_ticks = 0;
static _Atomic uint64_t g_glq_last_swap_ticks = 0;
static _Atomic uint64_t g_glq_worker_ticks = 0;
static _Atomic uint64_t g_glq_worker_cmd_ticks = 0;
static _Atomic uint64_t g_glq_worker_pfx_ticks = 0;
static _Atomic uint64_t g_glq_worker_swap_ticks = 0;
static _Atomic uint32_t g_glq_vao_hits = 0;
static _Atomic uint32_t g_glq_vao_misses = 0;
static _Atomic uint32_t g_glq_sync_count = 0;
static const char*      g_glq_last_sync_name = "";

/* Worker-private batch timing & VAO cache state (Core 1 only) */
static uint64_t g_glt_batch_pfx_ticks = 0;
static uint64_t g_glt_batch_swap_ticks = 0;
#ifdef __SWITCH__
static uint64_t g_glt_prev_swap_tick = 0;
static int      g_glt_requested_swap_interval = 1;
#endif

#define GLT_MAX_VAA       16u
#define GLT_VAO_HASH_SIZE 2048u
#define GLT_VAO_HASH_MASK (GLT_VAO_HASH_SIZE - 1u)
#define GLT_VAO_POOL_MAX  1024u

typedef struct GltAttrState {
    uint64_t ptr;
    GLuint   buf;
    int32_t  stride;
    GLenum   type;
    int32_t  size;
    uint8_t  norm;
    uint8_t  is_arb;
    uint8_t  valid; /* 0 = unset, 1 = VBO, 2 = client */
    uint8_t  _pad;
} GltAttrState;

typedef struct GltVaoAttrKey {
    uint32_t ptr;
    GLuint   buf;
    int32_t  stride;
    GLenum   type;
    int16_t  size;
    uint8_t  norm;
    uint8_t  _pad;
} GltVaoAttrKey;

typedef struct GltVaoPoolEntry {
    GLuint        vao;
    GLuint        elem_buf;
    uint16_t      vaa_mask;
    uint16_t      hw_vaa_mask;
    uint32_t      last_frame;
    uint32_t      hash;
    uint8_t       active;
    uint8_t       _pad[3];
    GltVaoAttrKey attrs[GLT_MAX_VAA];
} GltVaoPoolEntry;

typedef struct GltVaoHashSlot {
    uint32_t hash;
    uint16_t pool_idx;
    uint8_t  state; /* 0 = empty, 1 = occupied, 2 = tombstone */
    uint8_t  _pad;
} GltVaoHashSlot;

static bool            g_glt_vao_supported = false;
static GltVaoHashSlot  g_glt_vao_hash[GLT_VAO_HASH_SIZE];
static GltVaoPoolEntry g_glt_vao_pool[GLT_VAO_POOL_MAX];
static uint32_t        g_glt_vao_pool_count = 0;
static uint32_t        g_glt_vao_active_count = 0;
static uint32_t        g_glt_vao_tombstones = 0;
static uint32_t        g_glt_vao_evict_cursor = 0;
static uint32_t        g_glt_vao_frame = 1;

static GLuint          g_glt_hw_vao = 0;
static GLuint          g_glt_hw_vao_elem = 0;
static GLuint          g_glt_hw_array_buf = 0;
static bool            g_glt_hw_array_buf_known = false;
static GLuint          g_glt_vao0_elem_buf = 0;
static bool            g_glt_vao0_elem_known = false;
static uint16_t        g_glt_vao0_vaa_mask = 0;
static bool            g_glt_vao0_vaa_known = false;

static GLuint          g_glt_log_array_buf = 0;
static GLuint          g_glt_log_elem_buf = 0;
static uint16_t        g_glt_log_vaa_mask = 0;
static GltAttrState    g_glt_log_attrs[GLT_MAX_VAA];
static bool            g_glt_vao_dirty = true;
static uint32_t        g_glt_local_vao_hits = 0;
static uint32_t        g_glt_local_vao_misses = 0;

static uint32_t         g_glq_local_write = 0;
static uint32_t         g_glq_cached_read = 0;
static uint64_t         g_glq_submitted_frame = 0;
static uint32_t         g_glq_unflushed_bytes = 0;
static uint64_t         g_glq_space_wait_ticks = 0;
static uint64_t         g_glq_swap_wait_ticks = 0;
static GLuint           g_glq_array_binding = 0;
static bool             g_glq_array_binding_known = false;
static bool             g_glq_array_unbind_pending = false;
static GLuint           g_glq_elem_binding = 0;
static bool             g_glq_elem_binding_known = false;
static bool             g_glq_elem_unbind_pending = false;
static uint32_t         g_glq_array_bind_requests = 0;
static uint32_t         g_glq_array_bind_packets = 0;

static bool             g_glt_active = false;
static pthread_t        g_glt_thread;
static pthread_mutex_t  g_glt_wake_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_glt_wake_cv  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t  g_glt_sync_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_glt_sync_cv  = PTHREAD_COND_INITIALIZER;

static SDL_Window*      g_glt_window = NULL;
static SDL_GLContext    g_glt_ctx = NULL;
static volatile bool    g_glt_ready = false;

static inline void glq_cpu_yield(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#endif
}

static inline void glq_wake_consumer(void) {
    if (__builtin_expect(atomic_load_explicit(&g_glq_sleeping, memory_order_acquire), 0)) {
        pthread_mutex_lock(&g_glt_wake_mtx);
        pthread_cond_signal(&g_glt_wake_cv);
        pthread_mutex_unlock(&g_glt_wake_mtx);
    }
}

static inline void glq_publish(bool force_wake) {
    atomic_store_explicit(&g_glq_write, g_glq_local_write, memory_order_release);
    g_glq_unflushed_bytes = 0;
    if (force_wake) glq_wake_consumer();
}

static __attribute__((noinline)) void* glq_alloc_slow(uint32_t sz) {
    uint64_t wait_start = 0;
    for (;;) {
        uint32_t w = g_glq_local_write;
        uint32_t off = w & GLQ_MASK;
        uint32_t tail_space = GLQ_SIZE - off;

        /* Need room for wrap header (8 bytes) if packet doesn't fit at tail */
        if (tail_space < sz + 8u && off != 0) {
            /* Wait until consumer has freed at least tail_space + sz + 8u */
            uint32_t used = w - g_glq_cached_read;
            if (GLQ_SIZE - used < tail_space + sz + 8u) {
                glq_publish(true);
                g_glq_cached_read = atomic_load_explicit(&g_glq_read, memory_order_acquire);
                used = w - g_glq_cached_read;
                if (GLQ_SIZE - used < tail_space + sz + 8u) {
                    if (!wait_start) wait_start = gl_prof_tick();
                    glq_cpu_yield();
                    continue;
                }
            }
            GlCmdHdr* wh = (GlCmdHdr*)(g_glq_buf + off);
            wh->op = GLCMD_WRAP;
            wh->flags = 0;
            wh->size = tail_space;
            g_glq_local_write = w + tail_space;
            g_glq_unflushed_bytes += tail_space;
            continue;
        }

        uint32_t used = w - g_glq_cached_read;
        if (GLQ_SIZE - used < sz + 8u) {
            glq_publish(true);
            g_glq_cached_read = atomic_load_explicit(&g_glq_read, memory_order_acquire);
            used = w - g_glq_cached_read;
            if (GLQ_SIZE - used < sz + 8u) {
                if (!wait_start) wait_start = gl_prof_tick();
                glq_cpu_yield();
                continue;
            }
        }

        void* ptr = g_glq_buf + off;
        g_glq_local_write = w + sz;
        g_glq_unflushed_bytes += sz;
        if (wait_start) g_glq_space_wait_ticks += gl_prof_tick() - wait_start;
        return ptr;
    }
}

static inline void* glq_alloc(uint32_t raw_size) {
    uint32_t sz = (raw_size + 7u) & ~7u;
    uint32_t w = g_glq_local_write;
    uint32_t off = w & GLQ_MASK;
    uint32_t used = w - g_glq_cached_read;
    if (__builtin_expect(off + sz + 8u <= GLQ_SIZE && used + sz + 8u <= GLQ_SIZE, 1)) {
        g_glq_local_write = w + sz;
        g_glq_unflushed_bytes += sz;
        return g_glq_buf + off;
    }
    return glq_alloc_slow(sz);
}

static inline void glq_commit(void) {
    if (__builtin_expect(g_glq_unflushed_bytes >= 4096u, 0)) {
        glq_publish(true);
    }
}

static inline void glq_submit_array_binding(GLuint buffer) {
    GlCmd2U* cmd = glq_alloc(sizeof(*cmd));
    cmd->h = (GlCmdHdr){GLCMD_BIND_BUFFER, 0, sizeof(*cmd)};
    cmd->a = GL_ARRAY_BUFFER;
    cmd->b = buffer;
    g_glq_array_binding = buffer;
    g_glq_array_binding_known = true;
    g_glq_array_unbind_pending = false;
    g_glq_array_bind_packets++;
    glq_commit();
}

/* VBO draws use the binding saved with each attribute, so they can pass an
 * outstanding unbind. Flush before a command that reads the current binding. */
static inline void glq_flush_array_unbind(void) {
    if (__builtin_expect(g_glq_array_unbind_pending, 0)) glq_submit_array_binding(0);
}

static inline void glq_bind_array_buffer(GLuint buffer) {
    g_glq_array_bind_requests++;
    if (g_glq_array_binding_known && g_glq_array_binding == buffer) {
        g_glq_array_unbind_pending = false;
    } else if (buffer == 0) {
        g_glq_array_unbind_pending = true;
    } else {
        glq_submit_array_binding(buffer);
    }
}

static inline void glq_submit_elem_binding(GLuint buffer) {
    GlCmd2U* cmd = glq_alloc(sizeof(*cmd));
    cmd->h = (GlCmdHdr){GLCMD_BIND_BUFFER, 0, sizeof(*cmd)};
    cmd->a = GL_ELEMENT_ARRAY_BUFFER;
    cmd->b = buffer;
    g_glq_elem_binding = buffer;
    g_glq_elem_binding_known = true;
    g_glq_elem_unbind_pending = false;
    glq_commit();
}

static inline void glq_flush_elem_unbind(void) {
    if (__builtin_expect(g_glq_elem_unbind_pending, 0)) glq_submit_elem_binding(0);
}

static inline void glq_bind_elem_buffer(GLuint buffer) {
    if (g_glq_elem_binding_known && g_glq_elem_binding == buffer) {
        g_glq_elem_unbind_pending = false;
    } else if (buffer == 0) {
        g_glq_elem_unbind_pending = true;
    } else {
        glq_submit_elem_binding(buffer);
    }
}

static void glq_sync_call(GlSyncFn fn, CPUContext* c, const char* name) {
    glq_flush_array_unbind();
    glq_flush_elem_unbind();
    atomic_fetch_add_explicit(&g_glq_sync_count, 1u, memory_order_relaxed);
    g_glq_last_sync_name = name ? name : "?";
    atomic_store_explicit(&g_glq_sync_done, 0, memory_order_relaxed);
    GlCmdSyncCall* cmd = (GlCmdSyncCall*)glq_alloc(sizeof(GlCmdSyncCall));
    cmd->h.op = GLCMD_SYNC_CALL;
    cmd->h.flags = 0;
    cmd->h.size = (sizeof(GlCmdSyncCall) + 7u) & ~7u;
    cmd->fn = fn;
    cmd->ctx = c;
    glq_publish(true);

    for (int spin = 0; spin < 4000; spin++) {
        if (atomic_load_explicit(&g_glq_sync_done, memory_order_acquire)) return;
        glq_cpu_yield();
    }
    pthread_mutex_lock(&g_glt_sync_mtx);
    while (!atomic_load_explicit(&g_glq_sync_done, memory_order_acquire)) {
        pthread_cond_wait(&g_glt_sync_cv, &g_glt_sync_mtx);
    }
    pthread_mutex_unlock(&g_glt_sync_mtx);
}

/* ---------------- Worker Thread VAO Cache & Execution ---------------- */

static inline void glt_bind_vao0(void) {
    if (g_glt_hw_vao != 0) {
        pfn_glBindVertexArray(0);
        g_glt_hw_vao = 0;
        g_glt_hw_vao_elem = 0;
        g_glt_vao_dirty = true;
    }
}

static inline void glt_bind_hw_array_buf(GLuint buf) {
    if (!g_glt_hw_array_buf_known || g_glt_hw_array_buf != buf) {
        if (pfn_glBindBufferARB) pfn_glBindBufferARB(GL_ARRAY_BUFFER, buf);
        g_glt_hw_array_buf = buf;
        g_glt_hw_array_buf_known = true;
    }
}

static inline void glt_bind_vao0_elem_buf(GLuint buf) {
    if (!g_glt_vao0_elem_known || g_glt_vao0_elem_buf != buf) {
        if (pfn_glBindBufferARB) pfn_glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER, buf);
        g_glt_vao0_elem_buf = buf;
        g_glt_vao0_elem_known = true;
    }
}

static void glt_sync_vao0_vaa_mask(uint16_t target_mask) {
    uint16_t diff = g_glt_vao0_vaa_known ? (uint16_t)(g_glt_vao0_vaa_mask ^ target_mask) : 0xFFFFu;
    while (diff) {
        uint32_t i = (uint32_t)__builtin_ctz(diff);
        diff &= (uint16_t)(diff - 1u);
        if (target_mask & (1u << i)) {
            if (pfn_glEnableVertexAttribArrayARB) pfn_glEnableVertexAttribArrayARB(i);
            else if (pfn_glEnableVertexAttribArray) pfn_glEnableVertexAttribArray(i);
        } else {
            if (pfn_glDisableVertexAttribArrayARB) pfn_glDisableVertexAttribArrayARB(i);
            else if (pfn_glDisableVertexAttribArray) pfn_glDisableVertexAttribArray(i);
        }
    }
    g_glt_vao0_vaa_mask = target_mask;
    g_glt_vao0_vaa_known = true;
}

static void glt_flush_all_to_vao0(bool need_attrs) {
    glt_bind_vao0();
    glt_bind_vao0_elem_buf(g_glt_log_elem_buf);
    if (need_attrs) {
        glt_sync_vao0_vaa_mask(g_glt_log_vaa_mask);
        uint16_t m = g_glt_log_vaa_mask;
        while (m) {
            uint32_t i = (uint32_t)__builtin_ctz(m);
            m &= (uint16_t)(m - 1u);
            const GltAttrState* a = &g_glt_log_attrs[i];
            if (a->valid) {
                glt_bind_hw_array_buf(a->buf);
                const void* p = (const void*)(uintptr_t)a->ptr;
                if (a->is_arb && pfn_glVertexAttribPointerARB)
                    pfn_glVertexAttribPointerARB(i, a->size, a->type, a->norm, a->stride, p);
                else if (pfn_glVertexAttribPointer)
                    pfn_glVertexAttribPointer(i, a->size, a->type, a->norm, a->stride, p);
            }
        }
    }
    glt_bind_hw_array_buf(g_glt_log_array_buf);
}

static void glt_vao_cache_invalidate_all(void) {
    if (!g_glt_vao_supported) return;
    glt_bind_vao0();
    memset(g_glt_vao_hash, 0, sizeof(g_glt_vao_hash));
    for (uint32_t i = 0; i < g_glt_vao_pool_count; i++) {
        g_glt_vao_pool[i].active = 0;
    }
    g_glt_hw_array_buf_known = false;
    g_glt_vao0_elem_known = false;
    g_glt_vao_active_count = 0;
    g_glt_vao_tombstones = 0;
    g_glt_vao_evict_cursor = 0;
    g_glt_vao_dirty = true;
}

static void glt_vao_hash_rehash(void) {
    memset(g_glt_vao_hash, 0, sizeof(g_glt_vao_hash));
    g_glt_vao_tombstones = 0;
    for (uint32_t i = 0; i < g_glt_vao_pool_count; i++) {
        if (!g_glt_vao_pool[i].active) continue;
        uint32_t h = g_glt_vao_pool[i].hash;
        uint32_t slot = h & GLT_VAO_HASH_MASK;
        while (g_glt_vao_hash[slot].state == 1) slot = (slot + 1u) & GLT_VAO_HASH_MASK;
        g_glt_vao_hash[slot].hash = h;
        g_glt_vao_hash[slot].pool_idx = (uint16_t)i;
        g_glt_vao_hash[slot].state = 1;
    }
}

static void glt_vao_prepare_draw(bool indexed) {
    if (!g_glt_vao_supported) return;
    if (!g_glt_vao_dirty && g_glt_hw_vao != 0 && (!indexed || g_glt_hw_vao_elem == g_glt_log_elem_buf)) {
        g_glt_local_vao_hits++;
        return;
    }
    uint16_t mask = g_glt_log_vaa_mask;
    GLuint elem = indexed ? g_glt_log_elem_buf : 0u;
    if (mask == 0 || (indexed && elem == 0)) {
        glt_flush_all_to_vao0(true);
        return;
    }
    uint32_t h = 2166136261u;
    h = (h ^ elem) * 16777619u;
    h = (h ^ (uint32_t)mask) * 16777619u;
    uint16_t m = mask;
    while (m) {
        uint32_t i = (uint32_t)__builtin_ctz(m);
        m &= (uint16_t)(m - 1u);
        const GltAttrState* a = &g_glt_log_attrs[i];
        if (a->valid != 1 || a->buf == 0) {
            glt_flush_all_to_vao0(true);
            return;
        }
        h = (h ^ (uint32_t)a->ptr) * 16777619u;
        h = (h ^ a->buf) * 16777619u;
        h = (h ^ (uint32_t)a->stride) * 16777619u;
        h = (h ^ (a->type ^ ((uint32_t)a->size << 16) ^ ((uint32_t)a->norm << 24))) * 16777619u;
    }
    if (h == 0) h = 1u;

    uint32_t slot = h & GLT_VAO_HASH_MASK;
    uint32_t first_tomb = GLT_VAO_HASH_SIZE;
    for (uint32_t probe = 0; probe < GLT_VAO_HASH_SIZE; probe++) {
        GltVaoHashSlot* hs = &g_glt_vao_hash[slot];
        if (hs->state == 0) {
            if (first_tomb < GLT_VAO_HASH_SIZE) slot = first_tomb;
            break;
        }
        if (hs->state == 2) {
            if (first_tomb == GLT_VAO_HASH_SIZE) first_tomb = slot;
        } else if (hs->hash == h) {
            GltVaoPoolEntry* e = &g_glt_vao_pool[hs->pool_idx];
            if (e->elem_buf == elem && e->vaa_mask == mask) {
                bool match = true;
                uint16_t cm = mask;
                while (cm) {
                    uint32_t i = (uint32_t)__builtin_ctz(cm);
                    cm &= (uint16_t)(cm - 1u);
                    const GltAttrState* a = &g_glt_log_attrs[i];
                    const GltVaoAttrKey* k = &e->attrs[i];
                    if (k->buf != a->buf || k->ptr != (uint32_t)a->ptr ||
                        k->stride != a->stride || k->type != a->type ||
                        k->size != (int16_t)a->size || k->norm != a->norm) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    e->last_frame = g_glt_vao_frame;
                    if (g_glt_hw_vao != e->vao) {
                        pfn_glBindVertexArray(e->vao);
                        g_glt_hw_vao = e->vao;
                    }
                    g_glt_hw_vao_elem = elem;
                    g_glt_vao_dirty = false;
                    g_glt_local_vao_hits++;
                    return;
                }
            }
        }
        slot = (slot + 1u) & GLT_VAO_HASH_MASK;
    }

    /* Cache miss */
    g_glt_local_vao_misses++;
    uint16_t pidx = 0xFFFFu;
    if (g_glt_vao_active_count < g_glt_vao_pool_count) {
        for (uint32_t i = 0; i < g_glt_vao_pool_count; i++) {
            uint32_t idx = (g_glt_vao_evict_cursor + i) % g_glt_vao_pool_count;
            if (!g_glt_vao_pool[idx].active) {
                pidx = (uint16_t)idx;
                g_glt_vao_evict_cursor = (idx + 1u) % g_glt_vao_pool_count;
                break;
            }
        }
    }
    if (pidx == 0xFFFFu && g_glt_vao_pool_count < GLT_VAO_POOL_MAX) {
        pidx = (uint16_t)g_glt_vao_pool_count++;
        memset(&g_glt_vao_pool[pidx], 0, sizeof(GltVaoPoolEntry));
        pfn_glGenVertexArrays(1, &g_glt_vao_pool[pidx].vao);
    }
    if (pidx == 0xFFFFu) {
        uint32_t best = g_glt_vao_evict_cursor % GLT_VAO_POOL_MAX;
        uint32_t best_frame = g_glt_vao_pool[best].last_frame;
        for (uint32_t i = 0; i < GLT_VAO_POOL_MAX; i++) {
            uint32_t idx = (g_glt_vao_evict_cursor + i) % GLT_VAO_POOL_MAX;
            if (g_glt_vao_pool[idx].last_frame < g_glt_vao_frame) {
                best = idx;
                break;
            }
            if (g_glt_vao_pool[idx].last_frame < best_frame) {
                best_frame = g_glt_vao_pool[idx].last_frame;
                best = idx;
            }
        }
        g_glt_vao_evict_cursor = (best + 1u) % GLT_VAO_POOL_MAX;
        pidx = (uint16_t)best;
        uint32_t old_s = g_glt_vao_pool[pidx].hash & GLT_VAO_HASH_MASK;
        for (uint32_t p = 0; p < GLT_VAO_HASH_SIZE; p++) {
            if (g_glt_vao_hash[old_s].state == 0) break;
            if (g_glt_vao_hash[old_s].state == 1 && g_glt_vao_hash[old_s].pool_idx == pidx) {
                g_glt_vao_hash[old_s].state = 2;
                g_glt_vao_tombstones++;
                break;
            }
            old_s = (old_s + 1u) & GLT_VAO_HASH_MASK;
        }
        g_glt_vao_pool[pidx].active = 0;
        g_glt_vao_active_count--;
        if (g_glt_vao_tombstones > 256u) {
            glt_vao_hash_rehash();
            slot = h & GLT_VAO_HASH_MASK;
            while (g_glt_vao_hash[slot].state == 1) slot = (slot + 1u) & GLT_VAO_HASH_MASK;
        }
    }

    GltVaoPoolEntry* e = &g_glt_vao_pool[pidx];
    if (e->vao == 0) {
        glt_flush_all_to_vao0(true);
        return;
    }

    pfn_glBindVertexArray(e->vao);
    g_glt_hw_vao = e->vao;
    g_glt_hw_vao_elem = elem;

    uint16_t to_disable = (uint16_t)(e->hw_vaa_mask & ~mask);
    while (to_disable) {
        uint32_t i = (uint32_t)__builtin_ctz(to_disable);
        to_disable &= (uint16_t)(to_disable - 1u);
        if (pfn_glDisableVertexAttribArrayARB) pfn_glDisableVertexAttribArrayARB(i);
        else if (pfn_glDisableVertexAttribArray) pfn_glDisableVertexAttribArray(i);
    }
    if (pfn_glBindBufferARB) pfn_glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER, elem);

    uint16_t cm = mask;
    while (cm) {
        uint32_t i = (uint32_t)__builtin_ctz(cm);
        cm &= (uint16_t)(cm - 1u);
        const GltAttrState* a = &g_glt_log_attrs[i];
        glt_bind_hw_array_buf(a->buf);
        const void* p = (const void*)(uintptr_t)a->ptr;
        if (a->is_arb && pfn_glVertexAttribPointerARB)
            pfn_glVertexAttribPointerARB(i, a->size, a->type, a->norm, a->stride, p);
        else if (pfn_glVertexAttribPointer)
            pfn_glVertexAttribPointer(i, a->size, a->type, a->norm, a->stride, p);
        if (!(e->hw_vaa_mask & (1u << i))) {
            if (pfn_glEnableVertexAttribArrayARB) pfn_glEnableVertexAttribArrayARB(i);
            else if (pfn_glEnableVertexAttribArray) pfn_glEnableVertexAttribArray(i);
        }
        e->attrs[i].ptr = (uint32_t)a->ptr;
        e->attrs[i].buf = a->buf;
        e->attrs[i].stride = a->stride;
        e->attrs[i].type = a->type;
        e->attrs[i].size = (int16_t)a->size;
        e->attrs[i].norm = a->norm;
    }
    e->elem_buf = elem;
    e->vaa_mask = mask;
    e->hw_vaa_mask = mask;
    e->last_frame = g_glt_vao_frame;
    e->hash = h;
    e->active = 1;
    g_glt_vao_active_count++;

    if (g_glt_vao_hash[slot].state == 2) g_glt_vao_tombstones--;
    g_glt_vao_hash[slot].hash = h;
    g_glt_vao_hash[slot].pool_idx = pidx;
    g_glt_vao_hash[slot].state = 1;
    g_glt_vao_dirty = false;
}

static void glt_exec_cmd(const GlCmdHdr* hdr) {
    switch (hdr->op) {
        case GLCMD_WRAP:
            break;
        case GLCMD_SYNC_CALL: {
            const GlCmdSyncCall* c = (const GlCmdSyncCall*)hdr;
            if (g_glt_vao_supported) glt_flush_all_to_vao0(true);
            c->fn(c->ctx);
            if (g_glt_vao_supported) {
                g_glt_hw_array_buf_known = false;
                g_glt_vao0_elem_known = false;
                g_glt_vao_dirty = true;
            }
            atomic_store_explicit(&g_glq_sync_done, 1, memory_order_release);
            pthread_mutex_lock(&g_glt_sync_mtx);
            pthread_cond_broadcast(&g_glt_sync_cv);
            pthread_mutex_unlock(&g_glt_sync_mtx);
            break;
        }
        case GLCMD_ACTIVE_TEXTURE: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            if (pfn_glActiveTexture) pfn_glActiveTexture(c->a);
            break;
        }
        case GLCMD_ALPHA_FUNC: {
            const GlCmd1U1F* c = (const GlCmd1U1F*)hdr;
            pfn_glAlphaFunc(c->a, c->f0);
            break;
        }
        case GLCMD_BEGIN_QUERY: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (pfn_glBeginQueryARB) pfn_glBeginQueryARB(c->a, c->b);
            break;
        }
        case GLCMD_END_QUERY: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            if (pfn_glEndQueryARB) pfn_glEndQueryARB(c->a);
            break;
        }
        case GLCMD_BIND_BUFFER: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (g_glt_vao_supported) {
                if (c->a == GL_ARRAY_BUFFER) {
                    g_glt_log_array_buf = c->b;
                    break;
                }
                if (c->a == GL_ELEMENT_ARRAY_BUFFER) {
                    if (g_glt_log_elem_buf != c->b) {
                        g_glt_log_elem_buf = c->b;
                        g_glt_vao_dirty = true;
                    }
                    break;
                }
            }
            if (pfn_glBindBufferARB) pfn_glBindBufferARB(c->a, c->b);
            break;
        }
        case GLCMD_BIND_FRAMEBUFFER: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (pfn_glBindFramebufferEXT) pfn_glBindFramebufferEXT(c->a, c->b);
            break;
        }
        case GLCMD_BIND_PROGRAM_ARB: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (pfn_glBindProgramARB) pfn_glBindProgramARB(c->a, c->b);
            break;
        }
        case GLCMD_BIND_RENDERBUFFER: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (pfn_glBindRenderbufferEXT) pfn_glBindRenderbufferEXT(c->a, c->b);
            break;
        }
        case GLCMD_BIND_SAMPLER: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (pfn_glBindSampler) pfn_glBindSampler(c->a, c->b);
            break;
        }
        case GLCMD_BIND_TEXTURE: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            pfn_glBindTexture(c->a, c->b);
            break;
        }
        case GLCMD_BLEND_EQUATION: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            if (pfn_glBlendEquation) pfn_glBlendEquation(c->a);
            break;
        }
        case GLCMD_BLEND_FUNC: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            pfn_glBlendFunc(c->a, c->b);
            break;
        }
        case GLCMD_BLIT_FRAMEBUFFER: {
            const GlCmd10I* c = (const GlCmd10I*)hdr;
            if (pfn_glBlitFramebufferEXT)
                pfn_glBlitFramebufferEXT(c->v[0], c->v[1], c->v[2], c->v[3],
                                         c->v[4], c->v[5], c->v[6], c->v[7],
                                         (GLbitfield)c->v[8], (GLenum)c->v[9]);
            break;
        }
        case GLCMD_BUFFER_DATA: {
            const GlCmdBufData* c = (const GlCmdBufData*)hdr;
            const void* data = c->has_data ? (const void*)(c + 1) : NULL;
            if (g_glt_vao_supported) {
                if (c->target == GL_ARRAY_BUFFER) glt_bind_hw_array_buf(g_glt_log_array_buf);
                else if (c->target == GL_ELEMENT_ARRAY_BUFFER) { glt_bind_vao0(); glt_bind_vao0_elem_buf(g_glt_log_elem_buf); }
            }
            if (pfn_glBufferDataARB) pfn_glBufferDataARB(c->target, (GLsizeiptr)c->data_size, data, c->usage);
            break;
        }
        case GLCMD_BUFFER_SUB_DATA: {
            const GlCmdBufSubData* c = (const GlCmdBufSubData*)hdr;
            if (g_glt_vao_supported) {
                if (c->target == GL_ARRAY_BUFFER) glt_bind_hw_array_buf(g_glt_log_array_buf);
                else if (c->target == GL_ELEMENT_ARRAY_BUFFER) { glt_bind_vao0(); glt_bind_vao0_elem_buf(g_glt_log_elem_buf); }
            }
            if (pfn_glBufferSubDataARB) pfn_glBufferSubDataARB(c->target, (GLintptr)c->offset, (GLsizeiptr)c->data_size, (const void*)(c + 1));
            break;
        }
        case GLCMD_BUFFER_PARAM_APPLE: {
            const GlCmd3U* c = (const GlCmd3U*)hdr;
            if (g_glt_vao_supported) {
                if (c->a == GL_ARRAY_BUFFER) glt_bind_hw_array_buf(g_glt_log_array_buf);
                else if (c->a == GL_ELEMENT_ARRAY_BUFFER) { glt_bind_vao0(); glt_bind_vao0_elem_buf(g_glt_log_elem_buf); }
            }
            if (pfn_glBufferParameteriAPPLE) pfn_glBufferParameteriAPPLE(c->a, c->b, (GLint)c->c);
            break;
        }
        case GLCMD_CLEAR: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glClear(c->a);
            break;
        }
        case GLCMD_CLEAR_COLOR: {
            const GlCmd4F* c = (const GlCmd4F*)hdr;
            pfn_glClearColor(c->f[0], c->f[1], c->f[2], c->f[3]);
            break;
        }
        case GLCMD_CLEAR_DEPTH: {
            const GlCmd1D* c = (const GlCmd1D*)hdr;
            pfn_glClearDepth(c->d0);
            break;
        }
        case GLCMD_CLEAR_STENCIL: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glClearStencil((GLint)c->a);
            break;
        }
        case GLCMD_COLOR_MASK: {
            const GlCmd4U* c = (const GlCmd4U*)hdr;
            pfn_glColorMask((GLboolean)c->a, (GLboolean)c->b, (GLboolean)c->c, (GLboolean)c->d);
            break;
        }
        case GLCMD_CULL_FACE: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glCullFace(c->a);
            break;
        }
        case GLCMD_DEPTH_FUNC: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glDepthFunc(c->a);
            break;
        }
        case GLCMD_DEPTH_MASK: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glDepthMask((GLboolean)c->a);
            break;
        }
        case GLCMD_DEPTH_RANGE: {
            const GlCmd2D* c = (const GlCmd2D*)hdr;
            pfn_glDepthRange(c->d0, c->d1);
            break;
        }
        case GLCMD_DISABLE: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glDisable(c->a);
            break;
        }
        case GLCMD_ENABLE: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glEnable(c->a);
            break;
        }
        case GLCMD_DISABLE_VAA: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (g_glt_vao_supported && c->a < GLT_MAX_VAA) {
                uint16_t bit = (uint16_t)(1u << c->a);
                if (g_glt_log_vaa_mask & bit) {
                    g_glt_log_vaa_mask &= (uint16_t)~bit;
                    g_glt_vao_dirty = true;
                }
                break;
            }
            if (c->b) { if (pfn_glDisableVertexAttribArrayARB) pfn_glDisableVertexAttribArrayARB(c->a); }
            else      { if (pfn_glDisableVertexAttribArray) pfn_glDisableVertexAttribArray(c->a); }
            break;
        }
        case GLCMD_ENABLE_VAA: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (g_glt_vao_supported && c->a < GLT_MAX_VAA) {
                uint16_t bit = (uint16_t)(1u << c->a);
                if (!(g_glt_log_vaa_mask & bit)) {
                    g_glt_log_vaa_mask |= bit;
                    g_glt_vao_dirty = true;
                }
                break;
            }
            if (c->b) { if (pfn_glEnableVertexAttribArrayARB) pfn_glEnableVertexAttribArrayARB(c->a); }
            else      { if (pfn_glEnableVertexAttribArray) pfn_glEnableVertexAttribArray(c->a); }
            break;
        }
        case GLCMD_DRAW_ARRAYS: {
            const GlCmd3U* c = (const GlCmd3U*)hdr;
            if (g_glt_vao_supported) glt_vao_prepare_draw(false);
            pfn_glDrawArrays(c->a, (GLint)c->b, (GLsizei)c->c);
            break;
        }
        case GLCMD_DRAW_ARRAYS_CLIENT: {
            const GlCmdDrawArraysClient* c = (const GlCmdDrawArraysClient*)hdr;
            const GlCmdClientAttrDesc* descs = (const GlCmdClientAttrDesc*)(c + 1);
            const uint8_t* base = (const uint8_t*)c;
            if (g_glt_vao_supported) {
                glt_bind_vao0();
                glt_sync_vao0_vaa_mask(g_glt_log_vaa_mask);
                uint16_t client_bits = 0;
                for (uint32_t i = 0; i < c->num_attrs; i++) {
                    if (descs[i].index < GLT_MAX_VAA) client_bits |= (uint16_t)(1u << descs[i].index);
                }
                uint16_t vbo_bits = (uint16_t)(g_glt_log_vaa_mask & ~client_bits);
                while (vbo_bits) {
                    uint32_t i = (uint32_t)__builtin_ctz(vbo_bits);
                    vbo_bits &= (uint16_t)(vbo_bits - 1u);
                    const GltAttrState* a = &g_glt_log_attrs[i];
                    if (a->valid) {
                        glt_bind_hw_array_buf(a->buf);
                        const void* p = (const void*)(uintptr_t)a->ptr;
                        if (a->is_arb && pfn_glVertexAttribPointerARB)
                            pfn_glVertexAttribPointerARB(i, a->size, a->type, a->norm, a->stride, p);
                        else if (pfn_glVertexAttribPointer)
                            pfn_glVertexAttribPointer(i, a->size, a->type, a->norm, a->stride, p);
                    }
                }
                glt_bind_hw_array_buf(0);
            }
            for (uint32_t i = 0; i < c->num_attrs; i++) {
                const GlCmdClientAttrDesc* d = &descs[i];
                const void* ptr = (const void*)(base + d->data_off);
                if (d->is_arb) {
                    if (pfn_glVertexAttribPointerARB)
                        pfn_glVertexAttribPointerARB(d->index, d->size, d->type, (GLboolean)d->normalized, d->stride, ptr);
                } else {
                    if (pfn_glVertexAttribPointer)
                        pfn_glVertexAttribPointer(d->index, d->size, d->type, (GLboolean)d->normalized, d->stride, ptr);
                }
            }
            pfn_glDrawArrays(c->mode, 0, (GLsizei)c->count);
            break;
        }
        case GLCMD_DRAW_BUFFER: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glDrawBuffer(c->a);
            break;
        }
        case GLCMD_DRAW_RANGE_ELEMENTS: {
            const GlCmd6U* c = (const GlCmd6U*)hdr;
            if (g_glt_vao_supported) glt_vao_prepare_draw(true);
            if (pfn_glDrawRangeElements)
                pfn_glDrawRangeElements(c->a, c->b, c->c, (GLsizei)c->d, c->e, (const void*)(uintptr_t)c->f);
            break;
        }
        case GLCMD_FOGFV: {
            const GlCmd1U4F* c = (const GlCmd1U4F*)hdr;
            pfn_glFogfv(c->a, c->f);
            break;
        }
        case GLCMD_FB_RENDERBUFFER: {
            const GlCmd4U* c = (const GlCmd4U*)hdr;
            if (pfn_glFramebufferRenderbufferEXT) pfn_glFramebufferRenderbufferEXT(c->a, c->b, c->c, c->d);
            break;
        }
        case GLCMD_FB_TEXTURE2D: {
            const GlCmd5U* c = (const GlCmd5U*)hdr;
            if (pfn_glFramebufferTexture2DEXT) pfn_glFramebufferTexture2DEXT(c->a, c->b, c->c, c->d, (GLint)c->e);
            break;
        }
        case GLCMD_GENERATE_MIPMAP: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            if (pfn_glGenerateMipmapEXT) pfn_glGenerateMipmapEXT(c->a);
            break;
        }
        case GLCMD_PIXEL_STOREI: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            pfn_glPixelStorei(c->a, (GLint)c->b);
            break;
        }
        case GLCMD_POINT_PARAM_F: {
            const GlCmd1U1F* c = (const GlCmd1U1F*)hdr;
            if (pfn_glPointParameterfARB) pfn_glPointParameterfARB(c->a, c->f0);
            break;
        }
        case GLCMD_POINT_PARAM_FV: {
            const GlCmd1U4F* c = (const GlCmd1U4F*)hdr;
            if (pfn_glPointParameterfvARB) pfn_glPointParameterfvARB(c->a, c->f);
            break;
        }
        case GLCMD_POINT_SIZE: {
            const GlCmd1F* c = (const GlCmd1F*)hdr;
            pfn_glPointSize(c->f0);
            break;
        }
        case GLCMD_POLYGON_MODE: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            pfn_glPolygonMode(c->a, c->b);
            break;
        }
        case GLCMD_POLYGON_OFFSET: {
            const GlCmd2F* c = (const GlCmd2F*)hdr;
            pfn_glPolygonOffset(c->f0, c->f1);
            break;
        }
        case GLCMD_PROGRAM_LOCAL_4FV: {
            const GlCmd2U4F* c = (const GlCmd2U4F*)hdr;
            if (pfn_glProgramLocalParameter4fvARB) pfn_glProgramLocalParameter4fvARB(c->a, c->b, c->f);
            break;
        }
        case GLCMD_PROGRAM_LOCAL_I4IV: {
            const GlCmd2U4I* c = (const GlCmd2U4I*)hdr;
            if (pfn_glProgramLocalParameterI4ivNV) pfn_glProgramLocalParameterI4ivNV(c->a, c->b, c->v);
            break;
        }
        case GLCMD_SAMPLER_PARAM_F: {
            const GlCmd2U1F* c = (const GlCmd2U1F*)hdr;
            if (pfn_glSamplerParameterf) pfn_glSamplerParameterf(c->a, c->b, c->f0);
            break;
        }
        case GLCMD_SAMPLER_PARAM_I: {
            const GlCmd3U* c = (const GlCmd3U*)hdr;
            if (pfn_glSamplerParameteri) pfn_glSamplerParameteri(c->a, c->b, (GLint)c->c);
            break;
        }
        case GLCMD_SAMPLER_PARAM_FV: {
            const GlCmd2U4F* c = (const GlCmd2U4F*)hdr;
            if (pfn_glSamplerParameterfv) pfn_glSamplerParameterfv(c->a, c->b, c->f);
            break;
        }
        case GLCMD_SCISSOR: {
            const GlCmd4U* c = (const GlCmd4U*)hdr;
            pfn_glScissor((GLint)c->a, (GLint)c->b, (GLsizei)c->c, (GLsizei)c->d);
            break;
        }
        case GLCMD_SHADE_MODEL: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glShadeModel(c->a);
            break;
        }
        case GLCMD_STENCIL_FUNC: {
            const GlCmd3U* c = (const GlCmd3U*)hdr;
            pfn_glStencilFunc(c->a, (GLint)c->b, c->c);
            break;
        }
        case GLCMD_STENCIL_MASK: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            pfn_glStencilMask(c->a);
            break;
        }
        case GLCMD_STENCIL_OP: {
            const GlCmd3U* c = (const GlCmd3U*)hdr;
            pfn_glStencilOp(c->a, c->b, c->c);
            break;
        }
        case GLCMD_TEX_ENVF: {
            const GlCmd2U1F* c = (const GlCmd2U1F*)hdr;
            pfn_glTexEnvf(c->a, c->b, c->f0);
            break;
        }
        case GLCMD_TEX_PARAM_F: {
            const GlCmd2U1F* c = (const GlCmd2U1F*)hdr;
            pfn_glTexParameterf(c->a, c->b, c->f0);
            break;
        }
        case GLCMD_TEX_PARAM_I: {
            const GlCmd3U* c = (const GlCmd3U*)hdr;
            pfn_glTexParameteri(c->a, c->b, (GLint)c->c);
            break;
        }
        case GLCMD_TEX_PARAM_FV: {
            const GlCmd2U4F* c = (const GlCmd2U4F*)hdr;
            pfn_glTexParameterfv(c->a, c->b, c->f);
            break;
        }
        case GLCMD_UNIFORM_1I: {
            const GlCmd3U* c = (const GlCmd3U*)hdr;
            if (c->c) { if (pfn_glUniform1iARB) pfn_glUniform1iARB((GLint)c->a, (GLint)c->b); }
            else      { if (pfn_glUniform1i) pfn_glUniform1i((GLint)c->a, (GLint)c->b); }
            break;
        }
        case GLCMD_UNIFORM_1IV: {
            const GlCmdUniformV* c = (const GlCmdUniformV*)hdr;
            const GLint* v = (const GLint*)(c + 1);
            if (c->is_arb) { if (pfn_glUniform1ivARB) pfn_glUniform1ivARB(c->loc, c->count, v); }
            else           { if (pfn_glUniform1iv) pfn_glUniform1iv(c->loc, c->count, v); }
            break;
        }
        case GLCMD_UNIFORM_4FV: {
            const GlCmdUniformV* c = (const GlCmdUniformV*)hdr;
            const GLfloat* v = (const GLfloat*)(c + 1);
            if (c->is_arb) { if (pfn_glUniform4fvARB) pfn_glUniform4fvARB(c->loc, c->count, v); }
            else           { if (pfn_glUniform4fv) pfn_glUniform4fv(c->loc, c->count, v); }
            break;
        }
        case GLCMD_UNIFORM_4IV: {
            const GlCmdUniformV* c = (const GlCmdUniformV*)hdr;
            const GLint* v = (const GLint*)(c + 1);
            if (c->is_arb) { if (pfn_glUniform4ivARB) pfn_glUniform4ivARB(c->loc, c->count, v); }
            else           { if (pfn_glUniform4iv) pfn_glUniform4iv(c->loc, c->count, v); }
            break;
        }
        case GLCMD_USE_PROGRAM: {
            const GlCmd2U* c = (const GlCmd2U*)hdr;
            if (c->b) { if (pfn_glUseProgramObjectARB) pfn_glUseProgramObjectARB((GLhandleARB)c->a); }
            else      { if (pfn_glUseProgram) pfn_glUseProgram(c->a); }
            break;
        }
        case GLCMD_VERTEX_ATTRIB_PTR: {
            const GlCmdVAP* c = (const GlCmdVAP*)hdr;
            if (g_glt_vao_supported && c->index < GLT_MAX_VAA) {
                GltAttrState* a = &g_glt_log_attrs[c->index];
                uint8_t new_valid = (g_glt_log_array_buf != 0) ? 1u : 2u;
                if (a->ptr != c->ptr || a->buf != g_glt_log_array_buf ||
                    a->stride != c->stride || a->type != c->type ||
                    a->size != c->size || a->norm != (uint8_t)c->normalized ||
                    a->valid != new_valid) {
                    a->ptr = c->ptr;
                    a->buf = g_glt_log_array_buf;
                    a->stride = c->stride;
                    a->type = c->type;
                    a->size = c->size;
                    a->norm = (uint8_t)c->normalized;
                    a->is_arb = (uint8_t)c->is_arb;
                    a->valid = new_valid;
                    g_glt_vao_dirty = true;
                }
                break;
            }
            const void* ptr = (const void*)(uintptr_t)c->ptr;
            if (c->is_arb) {
                if (pfn_glVertexAttribPointerARB)
                    pfn_glVertexAttribPointerARB(c->index, c->size, c->type, (GLboolean)c->normalized, c->stride, ptr);
            } else {
                if (pfn_glVertexAttribPointer)
                    pfn_glVertexAttribPointer(c->index, c->size, c->type, (GLboolean)c->normalized, c->stride, ptr);
            }
            break;
        }
        case GLCMD_VIEWPORT: {
            const GlCmd4U* c = (const GlCmd4U*)hdr;
            pfn_glViewport((GLint)c->a, (GLint)c->b, (GLsizei)c->c, (GLsizei)c->d);
            break;
        }
        case GLCMD_SWAP: {
            const GlCmdSwap* c = (const GlCmdSwap*)hdr;
            if (g_glt_vao_supported) {
                glt_bind_vao0();
            }
            uint64_t t0 = gl_prof_tick();
            recomp_postfx_on_swap(c->w, c->h_px);
            uint64_t t1 = gl_prof_tick();
#ifdef __SWITCH__
            /* Re-assert swap_interval on nwindow (1 memory store, 0 IPC) so Horizon OS
             * sleep/resume or dock transitions can never silently re-enable VSync. */
            nwindowSetSwapInterval(nwindowGetDefault(),
                                   (g_glt_requested_swap_interval <= 1) ? 0u : (u32)g_glt_requested_swap_interval);
            /* Pace 60 FPS cap in software when swap_interval=0 so menus stay at 60 FPS
             * while <60 FPS gameplay never stalls 8-12ms on VSync dequeueBuffer in glClear. */
            if (g_glt_requested_swap_interval <= 1 && g_glt_prev_swap_tick != 0) {
                const uint64_t period_ticks = 320000ULL; /* 19.2 MHz / 60 Hz */
                uint64_t elapsed = t1 - g_glt_prev_swap_tick;
                if (elapsed < period_ticks) {
                    uint64_t rem_ns = ((period_ticks - elapsed) * 15625ULL) / 300ULL;
                    svcSleepThread((s64)rem_ns);
                }
            }
#endif
            if (g_glt_window) SDL_GL_SwapWindow(g_glt_window);
            uint64_t t2 = gl_prof_tick();
#ifdef __SWITCH__
            g_glt_prev_swap_tick = t2;
#endif
            g_glt_batch_pfx_ticks += (t1 - t0);
            g_glt_batch_swap_ticks += (t2 - t1);
            if (g_glt_vao_supported) {
                g_glt_hw_array_buf_known = false;
                g_glt_vao0_elem_known = false;
                g_glt_vao_frame++;
                if (g_glt_vao_frame == 0) g_glt_vao_frame = 1;
                if (g_glt_local_vao_hits) {
                    atomic_fetch_add_explicit(&g_glq_vao_hits, g_glt_local_vao_hits, memory_order_relaxed);
                    g_glt_local_vao_hits = 0;
                }
                if (g_glt_local_vao_misses) {
                    atomic_fetch_add_explicit(&g_glq_vao_misses, g_glt_local_vao_misses, memory_order_relaxed);
                    g_glt_local_vao_misses = 0;
                }
            }
            atomic_store_explicit(&g_glq_last_pfx_ticks, t1 - t0, memory_order_relaxed);
            atomic_store_explicit(&g_glq_last_swap_ticks, t2 - t1, memory_order_relaxed);
            atomic_store_explicit(&g_glq_completed_frame, c->frame_id, memory_order_release);
            pthread_mutex_lock(&g_glt_sync_mtx);
            pthread_cond_broadcast(&g_glt_sync_cv);
            pthread_mutex_unlock(&g_glt_sync_mtx);
            break;
        }
        case GLCMD_SET_SWAP_INTERVAL: {
            const GlCmd1U* c = (const GlCmd1U*)hdr;
            int iv = (int)c->a;
#ifdef __SWITCH__
            g_glt_requested_swap_interval = iv;
            /* Mesa EGL clamps eglSwapInterval(0) up to config->MinSwapInterval=1;
             * call libnx nwindowSetSwapInterval(nwindowGetDefault(), 0) directly! */
            int actual = (iv <= 1) ? 0 : iv;
            SDL_GL_SetSwapInterval(actual);
            Result rc = nwindowSetSwapInterval(nwindowGetDefault(), (u32)actual);
            fprintf(stderr, "[glt] nwindowSetSwapInterval(%d) rc=0x%x (req=%d)\n", actual, (unsigned)rc, iv);
#else
            SDL_GL_SetSwapInterval(iv);
#endif
            break;
        }
        case GLCMD_RESIZE: {
            const GlCmdResize* c = (const GlCmdResize*)hdr;
            if (g_glt_window) {
                SDL_SetWindowSize(g_glt_window, c->win_w, c->win_h);
#ifdef __SWITCH__
                g_glt_requested_swap_interval = c->swap_interval;
                int actual = (c->swap_interval <= 1) ? 0 : c->swap_interval;
                SDL_GL_SetSwapInterval(actual);
                nwindowSetSwapInterval(nwindowGetDefault(), (u32)actual);
#else
                SDL_GL_SetSwapInterval(c->swap_interval);
#endif
            }
            recomp_gl_set_backbuffer_scale(c->guest_w, c->guest_h, c->win_w, c->win_h);
            atomic_store_explicit(&g_glq_sync_done, 1, memory_order_release);
            pthread_mutex_lock(&g_glt_sync_mtx);
            pthread_cond_broadcast(&g_glt_sync_cv);
            pthread_mutex_unlock(&g_glt_sync_mtx);
            break;
        }
        default:
            break;
    }
}

static void* glt_worker_main(void* arg) {
    (void)arg;
#ifdef __SWITCH__
    /* Pin dedicated GL worker thread to Core 1 (zero contention with guest Core 0) */
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 0x2);
#endif
    SDL_GL_MakeCurrent(g_glt_window, g_glt_ctx);
#ifdef __SWITCH__
    {
        const RecompSettings* st = recomp_settings_get();
        int iv = (st && st->fps_cap <= 30) ? 2 : 1;
        int actual = (iv <= 1) ? 0 : iv;
        g_glt_requested_swap_interval = iv;
        g_glt_prev_swap_tick = 0;
        SDL_GL_SetSwapInterval(actual);
        Result rc = nwindowSetSwapInterval(nwindowGetDefault(), (u32)actual);
        fprintf(stderr, "[glt] Switch nwindowSetSwapInterval(%d) rc=0x%x (req=%d, 60fps software pacer active)\n",
                actual, (unsigned)rc, iv);
    }
#endif

    /* Probe GL_ARB_vertex_array_object support on the active GL context */
    g_glt_vao_supported = false;
    g_glt_vao_pool_count = 0;
    g_glt_vao_active_count = 0;
    g_glt_vao_tombstones = 0;
    g_glt_vao_evict_cursor = 0;
    g_glt_vao_frame = 1;
    g_glt_hw_vao = 0;
    g_glt_hw_vao_elem = 0;
    g_glt_hw_array_buf_known = false;
    g_glt_vao0_elem_known = false;
    g_glt_vao0_vaa_known = false;
    g_glt_vao_dirty = true;
    g_glt_local_vao_hits = 0;
    g_glt_local_vao_misses = 0;
    memset(g_glt_vao_hash, 0, sizeof(g_glt_vao_hash));
    memset(g_glt_log_attrs, 0, sizeof(g_glt_log_attrs));
    if (pfn_glGenVertexArrays && pfn_glBindVertexArray && pfn_glDeleteVertexArrays) {
        if (pfn_glGetError) (void)pfn_glGetError();
        GLuint test_vao = 0;
        pfn_glGenVertexArrays(1, &test_vao);
        GLenum err = pfn_glGetError ? pfn_glGetError() : GL_NO_ERROR;
        if (err == GL_NO_ERROR && test_vao != 0) {
            pfn_glBindVertexArray(test_vao);
            err = pfn_glGetError ? pfn_glGetError() : GL_NO_ERROR;
            pfn_glBindVertexArray(0);
            pfn_glDeleteVertexArrays(1, &test_vao);
            if (err == GL_NO_ERROR) {
                g_glt_vao_supported = (GLT_ENABLE_VAO_CACHE != 0);
            }
        }
    }
    fprintf(stderr, "[glt] GL_ARB_vertex_array_object support: %s (vao_cache=%d gen=%p bind=%p del=%p)\n",
            (pfn_glGenVertexArrays && pfn_glBindVertexArray) ? "YES" : "NO",
            g_glt_vao_supported ? 1 : 0,
            (void*)pfn_glGenVertexArrays, (void*)pfn_glBindVertexArray, (void*)pfn_glDeleteVertexArrays);
    fflush(stderr);

    pthread_mutex_lock(&g_glt_sync_mtx);
    g_glt_ready = true;
    pthread_cond_broadcast(&g_glt_sync_cv);
    pthread_mutex_unlock(&g_glt_sync_mtx);

    uint32_t r = 0;
    uint32_t published_r = 0;
    uint32_t local_op_counts[GLCMD_COUNT] = {0};
#ifdef RECOMP_GL_PROF
    uint64_t local_op_ticks[GLCMD_COUNT] = {0};
    uint32_t sample_frame_idx = 0;
    bool sample_this_frame = false;
#endif
    for (;;) {
        uint32_t w = atomic_load_explicit(&g_glq_write, memory_order_acquire);
        if (r == w) {
            int spin = 0;
            while (r == w && spin < 2000) {
                glq_cpu_yield();
                w = atomic_load_explicit(&g_glq_write, memory_order_acquire);
                spin++;
            }
            if (r == w) {
                pthread_mutex_lock(&g_glt_wake_mtx);
                atomic_store_explicit(&g_glq_sleeping, 1, memory_order_release);
                w = atomic_load_explicit(&g_glq_write, memory_order_acquire);
                while (r == w) {
                    pthread_cond_wait(&g_glt_wake_cv, &g_glt_wake_mtx);
                    w = atomic_load_explicit(&g_glq_write, memory_order_acquire);
                }
                atomic_store_explicit(&g_glq_sleeping, 0, memory_order_relaxed);
                pthread_mutex_unlock(&g_glt_wake_mtx);
            }
        }

        g_glt_batch_pfx_ticks = 0;
        g_glt_batch_swap_ticks = 0;
        uint64_t wt0 = gl_prof_tick();
        while (r != w) {
            const GlCmdHdr* hdr = (const GlCmdHdr*)(g_glq_buf + (r & GLQ_MASK));
            uint32_t sz = hdr->size;
            uint16_t op = hdr->op;
            if (op == GLCMD_SHUTDOWN) {
                if (g_glt_vao_supported) {
                    glt_bind_vao0();
                    for (uint32_t vi = 0; vi < g_glt_vao_pool_count; vi++) {
                        if (g_glt_vao_pool[vi].vao) {
                            pfn_glDeleteVertexArrays(1, &g_glt_vao_pool[vi].vao);
                            g_glt_vao_pool[vi].vao = 0;
                        }
                    }
                    g_glt_vao_pool_count = 0;
                    g_glt_vao_active_count = 0;
                }
                r += sz;
                atomic_store_explicit(&g_glq_read, r, memory_order_release);
                SDL_GL_MakeCurrent(g_glt_window, NULL);
                return NULL;
            }
            if (op < GLCMD_COUNT) local_op_counts[op]++;
#ifdef RECOMP_GL_PROF
            if (__builtin_expect(sample_this_frame, 0)) {
                uint64_t ot0 = gl_prof_tick();
                glt_exec_cmd(hdr);
                if (op < GLCMD_COUNT) local_op_ticks[op] += gl_prof_tick() - ot0;
            } else {
                glt_exec_cmd(hdr);
            }
#else
            glt_exec_cmd(hdr);
#endif
            r += sz;
            if (op == GLCMD_SWAP) {
                for (int oi = 0; oi < GLCMD_COUNT; oi++) {
                    uint32_t cnt = local_op_counts[oi];
                    if (cnt) {
                        atomic_fetch_add_explicit(&g_glq_op_counts[oi], cnt, memory_order_relaxed);
                        local_op_counts[oi] = 0;
                    }
                }
#ifdef RECOMP_GL_PROF
                if (sample_this_frame) {
                    for (int oi = 0; oi < GLCMD_COUNT; oi++) {
                        uint64_t tk = local_op_ticks[oi];
                        if (tk) {
                            atomic_fetch_add_explicit(&g_glq_op_ticks[oi], tk, memory_order_relaxed);
                            local_op_ticks[oi] = 0;
                        }
                    }
                    atomic_fetch_add_explicit(&g_glq_op_sample_frames, 1u, memory_order_relaxed);
                }
                sample_frame_idx++;
                sample_this_frame = ((sample_frame_idx % 7u) == 0u);
#endif
                atomic_store_explicit(&g_glq_read, r, memory_order_release);
                published_r = r;
            } else if (op == GLCMD_SYNC_CALL || op == GLCMD_RESIZE || (r - published_r) >= 4096u) {
                atomic_store_explicit(&g_glq_read, r, memory_order_release);
                published_r = r;
            }
        }
        if (r != published_r) {
            atomic_store_explicit(&g_glq_read, r, memory_order_release);
            published_r = r;
        }
        uint64_t batch_total = gl_prof_tick() - wt0;
        uint64_t batch_non_cmd = g_glt_batch_pfx_ticks + g_glt_batch_swap_ticks;
        uint64_t batch_cmd = (batch_total > batch_non_cmd) ? (batch_total - batch_non_cmd) : 0;
        atomic_fetch_add_explicit(&g_glq_worker_ticks, batch_total, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_glq_worker_cmd_ticks, batch_cmd, memory_order_relaxed);
        if (g_glt_batch_pfx_ticks) {
            atomic_fetch_add_explicit(&g_glq_worker_pfx_ticks, g_glt_batch_pfx_ticks, memory_order_relaxed);
        }
        if (g_glt_batch_swap_ticks) {
            atomic_fetch_add_explicit(&g_glq_worker_swap_ticks, g_glt_batch_swap_ticks, memory_order_relaxed);
        }
    }
    return NULL;
}

/* ---------------- Public Lifecycle API (called by hle_sdl12.c) ---------------- */

bool recomp_gl_thread_is_active(void) {
    return g_glt_active;
}

void recomp_gl_thread_start(SDL_Window* win, SDL_GLContext gl_ctx) {
    if (g_glt_active || !win || !gl_ctx) return;
#ifndef __SWITCH__
    if (g_null_gl_start > 0) return; /* keep host NULL_GL profiling single-threaded */
#endif
    ensure_gl_loaded();
    (void)query_hw_max_aniso();
    /* Release GL context from main thread before handing off to Core 1 worker */
    if (SDL_GL_MakeCurrent(win, NULL) != 0) {
        fprintf(stderr, "[glt] SDL_GL_MakeCurrent(NULL) failed: %s (staying single-threaded)\n", SDL_GetError());
        SDL_GL_MakeCurrent(win, gl_ctx);
        return;
    }

    g_glt_window = win;
    g_glt_ctx = gl_ctx;
    g_glt_ready = false;
    g_glq_local_write = 0;
    g_glq_cached_read = 0;
    g_glq_unflushed_bytes = 0;
    g_glq_submitted_frame = 0;
    g_glq_array_binding_known = false;
    g_glq_array_unbind_pending = false;
    g_glq_elem_binding_known = false;
    g_glq_elem_unbind_pending = false;
    g_glt_log_array_buf = g_bound_array_buf;
    g_glt_log_elem_buf = g_bound_elem_buf;
    g_glt_log_vaa_mask = (uint16_t)(g_cache_vaa_enabled & 0xFFFFu);
    /* Force producer to re-emit initial vertex array/buffer bindings into worker */
    g_array_buf_valid = false;
    g_elem_buf_valid = false;
    g_pbo_unpack_valid = false;
    g_pbo_pack_valid = false;
    g_cache_vaa_known = 0;
    for (int i = 0; i < GL_CACHE_MAX_VAA; i++) g_cache_vap[i].valid = false;
    atomic_store(&g_glq_write, 0);
    atomic_store(&g_glq_read, 0);
    atomic_store(&g_glq_sleeping, 0);
    atomic_store(&g_glq_completed_frame, 0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2u * 1024u * 1024u); /* 2 MB stack for Mesa shader compiler */
    int rc = pthread_create(&g_glt_thread, &attr, glt_worker_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "[glt] pthread_create failed (%d), staying single-threaded\n", rc);
        SDL_GL_MakeCurrent(win, gl_ctx);
        return;
    }

    pthread_mutex_lock(&g_glt_sync_mtx);
    while (!g_glt_ready) {
        pthread_cond_wait(&g_glt_sync_cv, &g_glt_sync_mtx);
    }
    pthread_mutex_unlock(&g_glt_sync_mtx);

#ifdef __SWITCH__
    /* Pin main guest CPU thread to Core 0 (zero migration flushes, zero GL contention) */
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 0x1);
#endif
    g_glt_active = true;
    fprintf(stderr, "[glt] Dedicated Async GL Worker Thread started (8 MB lock-free ring, 2 MB stack)\n");
}

void recomp_gl_thread_shutdown(void) {
    if (!g_glt_active) return;
    glq_flush_array_unbind();
    glq_flush_elem_unbind();
    GlCmdHdr* c = (GlCmdHdr*)glq_alloc(sizeof(GlCmdHdr));
    c->op = GLCMD_SHUTDOWN;
    c->flags = 0;
    c->size = (sizeof(GlCmdHdr) + 7u) & ~7u;
    glq_publish(true);
    pthread_join(g_glt_thread, NULL);
    g_glt_active = false;
    if (g_glt_window && g_glt_ctx) {
        SDL_GL_MakeCurrent(g_glt_window, g_glt_ctx);
    }
}

void recomp_gl_thread_swap(int win_w, int win_h, uint64_t* out_pfx_ticks, uint64_t* out_swap_ticks) {
    glq_flush_array_unbind();
    glq_flush_elem_unbind();
    uint64_t fid = ++g_glq_submitted_frame;
    GlCmdSwap* c = (GlCmdSwap*)glq_alloc(sizeof(GlCmdSwap));
    c->h.op = GLCMD_SWAP;
    c->h.flags = 0;
    c->h.size = (sizeof(GlCmdSwap) + 7u) & ~7u;
    c->w = win_w;
    c->h_px = win_h;
    c->frame_id = fid;
    glq_publish(true);

    /* Allow at most 2 frames in flight: wait until frame (fid - 2) has swapped */
    uint64_t wait_for = (fid > 2u) ? (fid - 2u) : 0u;
    if (wait_for > 0u) {
        uint64_t done = atomic_load_explicit(&g_glq_completed_frame, memory_order_acquire);
        if (done < wait_for) {
            uint64_t wait_start = gl_prof_tick();
            for (int spin = 0; spin < 2000 && done < wait_for; spin++) {
                glq_cpu_yield();
                done = atomic_load_explicit(&g_glq_completed_frame, memory_order_acquire);
            }
            if (done < wait_for) {
                pthread_mutex_lock(&g_glt_sync_mtx);
                while (atomic_load_explicit(&g_glq_completed_frame, memory_order_acquire) < wait_for) {
                    pthread_cond_wait(&g_glt_sync_cv, &g_glt_sync_mtx);
                }
                pthread_mutex_unlock(&g_glt_sync_mtx);
            }
            g_glq_swap_wait_ticks += gl_prof_tick() - wait_start;
        }
    }
    if (out_pfx_ticks) *out_pfx_ticks = atomic_load_explicit(&g_glq_last_pfx_ticks, memory_order_relaxed);
    if (out_swap_ticks) *out_swap_ticks = atomic_load_explicit(&g_glq_last_swap_ticks, memory_order_relaxed);
}

void recomp_gl_thread_set_swap_interval(int interval) {
    if (!g_glt_active) {
        SDL_GL_SetSwapInterval(interval);
        return;
    }
    GlCmd1U* c = (GlCmd1U*)glq_alloc(sizeof(GlCmd1U));
    c->h.op = GLCMD_SET_SWAP_INTERVAL;
    c->h.flags = 0;
    c->h.size = (sizeof(GlCmd1U) + 7u) & ~7u;
    c->a = (uint32_t)interval;
    glq_publish(true);
}

void recomp_gl_thread_resize(int win_w, int win_h, int guest_w, int guest_h, int swap_interval) {
    if (!g_glt_active) {
        if (g_glt_window) SDL_SetWindowSize(g_glt_window, win_w, win_h);
        SDL_GL_SetSwapInterval(swap_interval);
        recomp_gl_set_backbuffer_scale(guest_w, guest_h, win_w, win_h);
        return;
    }
    glq_flush_array_unbind();
    glq_flush_elem_unbind();
    atomic_store_explicit(&g_glq_sync_done, 0, memory_order_relaxed);
    GlCmdResize* c = (GlCmdResize*)glq_alloc(sizeof(GlCmdResize));
    c->h.op = GLCMD_RESIZE;
    c->h.flags = 0;
    c->h.size = (sizeof(GlCmdResize) + 7u) & ~7u;
    c->win_w = win_w;
    c->win_h = win_h;
    c->guest_w = guest_w;
    c->guest_h = guest_h;
    c->swap_interval = swap_interval;
    glq_publish(true);

    pthread_mutex_lock(&g_glt_sync_mtx);
    while (!atomic_load_explicit(&g_glq_sync_done, memory_order_acquire)) {
        pthread_cond_wait(&g_glt_sync_cv, &g_glt_sync_mtx);
    }
    pthread_mutex_unlock(&g_glt_sync_mtx);
    g_glq_array_binding_known = false;
}

/* ---------------- Producer Enqueue Helpers ---------------- */

static inline void glq_push_1u(uint16_t op, uint32_t a) {
    GlCmd1U* c = (GlCmd1U*)glq_alloc(sizeof(GlCmd1U));
    c->h.op = op; c->h.flags = 0; c->h.size = (sizeof(GlCmd1U) + 7u) & ~7u;
    c->a = a;
    glq_commit();
}
static inline void glq_push_2u(uint16_t op, uint32_t a, uint32_t b) {
    GlCmd2U* c = (GlCmd2U*)glq_alloc(sizeof(GlCmd2U));
    c->h.op = op; c->h.flags = 0; c->h.size = (sizeof(GlCmd2U) + 7u) & ~7u;
    c->a = a; c->b = b;
    glq_commit();
}
static inline void glq_push_3u(uint16_t op, uint32_t a, uint32_t b, uint32_t c0) {
    GlCmd3U* c = (GlCmd3U*)glq_alloc(sizeof(GlCmd3U));
    c->h.op = op; c->h.flags = 0; c->h.size = (sizeof(GlCmd3U) + 7u) & ~7u;
    c->a = a; c->b = b; c->c = c0;
    glq_commit();
}
static inline void glq_push_4u(uint16_t op, uint32_t a, uint32_t b, uint32_t c0, uint32_t d) {
    GlCmd4U* c = (GlCmd4U*)glq_alloc(sizeof(GlCmd4U));
    c->h.op = op; c->h.flags = 0; c->h.size = (sizeof(GlCmd4U) + 7u) & ~7u;
    c->a = a; c->b = b; c->c = c0; c->d = d;
    glq_commit();
}
static inline void glq_push_5u(uint16_t op, uint32_t a, uint32_t b, uint32_t c0, uint32_t d, uint32_t e) {
    GlCmd5U* c = (GlCmd5U*)glq_alloc(sizeof(GlCmd5U));
    c->h.op = op; c->h.flags = 0; c->h.size = (sizeof(GlCmd5U) + 7u) & ~7u;
    c->a = a; c->b = b; c->c = c0; c->d = d; c->e = e;
    glq_commit();
}
static inline void glq_push_6u(uint16_t op, uint32_t a, uint32_t b, uint32_t c0, uint32_t d, uint32_t e, uint32_t f) {
    GlCmd6U* c = (GlCmd6U*)glq_alloc(sizeof(GlCmd6U));
    c->h.op = op; c->h.flags = 0; c->h.size = (sizeof(GlCmd6U) + 7u) & ~7u;
    c->a = a; c->b = b; c->c = c0; c->d = d; c->e = e; c->f = f;
    glq_commit();
}
static inline void glq_push_2u1f(uint16_t op, uint32_t a, uint32_t b, float f0) {
    GlCmd2U1F* c = (GlCmd2U1F*)glq_alloc(sizeof(GlCmd2U1F));
    c->h.op = op; c->h.flags = 0; c->h.size = (sizeof(GlCmd2U1F) + 7u) & ~7u;
    c->a = a; c->b = b; c->f0 = f0;
    glq_commit();
}
static inline void glq_push_buf_sub_data(GLenum target, int32_t off, uint32_t len, const void* src) {
    if (target == GL_ARRAY_BUFFER) glq_flush_array_unbind();
    else if (target == GL_ELEMENT_ARRAY_BUFFER) glq_flush_elem_unbind();
    const uint8_t* p = (const uint8_t*)src;
    while (len > 0) {
        uint32_t chunk = (len > 1048576u) ? 1048576u : len;
        uint32_t total = (sizeof(GlCmdBufSubData) + chunk + 7u) & ~7u;
        GlCmdBufSubData* c = (GlCmdBufSubData*)glq_alloc(total);
        c->h.op = GLCMD_BUFFER_SUB_DATA;
        c->h.flags = 0;
        c->h.size = total;
        c->target = target;
        c->offset = off;
        c->data_size = chunk;
        memcpy(c + 1, p, chunk);
        glq_commit();
        p += chunk;
        off += (int32_t)chunk;
        len -= chunk;
    }
}

#endif /* RECOMP_GL_THREAD_IMPL_H */

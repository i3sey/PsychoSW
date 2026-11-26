#ifndef RECOMP_CPU_H
#define RECOMP_CPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* g_mem_base;

static inline uint8_t mem_r8_m(const uint8_t* __restrict__ m, uint32_t addr) {
    return m[addr];
}

static inline uint16_t mem_r16_m(const uint8_t* __restrict__ m, uint32_t addr) {
    uint16_t v;
    __builtin_memcpy(&v, m + addr, 2);
    return v;
}

static inline uint32_t mem_r32_m(const uint8_t* __restrict__ m, uint32_t addr) {
    uint32_t v;
    __builtin_memcpy(&v, m + addr, 4);
    return v;
}

static inline uint64_t mem_r64_m(const uint8_t* __restrict__ m, uint32_t addr) {
    uint64_t v;
    __builtin_memcpy(&v, m + addr, 8);
    return v;
}

static inline float mem_rf32_m(const uint8_t* __restrict__ m, uint32_t addr) {
    float v;
    __builtin_memcpy(&v, m + addr, 4);
    return v;
}

static inline double mem_rf64_m(const uint8_t* __restrict__ m, uint32_t addr) {
    double v;
    __builtin_memcpy(&v, m + addr, 8);
    return v;
}

static inline void mem_w8_m(uint8_t* __restrict__ m, uint32_t addr, uint8_t v) {
    m[addr] = v;
}

static inline void mem_w16_m(uint8_t* __restrict__ m, uint32_t addr, uint16_t v) {
    __builtin_memcpy(m + addr, &v, 2);
}

static inline void mem_w32_m(uint8_t* __restrict__ m, uint32_t addr, uint32_t v) {
    __builtin_memcpy(m + addr, &v, 4);
}

static inline void mem_w64_m(uint8_t* __restrict__ m, uint32_t addr, uint64_t v) {
    __builtin_memcpy(m + addr, &v, 8);
}

static inline void mem_wf32_m(uint8_t* __restrict__ m, uint32_t addr, float v) {
    __builtin_memcpy(m + addr, &v, 4);
}

static inline void mem_wf64_m(uint8_t* __restrict__ m, uint32_t addr, double v) {
    __builtin_memcpy(m + addr, &v, 8);
}

#ifdef RECOMP_USE_LOCAL_MEM
#define mem_r8(addr)      mem_r8_m(_m, (addr))
#define mem_r16(addr)     mem_r16_m(_m, (addr))
#define mem_r32(addr)     mem_r32_m(_m, (addr))
#define mem_r64(addr)     mem_r64_m(_m, (addr))
#define mem_rf32(addr)    mem_rf32_m(_m, (addr))
#define mem_rf64(addr)    mem_rf64_m(_m, (addr))
#define mem_w8(addr, v)   mem_w8_m(_m, (addr), (v))
#define mem_w16(addr, v)  mem_w16_m(_m, (addr), (v))
#define mem_w32(addr, v)  mem_w32_m(_m, (addr), (v))
#define mem_w64(addr, v)  mem_w64_m(_m, (addr), (v))
#define mem_wf32(addr, v) mem_wf32_m(_m, (addr), (v))
#define mem_wf64(addr, v) mem_wf64_m(_m, (addr), (v))
#else
static inline uint8_t mem_r8(uint32_t addr) { return mem_r8_m(g_mem_base, addr); }
static inline uint16_t mem_r16(uint32_t addr) { return mem_r16_m(g_mem_base, addr); }
static inline uint32_t mem_r32(uint32_t addr) { return mem_r32_m(g_mem_base, addr); }
static inline uint64_t mem_r64(uint32_t addr) { return mem_r64_m(g_mem_base, addr); }
static inline float mem_rf32(uint32_t addr) { return mem_rf32_m(g_mem_base, addr); }
static inline double mem_rf64(uint32_t addr) { return mem_rf64_m(g_mem_base, addr); }
static inline void mem_w8(uint32_t addr, uint8_t v) { mem_w8_m(g_mem_base, addr, v); }
static inline void mem_w16(uint32_t addr, uint16_t v) { mem_w16_m(g_mem_base, addr, v); }
static inline void mem_w32(uint32_t addr, uint32_t v) { mem_w32_m(g_mem_base, addr, v); }
static inline void mem_w64(uint32_t addr, uint64_t v) { mem_w64_m(g_mem_base, addr, v); }
static inline void mem_wf32(uint32_t addr, float v) { mem_wf32_m(g_mem_base, addr, v); }
static inline void mem_wf64(uint32_t addr, double v) { mem_wf64_m(g_mem_base, addr, v); }
#endif

typedef union XmmReg {
    float f[4];
    double d[2];
    uint32_t u[4];
    uint64_t u64[2];
} XmmReg;

typedef struct CPUContext {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
    uint32_t eip;
    double st[8];
    int top;
    uint16_t fpu_cw;
    uint16_t fpu_sw;
    XmmReg xmm[8];
    void* fiber;
} CPUContext;

typedef void (*RecompFn)(CPUContext* __restrict__ ctx, uint8_t* __restrict__ _m);

static inline uint8_t reg_r8l(uint32_t r) { return (uint8_t)(r & 0xFFu); }
static inline uint8_t reg_r8h(uint32_t r) { return (uint8_t)((r >> 8) & 0xFFu); }
static inline uint16_t reg_r16(uint32_t r) { return (uint16_t)(r & 0xFFFFu); }

static inline void reg_w8l(uint32_t* r, uint8_t v) {
    *r = (*r & ~0xFFu) | (uint32_t)v;
}
static inline void reg_w8h(uint32_t* r, uint8_t v) {
    *r = (*r & ~0xFF00u) | ((uint32_t)v << 8);
}
static inline void reg_w16(uint32_t* r, uint16_t v) {
    *r = (*r & ~0xFFFFu) | (uint32_t)v;
}

static inline void push32_m(CPUContext* __restrict__ ctx, uint8_t* __restrict__ m, uint32_t v) {
    ctx->esp -= 4;
    mem_w32_m(m, ctx->esp, v);
}

static inline uint32_t pop32_m(CPUContext* __restrict__ ctx, const uint8_t* __restrict__ m) {
    uint32_t v = mem_r32_m(m, ctx->esp);
    ctx->esp += 4;
    return v;
}

#ifdef RECOMP_USE_LOCAL_MEM
#define push32(ctx, v) push32_m((ctx), _m, (v))
#define pop32(ctx)     pop32_m((ctx), _m)
#else
static inline void push32(CPUContext* ctx, uint32_t v) {
    push32_m(ctx, g_mem_base, v);
}

static inline uint32_t pop32(CPUContext* ctx) {
    return pop32_m(ctx, g_mem_base);
}
#endif

static inline uint8_t recomp_parity(uint8_t v) {
    return (uint8_t)(!__builtin_parity((unsigned)v));
}

static inline void fpu_push(CPUContext* ctx, double v) {
    ctx->top = (ctx->top - 1) & 7;
    ctx->st[ctx->top] = v;
}

static inline double fpu_pop(CPUContext* ctx) {
    double v = ctx->st[ctx->top];
    ctx->top = (ctx->top + 1) & 7;
    return v;
}

static inline double fpu_st(const CPUContext* ctx, int idx) {
    return ctx->st[(ctx->top + idx) & 7];
}

static inline void fpu_set(CPUContext* ctx, int idx, double v) {
    ctx->st[(ctx->top + idx) & 7] = v;
}

static inline void fpu_fxch(CPUContext* ctx, int idx) {
    int a = ctx->top & 7;
    int b = (ctx->top + idx) & 7;
    double t = ctx->st[a];
    ctx->st[a] = ctx->st[b];
    ctx->st[b] = t;
}

static inline int64_t fpu_round_i64(double v, uint16_t cw) {
    if (__builtin_expect(!(v >= -9223372036854775808.0 && v < 9223372036854775808.0), 0))
        return INT64_MIN;
    int rc = (cw >> 10) & 3;
    if (__builtin_expect(rc == 3, 1)) return (int64_t)v;
    if (__builtin_expect(rc == 0, 1)) return (int64_t)__builtin_llrint(v);
    double r = (rc == 1) ? __builtin_floor(v) : __builtin_ceil(v);
    if (__builtin_expect(r >= 9223372036854775808.0 || r < -9223372036854775808.0, 0))
        return INT64_MIN;
    return (int64_t)r;
}

static inline int32_t fpu_round_i32(double v, uint16_t cw) {
    if (__builtin_expect(!(v > -2147483649.0 && v < 2147483648.0), 0))
        return INT32_MIN;
    int rc = (cw >> 10) & 3;
    if (__builtin_expect(rc == 3, 1)) return (int32_t)v;
    if (__builtin_expect(rc == 0, 1)) {
        long iv = __builtin_lrint(v);
        if (__builtin_expect(iv < INT32_MIN || iv > INT32_MAX, 0)) return INT32_MIN;
        return (int32_t)iv;
    }
    double r = (rc == 1) ? __builtin_floor(v) : __builtin_ceil(v);
    if (__builtin_expect(r < (double)INT32_MIN || r > (double)INT32_MAX, 0)) return INT32_MIN;
    return (int32_t)r;
}

static inline int16_t fpu_round_i16(double v, uint16_t cw) {
    int32_t iv = fpu_round_i32(v, cw);
    if (__builtin_expect(iv < INT16_MIN || iv > INT16_MAX, 0)) return INT16_MIN;
    return (int16_t)iv;
}

static inline void fpu_fucom(CPUContext* ctx, double a, double b, uint8_t* zf, uint8_t* pf, uint8_t* cf) {
    uint16_t sw = ctx->fpu_sw & (uint16_t)~0x4500u;
    if (isnan(a) || isnan(b)) {
        *zf = 1; *pf = 1; *cf = 1;
        sw |= 0x4500u;
    } else if (a < b) {
        *zf = 0; *pf = 0; *cf = 1;
        sw |= 0x0100u;
    } else if (a == b) {
        *zf = 1; *pf = 0; *cf = 0;
        sw |= 0x4000u;
    } else {
        *zf = 0; *pf = 0; *cf = 0;
    }
    ctx->fpu_sw = sw;
}

static inline void fpu_fprem(CPUContext* ctx) {
    double a = fpu_st(ctx, 0);
    double b = fpu_st(ctx, 1);
    fpu_set(ctx, 0, fmod(a, b));
    ctx->fpu_sw &= (uint16_t)~0x0400u; /* Clear C2: reduction complete */
}

/* Dispatch & indirect calls */
RecompFn recomp_lookup(uint32_t addr);
void recomp_icall(CPUContext* ctx, uint32_t addr);
void recomp_tailcall(CPUContext* ctx, uint32_t addr);
void recomp_int3(CPUContext* ctx, uint32_t addr);

/* setjmp/longjmp + EScriptVM coroutine fiber support (hle_jmp.c) */
typedef struct RecompJmpSlot {
    jmp_buf env;
    void* fiber;
    uint32_t guest_jmpbuf;
    uint32_t guest_esp;
    uint32_t guest_ebp;
    uint32_t guest_ebx;
    uint32_t guest_esi;
    uint32_t guest_edi;
    int fpu_top;
    uint16_t fpu_cw;
    uint16_t fpu_sw;
    int val;
    uint32_t slot_id;
    uint64_t fiber_gen;
    bool active;
} RecompJmpSlot;

RecompJmpSlot* recomp_setjmp_prep(CPUContext* ctx, uint32_t guest_jmpbuf_addr);
void recomp_setjmp_restore(CPUContext* ctx, RecompJmpSlot* slot);
void recomp_escript_run_coro(CPUContext* ctx, uint32_t thread_ptr32, uint32_t new_esp32, RecompFn body_fn);
void recomp_escript_free_coro(uint32_t thread_ptr32);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_CPU_H */

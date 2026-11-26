#define _GNU_SOURCE
#include "runtime/cpu.h"
#include "runtime/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef __SWITCH__
/* AArch64 callee-saved fiber context */
typedef struct FiberRegs {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, sp;
    double d8, d9, d10, d11, d12, d13, d14, d15;
} FiberRegs;

extern void recomp_aarch64_fiber_swap(FiberRegs* from, FiberRegs* to);
extern void recomp_aarch64_fiber_trampoline(void);
#else
#include <ucontext.h>
#endif

#define JMP_MAGIC 0x5245434F4D504A42ULL /* "RECOMPJB" */
#define MAX_JMP_SLOTS 65536
#define MAX_CORO_FIBERS 128
#define CORO_HOST_STACK_SIZE (512 * 1024)

typedef struct RecompFiber {
    bool in_use;
    bool finished;
    uint32_t thread_ptr32;
    uint32_t new_esp32;
    uint32_t guest_ebp32;
    uint64_t gen;
    CPUContext coro_ctx;
    RecompFn body_fn;
    uint8_t* host_stack;
    RecompJmpSlot* last_slot;
#ifdef __SWITCH__
    FiberRegs regs;
#else
    ucontext_t uctx;
#endif
} RecompFiber;

static RecompFiber g_main_fiber;
static RecompFiber g_coro_fibers[MAX_CORO_FIBERS];
static __thread RecompFiber* t_current_fiber = NULL;
static pthread_mutex_t g_jmp_lock = PTHREAD_MUTEX_INITIALIZER;

static RecompJmpSlot g_jmp_slots[MAX_JMP_SLOTS];
static uint32_t g_next_slot = 0;
static uint64_t g_next_fiber_gen = 2;
static RecompJmpSlot* g_active_sched_slot = NULL;

static RecompFiber* get_current_fiber(void) {
    if (!t_current_fiber) {
        if (g_main_fiber.gen == 0) {
            g_main_fiber.gen = 1;
        }
        t_current_fiber = &g_main_fiber;
    }
    return t_current_fiber;
}

static void fiber_swap(RecompFiber* from, RecompFiber* to) {
    t_current_fiber = to;
#ifdef __SWITCH__
    recomp_aarch64_fiber_swap(&from->regs, &to->regs);
#else
    swapcontext(&from->uctx, &to->uctx);
#endif
}

static void coro_entry_common(RecompFiber* f) {
    t_current_fiber = f;
    f->body_fn(&f->coro_ctx, g_mem_base);
    f->finished = true;
    /* Shouldn't return normally; if it ever does, swap back to main */
    fiber_swap(f, &g_main_fiber);
}

#ifdef __SWITCH__
void recomp_aarch64_fiber_entry_c(void) {
    RecompFiber* f = t_current_fiber;
    coro_entry_common(f);
}
#else
static void linux_coro_trampoline(uint32_t lo, uint32_t hi) {
    uintptr_t p = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    coro_entry_common((RecompFiber*)p);
}
#endif

static void init_coro_fiber(RecompFiber* f) {
    if (!f->host_stack) {
        f->host_stack = (uint8_t*)aligned_alloc(16, CORO_HOST_STACK_SIZE);
    }
#ifdef __SWITCH__
    memset(&f->regs, 0, sizeof(f->regs));
    uintptr_t top = ((uintptr_t)(f->host_stack + CORO_HOST_STACK_SIZE)) & ~15ULL;
    f->regs.sp = (uint64_t)top;
    f->regs.x30 = (uint64_t)(uintptr_t)recomp_aarch64_fiber_trampoline;
#else
    getcontext(&f->uctx);
    f->uctx.uc_stack.ss_sp = f->host_stack;
    f->uctx.uc_stack.ss_size = CORO_HOST_STACK_SIZE;
    f->uctx.uc_link = &g_main_fiber.uctx;
    uintptr_t p = (uintptr_t)f;
    uint32_t lo = (uint32_t)(p & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)((uint64_t)p >> 32);
    makecontext(&f->uctx, (void (*)(void))linux_coro_trampoline, 2, lo, hi);
#endif
}

static bool is_jmp_slot_protected(const RecompJmpSlot* s, RecompFiber* cur, uint32_t cur_esp) {
    if (!s->active || !s->fiber) return false;
    if (s == g_active_sched_slot) return true;
    RecompFiber* sf = (RecompFiber*)s->fiber;
    if (s->fiber_gen != sf->gen) return false;
    if (sf == &g_main_fiber) {
        uint32_t main_esp = (cur == &g_main_fiber) ? cur_esp : g_main_fiber.coro_ctx.esp;
        return main_esp < s->guest_esp;
    }
    if (!sf->in_use || sf->finished) return false;
    uint32_t f_esp = (sf == cur) ? cur_esp : sf->coro_ctx.esp;
    return (sf == cur) ? (f_esp < s->guest_esp) : (f_esp <= s->guest_esp);
}

RecompJmpSlot* recomp_setjmp_prep(CPUContext* ctx, uint32_t guest_jmpbuf_addr) {
    RecompFiber* cur = get_current_fiber();
    RecompJmpSlot* slot = NULL;
    uint32_t id = 0;
    RecompJmpSlot* last = cur->last_slot;
    if (last && last->active && last->fiber == cur && last->fiber_gen == cur->gen &&
        last->guest_jmpbuf == guest_jmpbuf_addr && last != g_active_sched_slot) {
        slot = last;
        id = last->slot_id;
    } else if (mem_r64(guest_jmpbuf_addr) == JMP_MAGIC) {
        uint32_t prev_id = mem_r32(guest_jmpbuf_addr + 8);
        if (prev_id < MAX_JMP_SLOTS) {
            RecompJmpSlot* prev = &g_jmp_slots[prev_id];
            if (prev->active && prev->fiber == cur && prev->fiber_gen == cur->gen &&
                prev->guest_jmpbuf == guest_jmpbuf_addr && prev != g_active_sched_slot) {
                slot = prev;
                id = prev_id;
                cur->last_slot = slot;
            }
        }
    }
    if (!slot) {
        pthread_mutex_lock(&g_jmp_lock);
        bool found = false;
        for (uint32_t tries = 0; tries < MAX_JMP_SLOTS; tries++) {
            uint32_t cand = (g_next_slot + tries) % MAX_JMP_SLOTS;
            if (!is_jmp_slot_protected(&g_jmp_slots[cand], cur, ctx->esp)) {
                id = cand;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "[jmp] FATAL: exhausted all %u jmp slots!\n", (unsigned)MAX_JMP_SLOTS);
            fflush(stderr);
            abort();
        }
        g_next_slot = (id + 1u) % MAX_JMP_SLOTS;
        slot = &g_jmp_slots[id];
        slot->active = true;
        slot->slot_id = id;
        slot->fiber = cur;
        slot->fiber_gen = cur->gen;
        slot->guest_jmpbuf = guest_jmpbuf_addr;
        cur->last_slot = slot;
        pthread_mutex_unlock(&g_jmp_lock);
    }

    slot->guest_esp = ctx->esp;
    slot->guest_ebp = ctx->ebp;
    slot->guest_ebx = ctx->ebx;
    slot->guest_esi = ctx->esi;
    slot->guest_edi = ctx->edi;
    slot->fpu_top = ctx->top;
    slot->fpu_cw = ctx->fpu_cw;
    slot->fpu_sw = ctx->fpu_sw;
    slot->val = 0;

    if (cur == &g_main_fiber && guest_jmpbuf_addr >= 0x10000000u && guest_jmpbuf_addr < 0x3F000000u) {
        /* Track active RunScript scheduler jmp_buf on Fiber 0 */
        g_active_sched_slot = slot;
    }

    mem_w64(guest_jmpbuf_addr, JMP_MAGIC);
    mem_w32(guest_jmpbuf_addr + 8, id);
    return slot;
}

void recomp_setjmp_restore(CPUContext* ctx, RecompJmpSlot* slot) {
    ctx->esp = slot->guest_esp;
    ctx->ebp = slot->guest_ebp;
    ctx->ebx = slot->guest_ebx;
    ctx->esi = slot->guest_esi;
    ctx->edi = slot->guest_edi;
    ctx->top = slot->fpu_top;
    ctx->fpu_cw = slot->fpu_cw;
    ctx->fpu_sw = slot->fpu_sw;
    ctx->eax = (uint32_t)(slot->val ? slot->val : 1);
}

void recomp_escript_run_coro(CPUContext* ctx, uint32_t thread_ptr32, uint32_t new_esp32, RecompFn body_fn) {
    RecompFiber* f = NULL;
    for (int i = 0; i < MAX_CORO_FIBERS; i++) {
        if (g_coro_fibers[i].in_use && g_coro_fibers[i].thread_ptr32 == thread_ptr32) {
            f = &g_coro_fibers[i];
            break;
        }
    }
    if (!f) {
        for (int i = 0; i < MAX_CORO_FIBERS; i++) {
            if (!g_coro_fibers[i].in_use || g_coro_fibers[i].finished) {
                f = &g_coro_fibers[i];
                break;
            }
        }
    }
    if (!f) {
        fprintf(stderr, "[coro] Out of coroutine fibers!\n");
        abort();
    }

    f->in_use = true;
    f->finished = false;
    f->thread_ptr32 = thread_ptr32;
    f->new_esp32 = new_esp32;
    f->guest_ebp32 = ctx->ebp;
    f->gen = g_next_fiber_gen++;
    f->last_slot = NULL;
    f->body_fn = body_fn;
    memcpy(&f->coro_ctx, ctx, sizeof(CPUContext));
    f->coro_ctx.esp = new_esp32;
    f->coro_ctx.ebp = ctx->ebp;
    init_coro_fiber(f);

    if (g_main_fiber.gen == 0) g_main_fiber.gen = 1;
    g_main_fiber.coro_ctx.esp = ctx->esp;
    RecompJmpSlot* saved_sched = g_active_sched_slot;
    fiber_swap(&g_main_fiber, f);
    g_active_sched_slot = saved_sched;
    longjmp(saved_sched->env, saved_sched->val);
}

void recomp_escript_free_coro(uint32_t thread_ptr32) {
    for (int i = 0; i < MAX_CORO_FIBERS; i++) {
        if (g_coro_fibers[i].in_use && g_coro_fibers[i].thread_ptr32 == thread_ptr32) {
            g_coro_fibers[i].finished = true;
            g_coro_fibers[i].in_use = false;
        }
    }
}

void imp__setjmp(CPUContext* ctx) {
    /* Fallback stub if ever called indirectly */
    uint32_t buf = mem_r32(ctx->esp + 4);
    (void)recomp_setjmp_prep(ctx, buf);
    ctx->eax = 0;
}

static RecompJmpSlot* g_pending_resume_slot_internal = NULL;

void imp_longjmp(CPUContext* ctx) {
    uint32_t buf = mem_r32(ctx->esp + 4);
    int val = (int)mem_r32(ctx->esp + 8);
    if (val == 0) val = 1;

    uint64_t magic = mem_r64(buf);
    if (magic != JMP_MAGIC) {
        fprintf(stderr, "[jmp] FATAL: invalid guest jmp_buf at 0x%08x (magic=0x%llx)\n",
                buf, (unsigned long long)magic);
        abort();
    }
    uint32_t id = mem_r32(buf + 8);
    if (id >= MAX_JMP_SLOTS) {
        fprintf(stderr, "[jmp] FATAL: slot_id %u out of range\n", id);
        abort();
    }

    RecompJmpSlot* target = &g_jmp_slots[id];
    RecompFiber* cur = get_current_fiber();
    RecompFiber* dst_fiber = (RecompFiber*)target->fiber;

    if (dst_fiber == cur) {
        if (ctx->esp > target->guest_esp) {
            fprintf(stderr, "[jmp] FATAL: stale longjmp! buf=0x%08x id=%u cur_esp=0x%08x target_esp=0x%08x target_jmpbuf=0x%08x\n",
                    buf, id, ctx->esp, target->guest_esp, target->guest_jmpbuf);
            fflush(stderr);
            abort();
        }
        target->val = val;
        longjmp(target->env, val);
    }

    /* Cross-fiber jump */
    if (dst_fiber == &g_main_fiber) {
        /* Jumping from coroutine back to RunScript scheduler on Fiber 0:
         * Use currently active RunScript jmp_buf so stack frames always match! */
        if (!g_active_sched_slot) {
            fprintf(stderr, "[jmp] FATAL: no active scheduler slot on main fiber!\n");
            abort();
        }
        g_active_sched_slot->val = val;
        /* Mark fiber finished if longjmp came from 0x86b3880 (script termination) */
        if (buf == 0x086B3880u) {
            cur->finished = true;
        } else {
            cur->coro_ctx.esp = ctx->esp;
        }
        fiber_swap(cur, &g_main_fiber);
        /* Execution on `cur` fiber resumes RIGHT HERE when RunScript later resumes this coroutine! */
        /* Wait: when RunScript swaps back to `cur`, `cur` had yielded at YieldThread (`buf != 0x86b3880`),
         * and RunScript passed us the slot saved at YieldThread! Look up the coroutine's yielded slot: */
        RecompJmpSlot* resume_slot = g_pending_resume_slot_internal;
        longjmp(resume_slot->env, resume_slot->val);
    } else {
        /* Jumping from Fiber 0 (RunScript) into paused Coro Fiber */
        RecompJmpSlot* saved_sched = g_active_sched_slot;
        target->val = val;
        g_pending_resume_slot_internal = target;
        cur->coro_ctx.esp = ctx->esp;
        fiber_swap(cur, dst_fiber);
        /* Fiber 0 resumes RIGHT HERE when Coro Fiber yields or exits back to main! */
        g_active_sched_slot = saved_sched;
        longjmp(saved_sched->env, saved_sched->val);
    }
}

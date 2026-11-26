#define _GNU_SOURCE
#include "runtime/cpu.h"
#include "runtime/launcher_ui.h"
#include "runtime/memory.h"
#include "runtime/postfx.h"
#include "runtime/settings.h"
#include "runtime/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __SWITCH__
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

#define HOST_PROF_MAX_SAMPLES 131072
static uintptr_t g_prof_pcs[HOST_PROF_MAX_SAMPLES];
static volatile uint32_t g_prof_count = 0;
static uint64_t g_prof_start_frame = 0;
static bool g_prof_active = false;

static void host_sigprof_handler(int sig, siginfo_t* si, void* uctx) {
    (void)sig; (void)si;
    uint32_t idx = g_prof_count;
    if (idx < HOST_PROF_MAX_SAMPLES) {
        ucontext_t* uc = (ucontext_t*)uctx;
        g_prof_pcs[idx] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        g_prof_count = idx + 1u;
    }
}

void recomp_host_prof_dump(void) {
    if (!g_prof_active) return;
    struct itimerval zero = {{0, 0}, {0, 0}};
    setitimer(ITIMER_PROF, &zero, NULL);
    g_prof_active = false;
    FILE* fp = fopen("/tmp/pshy_pcs.txt", "w");
    if (fp) {
        extern int main(int, char**);
        fprintf(fp, "MAIN 0x%lx\n", (unsigned long)(uintptr_t)&main);
        uint32_t n = g_prof_count;
        for (uint32_t i = 0; i < n; i++) {
            fprintf(fp, "0x%lx\n", (unsigned long)g_prof_pcs[i]);
        }
        fclose(fp);
        fprintf(stderr, "[prof] Dumped %u PC samples to /tmp/pshy_pcs.txt\n", n);
    }
}

void recomp_host_prof_frame(uint64_t frame) {
    if (g_prof_start_frame == 0) return;
    if (!g_prof_active && frame >= g_prof_start_frame) {
        g_prof_active = true;
        g_prof_count = 0;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = host_sigprof_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        struct itimerval it = {{0, 1000}, {0, 1000}}; /* 1000 Hz */
        setitimer(ITIMER_PROF, &it, NULL);
        fprintf(stderr, "[prof] Started 1000 Hz SIGPROF sampling at frame %llu\n", (unsigned long long)frame);
    }
}
#endif

#ifdef __SWITCH__
#include <SDL2/SDL_thread.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

static AppletHookCookie g_applet_cookie;

static void switch_applet_hook_cb(AppletHookType hook, void* param) {
    (void)param;
    if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnResume) {
        AppletFocusState fs = appletGetFocusState();
        bool focused = (fs == AppletFocusState_InFocus);
        recomp_openal_set_suspended(!focused);
        recomp_sdl_notify_focus(focused);
        recomp_time_notify_pause(!focused);
    } else if (hook == AppletHookType_OnOperationMode) {
        recomp_sdl_notify_operation_mode();
    }
}

Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid);

/* Redirect default ideal core (-2 -> Core 0) used by SDL2 audio, OpenAL, Mesa, and FFmpeg
 * onto Core 2 so Core 0 stays dedicated to the main recompiled game thread. */
Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid) {
    void* lr = __builtin_return_address(0);
    int target_cpuid = (cpuid == -2) ? 2 : cpuid;
    Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, target_cpuid);
    if (R_FAILED(rc) && target_cpuid != cpuid) {
        fprintf(stderr, "[threadCreate] entry=%p prio=%d cpuid=%d->%d failed=0x%x fallback=%d stack=0x%zx lr=%p\n",
                (void*)entry, prio, cpuid, target_cpuid, (unsigned)rc, cpuid, stack_sz, lr);
        target_cpuid = cpuid;
        rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, target_cpuid);
    }
    fprintf(stderr, "[threadCreate] entry=%p prio=%d cpuid=%d->%d stack=0x%zx lr=%p rc=0x%x\n",
            (void*)entry, prio, cpuid, target_cpuid, stack_sz, lr, (unsigned)rc);
    fflush(stderr);
    return rc;
}

/* Fix devkitPro libSDL2 SDL_SYS_SetThreadPriority bug: only HIGH (2) maps to prio 43;
 * TIME_CRITICAL (3, used by SDL_RunAudio) fell through to 59 and starved under cooperative
 * scheduling on Core 2 alongside Mesa/guest/perf workers at prio 59. */
int __wrap_SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority) {
    int hos_prio = 0x3B; /* 59: LOW / NORMAL */
    if (priority == SDL_THREAD_PRIORITY_TIME_CRITICAL) {
        hos_prio = 0x28; /* 40: preempts guest (50) and background workers (59) on Core 2 */
    } else if (priority == SDL_THREAD_PRIORITY_HIGH) {
        hos_prio = 0x2B; /* 43 */
    }
    Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, hos_prio);
    fprintf(stderr, "[sdl-prio] SDL_SYS_SetThreadPriority(%d) -> HOS prio=%d rc=0x%x\n",
            (int)priority, hos_prio, (unsigned)rc);
    fflush(stderr);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

Result __real_nwindowDequeueBuffer(NWindow* nw, s32* out_slot, NvMultiFence* out_fence);
_Atomic uint64_t g_nw_wb_ticks = 0;
_Atomic uint64_t g_nw_fn_ticks = 0;
_Atomic uint32_t g_nw_wb_count = 0;

/* Fix libnx nwindowDequeueBuffer VBlank stall when swap_interval == 0:
 * libnx unconditionally calls eventWait(&nw->event, UINT64_MAX) BEFORE
 * bqDequeueBuffer(..., async=true, ...), which blocks up to 16.67ms on the
 * next 60 Hz VBlank even when a free buffer slot is already available in
 * nvflinger (causing clr=9.4-17.7ms stalls and capping FPS at 40-41 FPS).
 * Try non-blocking bqDequeueBuffer FIRST and only eventWait on WouldBlock. */
Result __wrap_nwindowDequeueBuffer(NWindow* nw, s32* out_slot, NvMultiFence* out_fence) {
    if (!nw || !nwindowIsValid(nw) || nw->swap_interval != 0 || !eventActive(&nw->event)) {
        return __real_nwindowDequeueBuffer(nw, out_slot, out_fence);
    }
    mutexLock(&nw->mutex);
    if (!nw->slots_configured || nw->cur_slot >= 0) {
        mutexUnlock(&nw->mutex);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    s32 slot = -1;
    NvMultiFence fence;
    memset(&fence, 0, sizeof(fence));
    Result rc = bqDequeueBuffer(&nw->bq, true, nw->width, nw->height, nw->format, nw->usage, &slot, &fence);
    while (R_VALUE(rc) == MAKERESULT(Module_LibnxBinder, LibnxBinderError_WouldBlock)) {
        uint64_t t0 = armGetSystemTick();
        eventWait(&nw->event, UINT64_MAX);
        rc = bqDequeueBuffer(&nw->bq, true, nw->width, nw->height, nw->format, nw->usage, &slot, &fence);
        __atomic_fetch_add(&g_nw_wb_ticks, armGetSystemTick() - t0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_nw_wb_count, 1u, __ATOMIC_RELAXED);
    }
    if (R_SUCCEEDED(rc)) {
        if (!(nw->slots_requested & (1ULL << slot))) {
            rc = bqRequestBuffer(&nw->bq, slot, NULL);
            if (R_FAILED(rc)) {
                bqCancelBuffer(&nw->bq, slot, &fence);
                mutexUnlock(&nw->mutex);
                return rc;
            }
            nw->slots_requested |= (1ULL << slot);
        }
        nw->cur_slot = slot;
        if (out_slot) *out_slot = slot;
        if (out_fence) {
            *out_fence = fence;
        } else {
            uint64_t ft0 = armGetSystemTick();
            nvMultiFenceWait(&fence, -1);
            __atomic_fetch_add(&g_nw_fn_ticks, armGetSystemTick() - ft0, __ATOMIC_RELAXED);
        }
    }
    mutexUnlock(&nw->mutex);
    return rc;
}
#endif

extern void recomp_init_got_plt(void);
extern void recomp_init_stdio_handles(void);
extern void fn_0852386e(CPUContext* __restrict__ ctx, uint8_t* __restrict__ _m); /* guest main(int argc, char** argv) */

static void run_guest_ctors(CPUContext* ctx) {
    /* __CTOR_LIST__ = 0x08687ce0, __CTOR_END__ = 0x08687ebc
     * __do_global_ctors_aux walks backwards from 0x08687eb8 until 0xFFFFFFFF */
    uint32_t p = 0x08687EB8u;
    int count = 0;
    while (p >= 0x08687CE0u) {
        uint32_t fn = mem_r32(p);
        if (fn == 0xFFFFFFFFu) break;
        if (fn != 0) {
            push32(ctx, 0xDEAD0000u);
            recomp_icall(ctx, fn);
            count++;
        }
        p -= 4u;
    }
    fprintf(stderr, "[main] Executed %d C++ global constructors\n", count);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/psychonauts", 0755);
    freopen("sdmc:/switch/psychonauts/recomp.log", "w", stderr);
    dup2(fileno(stderr), fileno(stdout));
    setvbuf(stderr, NULL, _IOFBF, 65536);
    setvbuf(stdout, NULL, _IOFBF, 65536);
    /* Allow process threads to schedule across all 3 application CPU cores (0, 1, 2) */
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 0x7);

    Result sock_rc = socketInitializeDefault();
    AppletType atype = appletGetAppletType();
    u32 hos_ver = hosversionGet();
    fprintf(stderr, "[switch] Psychonauts recomp starting (HOS=0x%08x, AppletType=%d, socket=0x%x)\n",
            (unsigned)hos_ver, (int)atype, (unsigned)sock_rc);
    if (atype != AppletType_Application && atype != AppletType_SystemApplication) {
        fprintf(stderr, "[switch] WARNING: Launched in Applet Mode! Hold R over a game for full RAM.\n");
    } else {
        appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
    }
    appletHook(&g_applet_cookie, switch_applet_hook_cb, NULL);
    const char* default_dir = "sdmc:/switch/psychonauts";
#else
    const char* default_dir = "game/linux";
#endif

    const char* env_dir = getenv("RECOMP_GAME_DIR");
    if (env_dir && *env_dir) {
        recomp_set_game_dir(env_dir);
    } else {
        recomp_set_game_dir(default_dir);
    }
#ifndef __SWITCH__
    {
        const char* psf = getenv("RECOMP_PROF_START_FRAME");
        if (psf && atoi(psf) > 0) g_prof_start_frame = (uint64_t)atoll(psf);
    }
#endif

#ifdef __SWITCH__
    mkdir("sdmc:/switch/psychonauts/shader_cache", 0755);
    setenv("MESA_GLSL_CACHE_DIR", "sdmc:/switch/psychonauts/shader_cache", 1);
    setenv("MESA_SHADER_CACHE_DIR", "sdmc:/switch/psychonauts/shader_cache", 1);
    setenv("MESA_NO_ERROR", "1", 1);
#endif

    /* Reserve the 1 GB guest arena in g_StackRegion BEFORE launcher_ui_run()
     * initializes SDL2/EGL/Mesa, which can fragment g_StackRegion on Switch
     * and cause virtmemFindStack(1GB) -> fallback to ASLR -> svcMapMemory 0xdc01. */
    if (!recomp_mem_init()) {
        fprintf(stderr, "[main] Memory initialization failed!\n");
#ifdef __SWITCH__
        appletUnhook(&g_applet_cookie);
        if (R_SUCCEEDED(sock_rc)) socketExit();
#endif
        return 1;
    }

    recomp_settings_load(recomp_get_game_dir());
    launcher_ui_run(recomp_get_game_dir());

    {
        int res_w = 1280, res_h = 720;
        recomp_resolve_target_resolution(&res_w, &res_h);
        recomp_sync_display_settings_ini(recomp_get_game_dir(), res_w, res_h);
    }

#ifdef __SWITCH__
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
#endif

    vfs_init(recomp_get_game_dir());

    char elf_path[1024];
    snprintf(elf_path, sizeof(elf_path), "%s/Psychonauts", recomp_get_game_dir());
    if (!recomp_load_elf(elf_path)) {
        fprintf(stderr, "[main] Failed to load ELF: %s\n", elf_path);
        recomp_mem_shutdown();
        return 1;
    }

    recomp_init_got_plt();
    recomp_init_stdio_handles();

    CPUContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fpu_cw = 0x037Fu;
    ctx.esp = 0x097FFF00u;

    /* Prepare guest argv = { "<game_dir>/Psychonauts", NULL } */
    uint32_t guest_argv0 = guest_strdup(elf_path);
    uint32_t guest_argv = guest_rt_alloc(16, 4);
    mem_w32(guest_argv + 0, guest_argv0);
    mem_w32(guest_argv + 4, 0);

    run_guest_ctors(&ctx);

    fprintf(stderr, "[main] Entering guest main(1, 0x%08x)...\n", guest_argv);
    push32(&ctx, guest_argv);
    push32(&ctx, 1u);
    push32(&ctx, 0xDEAD9999u);
    fn_0852386e(&ctx, g_mem_base);
    int exit_code = (int)ctx.eax;
    fprintf(stderr, "[main] Guest main exited with code %d\n", exit_code);

    recomp_postfx_shutdown();
    recomp_sdl_shutdown_rumble();
    recomp_sdl_shutdown_sensors();
    recomp_mem_shutdown();
#ifdef __SWITCH__
    appletUnhook(&g_applet_cookie);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if (R_SUCCEEDED(sock_rc)) socketExit();
#endif
    return exit_code;
}

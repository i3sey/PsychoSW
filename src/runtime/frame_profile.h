#ifndef RECOMP_FRAME_PROFILE_H
#define RECOMP_FRAME_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

enum RecompFrameTiming {
    RECOMP_TIME_REQUEST_SCENE,
    RECOMP_TIME_PREPROCESS_MESHES,
    RECOMP_TIME_DRAW_OPAQUE,
    RECOMP_TIME_DRAW_TRANSPARENT,
    RECOMP_TIME_AFFECTORS,
    RECOMP_TIME_SIM_ENTITY_SCAN,
    RECOMP_TIME_SIM_ACTIONS,
    RECOMP_TIME_SIM_PHYSICS,
    RECOMP_TIME_SORT_MESHES,
    RECOMP_TIME_COUNT,
};

extern uint64_t g_recomp_frame_ticks[RECOMP_TIME_COUNT];
extern uint32_t g_recomp_geometry_calls;
uint64_t recomp_phase_now(void);

enum RecompSimPhase {
    RECOMP_SIM_ACTIONS,
    RECOMP_SIM_PHYSICS,
    RECOMP_SIM_ANIMATION,
    RECOMP_SIM_COLLISION,
    RECOMP_SIM_COLLISION_DETAIL,
    RECOMP_SIM_PLAYER,
    RECOMP_SIM_RENDER,
    RECOMP_SIM_FRAME,
    RECOMP_SIM_PHASE_COUNT,
};

#ifdef RECOMP_SIM_DETAIL_PROF
extern bool g_recomp_sample_sim;
extern _Thread_local bool g_recomp_sample_collision;
void recomp_profile_sim_target(enum RecompSimPhase phase, uint32_t addr, uint64_t ticks);

#define RECOMP_SAMPLE_CALL_IF(enabled, phase, addr, call) do {      \
    uint64_t sample_start = (enabled) ? recomp_phase_now() : 0;     \
    call;                                                          \
    if (sample_start)                                              \
        recomp_profile_sim_target(phase, addr, recomp_phase_now() - sample_start); \
} while (0)

#define RECOMP_SAMPLE_SIM_CALL(phase, addr, call) \
    RECOMP_SAMPLE_CALL_IF(g_recomp_sample_sim, phase, addr, call)
#define RECOMP_SAMPLE_COLLISION_DETAIL(addr, call) \
    RECOMP_SAMPLE_CALL_IF(g_recomp_sample_collision, RECOMP_SIM_COLLISION_DETAIL, addr, call)
#else
static inline void recomp_profile_sim_target(enum RecompSimPhase phase, uint32_t addr, uint64_t ticks) {
    (void)phase; (void)addr; (void)ticks;
}
#define RECOMP_SAMPLE_SIM_CALL(phase, addr, call) do { call; } while (0)
#define RECOMP_SAMPLE_COLLISION_DETAIL(addr, call) do { call; } while (0)
#endif

/* Use on outer call sites so recursive scene and domain work is counted once. */
#define RECOMP_TIME_CALL(phase, call) do {                         \
    uint64_t start = recomp_phase_now();                          \
    call;                                                         \
    g_recomp_frame_ticks[phase] += recomp_phase_now() - start;      \
} while (0)

#endif

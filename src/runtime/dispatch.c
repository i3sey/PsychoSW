#include "runtime/cpu.h"
#include "runtime/memory.h"
#include <stdio.h>

/* recomp_icall and recomp_tailcall are implemented directly in recomp_dispatch.c
 * so recomp_lookup() is inlined with zero cross-TU call overhead. */
uint64_t g_recomp_icall_count = 0;

void recomp_int3(CPUContext* ctx, uint32_t addr) {
    (void)ctx;
    fprintf(stderr, "[recomp] Guest hit int3 at 0x%08x (continuing)\n", addr);
}

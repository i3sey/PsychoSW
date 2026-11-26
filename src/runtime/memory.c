#define _GNU_SOURCE
#include "runtime/memory.h"
#include "runtime/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

uint8_t* g_mem_base = NULL;

static char g_game_dir[1024] = "game/linux";

const char* recomp_get_game_dir(void) {
    return g_game_dir;
}

void recomp_set_game_dir(const char* dir) {
    if (dir && *dir) {
#ifndef __SWITCH__
        char* absolute = realpath(dir, NULL);
        if (absolute) {
            snprintf(g_game_dir, sizeof(g_game_dir), "%s", absolute);
            free(absolute);
            return;
        }
#endif
        snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
    }
}

/* ---------------- Virtual Memory Backing ---------------- */

#ifdef __SWITCH__
#define MAX_SWITCH_MAPPINGS 2048
typedef struct SwitchMapping {
    void* dst;
    void* src;
    size_t size;
} SwitchMapping;

static VirtmemReservation* g_switch_res = NULL;
static SwitchMapping g_switch_maps[MAX_SWITCH_MAPPINGS];
static int g_switch_num_maps = 0;
static uint8_t g_page_bitmap[256 * 1024 / 8]; /* 256K x 4KB pages = 1GB -> 32 KB bitmap */
#else
static uint8_t g_page_bitmap[256 * 1024 / 8];
#endif

static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;

static inline bool page_is_committed(uint32_t page_idx) {
    return (g_page_bitmap[page_idx >> 3] & (1u << (page_idx & 7))) != 0;
}

static inline void page_mark_committed(uint32_t page_idx) {
    g_page_bitmap[page_idx >> 3] |= (uint8_t)(1u << (page_idx & 7));
}

bool recomp_mem_init(void) {
    memset(g_page_bitmap, 0, sizeof(g_page_bitmap));
#ifdef __SWITCH__
    virtmemLock();
    /* svcMapMemory on HOS only accepts addresses inside g_StackRegion (virtmemFindStack) */
    void* base = virtmemFindStack(0x40000000ULL, 0x10000);
    if (!base) {
        base = virtmemFindAslr(0x40000000ULL, 0x10000);
    }
    if (!base) {
        virtmemUnlock();
        fprintf(stderr, "[mem] virtmemFindStack(1GB) failed\n");
        return false;
    }
    g_switch_res = virtmemAddReservation(base, 0x40000000ULL);
    virtmemUnlock();
    if (!g_switch_res) {
        fprintf(stderr, "[mem] virtmemAddReservation(1GB) failed\n");
        return false;
    }
    g_mem_base = (uint8_t*)base;
    fprintf(stderr, "[mem] Switch 1GB arena at %p\n", base);
#else
    void* base = mmap(NULL, 0x40000000ULL, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        perror("[mem] mmap 1GB failed");
        return false;
    }
    g_mem_base = (uint8_t*)base;
#endif
    /* Commit runtime synthetic region 0x08700000..0x08900000 (2 MB) */
    if (!recomp_mem_commit(0x08700000u, 0x00200000u)) return false;
    /* Commit main thread stack region 0x09000000..0x09800000 (8 MB) */
    if (!recomp_mem_commit(0x09000000u, 0x00800000u)) return false;
    return true;
}

void recomp_mem_shutdown(void) {
    if (!g_mem_base) return;
#ifdef __SWITCH__
    for (int i = g_switch_num_maps - 1; i >= 0; i--) {
        svcUnmapMemory(g_switch_maps[i].dst, g_switch_maps[i].src, g_switch_maps[i].size);
        free(g_switch_maps[i].src);
    }
    g_switch_num_maps = 0;
    if (g_switch_res) {
        virtmemLock();
        virtmemRemoveReservation(g_switch_res);
        virtmemUnlock();
        g_switch_res = NULL;
    }
#else
    munmap(g_mem_base, 0x40000000ULL);
#endif
    g_mem_base = NULL;
}

bool recomp_mem_commit(uint32_t guest_addr, uint32_t size) {
    if (size == 0) return true;
    uint64_t start = (uint64_t)guest_addr & ~0xFFFull;
    uint64_t end = ((uint64_t)guest_addr + size + 0xFFFull) & ~0xFFFull;
    if (end > 0x40000000ULL) return false;

    pthread_mutex_lock(&g_mem_lock);
#ifdef __SWITCH__
    /* Coalesce uncommitted spans into contiguous svcMapMemory calls */
    uint64_t cur = start;
    while (cur < end) {
        uint32_t p = (uint32_t)(cur >> 12);
        if (page_is_committed(p)) {
            cur += 0x1000;
            continue;
        }
        uint64_t span_start = cur;
        while (cur < end && !page_is_committed((uint32_t)(cur >> 12))) {
            cur += 0x1000;
        }
        size_t span_size = (size_t)(cur - span_start);
        if (g_switch_num_maps >= MAX_SWITCH_MAPPINGS) {
            pthread_mutex_unlock(&g_mem_lock);
            fprintf(stderr, "[mem] Exceeded MAX_SWITCH_MAPPINGS\n");
            return false;
        }
        void* backing = memalign(0x1000, span_size);
        if (!backing) {
            pthread_mutex_unlock(&g_mem_lock);
            fprintf(stderr, "[mem] Out of Switch RAM allocating %zu bytes\n", span_size);
            return false;
        }
        memset(backing, 0, span_size);
        Result rc = svcMapMemory(g_mem_base + span_start, backing, span_size);
        if (R_FAILED(rc)) {
            free(backing);
            pthread_mutex_unlock(&g_mem_lock);
            fprintf(stderr, "[mem] svcMapMemory(0x%llx, %zu) failed: 0x%x\n",
                    (unsigned long long)span_start, span_size, rc);
            return false;
        }
        g_switch_maps[g_switch_num_maps++] = (SwitchMapping){ g_mem_base + span_start, backing, span_size };
        for (uint64_t a = span_start; a < cur; a += 0x1000) {
            page_mark_committed((uint32_t)(a >> 12));
        }
    }
#else
    /* Commit via mprotect (avoid re-protecting already committed ranges redundantly) */
    uint64_t cur = start;
    while (cur < end) {
        uint32_t p = (uint32_t)(cur >> 12);
        if (page_is_committed(p)) {
            cur += 0x1000;
            continue;
        }
        uint64_t span_start = cur;
        while (cur < end && !page_is_committed((uint32_t)(cur >> 12))) {
            cur += 0x1000;
        }
        if (mprotect(g_mem_base + span_start, (size_t)(cur - span_start), PROT_READ | PROT_WRITE) != 0) {
            pthread_mutex_unlock(&g_mem_lock);
            perror("[mem] mprotect commit failed");
            return false;
        }
        for (uint64_t a = span_start; a < cur; a += 0x1000) {
            page_mark_committed((uint32_t)(a >> 12));
        }
    }
#endif
    pthread_mutex_unlock(&g_mem_lock);
    return true;
}

/* ---------------- ELF32 Loader ---------------- */

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr_Raw;

bool recomp_load_elf(const char* elf_path) {
    FILE* fp = fopen(elf_path, "rb");
    if (!fp) {
        fprintf(stderr, "[elf] Cannot open %s\n", elf_path);
        return false;
    }
    uint8_t ehdr[52];
    if (fread(ehdr, 1, 52, fp) != 52) {
        fclose(fp);
        return false;
    }
    if (ehdr[0] != 0x7F || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F' || ehdr[4] != 1) {
        fprintf(stderr, "[elf] Not ELF32\n");
        fclose(fp);
        return false;
    }
    uint32_t phoff;
    uint16_t phentsize, phnum;
    memcpy(&phoff, ehdr + 28, 4);
    memcpy(&phentsize, ehdr + 42, 2);
    memcpy(&phnum, ehdr + 44, 2);

    for (uint16_t i = 0; i < phnum; i++) {
        Elf32_Phdr_Raw ph;
        fseek(fp, (long)(phoff + i * phentsize), SEEK_SET);
        if (fread(&ph, 1, sizeof(ph), fp) != sizeof(ph)) {
            fclose(fp);
            return false;
        }
        if (ph.p_type != 1 /* PT_LOAD */) continue;
        if (!recomp_mem_commit(ph.p_vaddr, ph.p_memsz)) {
            fclose(fp);
            return false;
        }
        if (ph.p_filesz > 0) {
            fseek(fp, (long)ph.p_offset, SEEK_SET);
            if (fread(g_mem_base + ph.p_vaddr, 1, ph.p_filesz, fp) != ph.p_filesz) {
                fclose(fp);
                return false;
            }
        }
        if (ph.p_memsz > ph.p_filesz) {
            memset(g_mem_base + ph.p_vaddr + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);
        }
        fprintf(stderr, "[elf] Loaded PT_LOAD 0x%08x..0x%08x (file 0x%x)\n",
                ph.p_vaddr, ph.p_vaddr + ph.p_memsz, ph.p_filesz);
    }
    fclose(fp);
    return true;
}

/* ---------------- Runtime Static Bump & Stack Allocators ---------------- */

static uint32_t g_rt_bump = 0x08700000u;
static uint32_t g_rt_end = 0x08900000u;
static uint32_t g_stack_bump = 0x09800000u; /* 0x09000000..0x09800000 reserved for main */
static uint32_t g_stack_end = 0x0D000000u;

uint32_t guest_rt_alloc(uint32_t size, uint32_t align) {
    if (align < 4) align = 4;
    pthread_mutex_lock(&g_mem_lock);
    uint32_t p = (g_rt_bump + (align - 1u)) & ~(align - 1u);
    if (p + size > g_rt_end) {
        pthread_mutex_unlock(&g_mem_lock);
        fprintf(stderr, "[mem] Runtime static arena exhausted!\n");
        abort();
    }
    g_rt_bump = p + size;
    pthread_mutex_unlock(&g_mem_lock);
    memset(g_mem_base + p, 0, size);
    return p;
}

uint32_t guest_alloc_stack(uint32_t size) {
    size = (size + 0xFFFu) & ~0xFFFu;
    pthread_mutex_lock(&g_mem_lock);
    uint32_t base = g_stack_bump + 0x10000u; /* 64 KB unmapped guard below each stack */
    if (base + size > g_stack_end) {
        pthread_mutex_unlock(&g_mem_lock);
        fprintf(stderr, "[mem] Stack arena exhausted!\n");
        return 0;
    }
    g_stack_bump = base + size;
    pthread_mutex_unlock(&g_mem_lock);
    if (!recomp_mem_commit(base, size)) return 0;
    return base + size; /* Returns initial ESP (top of stack) */
}

/* ---------------- 16-Byte Aligned Segregated Guest Heap Allocator ---------------- */
/* Heap range: 0x10000000 .. 0x3F000000 (752 MB virtual inside 1 GB arena, 16 MB slabs) */

#define HEAP_START 0x10000000u
#define HEAP_LIMIT 0x3F000000u
#define SLAB_SIZE  0x01000000u /* 16 MB per slab */
#define NUM_BINS   32

/* Block layout in guest memory (16-byte header, user payload is always 16-byte aligned):
 * +0x00: uint32_t size       (total block size including 16B header; bit 0 = allocated, bit 1 = prev_free)
 * +0x04: uint32_t prev_size  (valid if prev_free bit is set, or stored at footer of free block)
 * +0x08: uint32_t next_free  (guest ptr to block header, or 0)
 * +0x0c: uint32_t prev_free  (guest ptr to block header, or 0)
 * Free blocks also store their size in the last 4 bytes [blk + size - 4] for O(1) backward coalesce.
 */
#define BLK_ALLOC_BIT 1u
#define BLK_SIZE_MASK (~15u)
#define HDR_SIZE      16u

static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_brk = HEAP_START;
static uint32_t g_bins[NUM_BINS];
static bool g_heap_inited = false;

static inline uint32_t blk_get_size(uint32_t blk) {
    return mem_r32(blk) & BLK_SIZE_MASK;
}

static inline bool blk_is_alloc(uint32_t blk) {
    return (mem_r32(blk) & BLK_ALLOC_BIT) != 0;
}

static inline int size_to_bin(uint32_t sz) {
    if (sz <= 256u) return (int)((sz >> 4) - 1u); /* bins 0..15: exact 16..256 */
    int b = 16;
    uint32_t v = sz >> 8;
    while (v > 1u && b < NUM_BINS - 1) {
        v >>= 1;
        b++;
    }
    return b;
}

static void bin_insert(uint32_t blk, uint32_t sz) {
    int b = size_to_bin(sz);
    uint32_t head = g_bins[b];
    mem_w32(blk, sz); /* alloc bit = 0 */
    mem_w32(blk + 8, head);
    mem_w32(blk + 12, 0);
    if (head) {
        mem_w32(head + 12, blk);
    }
    g_bins[b] = blk;
    /* Write footer size at end of free block */
    mem_w32(blk + sz - 4, sz);
}

static void bin_remove(uint32_t blk, uint32_t sz) {
    int b = size_to_bin(sz);
    uint32_t nxt = mem_r32(blk + 8);
    uint32_t prv = mem_r32(blk + 12);
    if (prv) {
        mem_w32(prv + 8, nxt);
    } else {
        g_bins[b] = nxt;
    }
    if (nxt) {
        mem_w32(nxt + 12, prv);
    }
}

static bool heap_grow(uint32_t min_bytes) {
    uint32_t grow = (min_bytes + SLAB_SIZE - 1u) & ~(SLAB_SIZE - 1u);
    if ((uint64_t)g_heap_brk + (uint64_t)grow > (uint64_t)HEAP_LIMIT) {
        grow = (min_bytes + 0xFFFu) & ~0xFFFu;
        if ((uint64_t)g_heap_brk + (uint64_t)grow > (uint64_t)HEAP_LIMIT) return false;
    }
    uint32_t old_brk = g_heap_brk;
    if (!recomp_mem_commit(old_brk, grow)) return false;
    g_heap_brk = old_brk + grow;

    /* Reserve last 16 bytes of committed span as a sentinel if first time, or coalesce with last free block */
    uint32_t blk = old_brk;
    uint32_t sz = grow;
    if (old_brk == HEAP_START) {
        /* Place 16-byte sentinel at start [HEAP_START] so blk > HEAP_START always has valid prev boundary */
        mem_w32(HEAP_START, 16u | BLK_ALLOC_BIT);
        blk += 16u;
        sz -= 16u;
    } else {
        /* Remove trailing sentinel at old_brk - 16 */
        blk = old_brk - 16u;
        sz = grow + 16u;
    }
    /* Leave 16-byte sentinel at g_heap_brk - 16 */
    sz -= 16u;
    mem_w32(g_heap_brk - 16u, 16u | BLK_ALLOC_BIT);

    /* Check if block immediately before blk is free and coalesce (avoid 32-bit wrap) */
    if (blk > HEAP_START + 16u) {
        uint32_t prev_sz = mem_r32(blk - 4u);
        if (prev_sz >= 32u && (prev_sz & 15u) == 0 && prev_sz <= blk - (HEAP_START + 16u)) {
            uint32_t prev_blk = blk - prev_sz;
            if (prev_blk < g_heap_brk && !blk_is_alloc(prev_blk) && blk_get_size(prev_blk) == prev_sz) {
                bin_remove(prev_blk, prev_sz);
                blk = prev_blk;
                sz += prev_sz;
            }
        }
    }
    bin_insert(blk, sz);
    return true;
}

uint32_t guest_malloc(uint32_t size) {
    if (size == 0) size = 16u;
    if (size > 0x3FFFFFFFu) return 0;
    uint32_t req = ((size + 15u) & ~15u) + HDR_SIZE;
    if (req < 32u) req = 32u;

    pthread_mutex_lock(&g_heap_lock);
    if (!g_heap_inited) {
        memset(g_bins, 0, sizeof(g_bins));
        g_heap_inited = true;
        if (!heap_grow(SLAB_SIZE)) {
            pthread_mutex_unlock(&g_heap_lock);
            return 0;
        }
    }

    int start_bin = size_to_bin(req);
    uint32_t found = 0;
    uint32_t found_sz = 0;

    for (int b = start_bin; b < NUM_BINS; b++) {
        uint32_t cur = g_bins[b];
        int steps = 0;
        while (cur && steps < 64) {
            uint32_t csz = blk_get_size(cur);
            if (csz >= req) {
                found = cur;
                found_sz = csz;
                break;
            }
            cur = mem_r32(cur + 8);
            steps++;
        }
        if (found) break;
    }

    if (!found) {
        /* Full scan in start_bin if steps capped, else grow */
        if (!heap_grow(req + 64u)) {
            pthread_mutex_unlock(&g_heap_lock);
            return 0;
        }
        for (int b = start_bin; b < NUM_BINS; b++) {
            uint32_t cur = g_bins[b];
            while (cur) {
                uint32_t csz = blk_get_size(cur);
                if (csz >= req) {
                    found = cur;
                    found_sz = csz;
                    break;
                }
                cur = mem_r32(cur + 8);
            }
            if (found) break;
        }
    }

    if (!found) {
        pthread_mutex_unlock(&g_heap_lock);
        return 0;
    }

    bin_remove(found, found_sz);
    uint32_t rem = found_sz - req;
    if (rem >= 32u) {
        mem_w32(found, req | BLK_ALLOC_BIT);
        mem_w32(found + req - 4u, 0); /* clear footer marker since allocated */
        bin_insert(found + req, rem);
    } else {
        mem_w32(found, found_sz | BLK_ALLOC_BIT);
        mem_w32(found + found_sz - 4u, 0);
    }
    pthread_mutex_unlock(&g_heap_lock);
    return found + HDR_SIZE;
}

void guest_free(uint32_t ptr) {
    if (!ptr) return;
    if (ptr < HEAP_START + 16u || ptr >= g_heap_brk) {
        /* Ignore pointers outside heap (e.g. static ELF addresses or invalid frees) */
        return;
    }
    pthread_mutex_lock(&g_heap_lock);
    uint32_t blk = ptr - HDR_SIZE;
    uint32_t header = mem_r32(blk);
    if (!(header & BLK_ALLOC_BIT)) {
        pthread_mutex_unlock(&g_heap_lock);
        return;
    }
    uint32_t sz = header & BLK_SIZE_MASK;

    /* Coalesce forward */
    uint32_t next_blk = blk + sz;
    if (next_blk < g_heap_brk - 16u && !blk_is_alloc(next_blk)) {
        uint32_t nsz = blk_get_size(next_blk);
        bin_remove(next_blk, nsz);
        sz += nsz;
    }

    /* Coalesce backward (subtraction bounds prevent 32-bit unsigned wrap) */
    if (blk > HEAP_START + 16u) {
        uint32_t prev_sz = mem_r32(blk - 4u);
        if (prev_sz >= 32u && (prev_sz & 15u) == 0 && prev_sz <= blk - (HEAP_START + 16u)) {
            uint32_t prev_blk = blk - prev_sz;
            if (prev_blk < g_heap_brk && !blk_is_alloc(prev_blk) && blk_get_size(prev_blk) == prev_sz) {
                bin_remove(prev_blk, prev_sz);
                blk = prev_blk;
                sz += prev_sz;
            }
        }
    }

    bin_insert(blk, sz);
    pthread_mutex_unlock(&g_heap_lock);
}

uint32_t guest_calloc(uint32_t nmemb, uint32_t size) {
    uint64_t tot = (uint64_t)nmemb * (uint64_t)size;
    if (tot > 0x3FFFFFFFu) return 0;
    uint32_t p = guest_malloc((uint32_t)tot);
    if (p && tot) memset(g_mem_base + p, 0, (size_t)tot);
    return p;
}

uint32_t guest_realloc(uint32_t ptr, uint32_t size) {
    if (!ptr) return guest_malloc(size);
    if (size == 0) {
        guest_free(ptr);
        return 0;
    }
    uint32_t old_blk = ptr - HDR_SIZE;
    uint32_t old_payload = blk_get_size(old_blk) - HDR_SIZE;
    if (size <= old_payload) return ptr;

    uint32_t np = guest_malloc(size);
    if (!np) return 0;
    memcpy(g_mem_base + np, g_mem_base + ptr, old_payload);
    guest_free(ptr);
    return np;
}

uint32_t guest_strdup(const char* s) {
    if (!s) return 0;
    size_t len = strlen(s) + 1;
    uint32_t p = guest_malloc((uint32_t)len);
    if (p) memcpy(g_mem_base + p, s, len);
    return p;
}

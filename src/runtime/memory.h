#ifndef RECOMP_MEMORY_H
#define RECOMP_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* g_mem_base;

bool recomp_mem_init(void);
void recomp_mem_shutdown(void);
bool recomp_mem_commit(uint32_t guest_addr, uint32_t size);
bool recomp_load_elf(const char* elf_path);

uint32_t guest_rt_alloc(uint32_t size, uint32_t align);
uint32_t guest_alloc_stack(uint32_t size);

uint32_t guest_malloc(uint32_t size);
uint32_t guest_calloc(uint32_t nmemb, uint32_t size);
uint32_t guest_realloc(uint32_t ptr, uint32_t size);
void     guest_free(uint32_t ptr);
uint32_t guest_strdup(const char* s);

const char* recomp_get_game_dir(void);
void recomp_set_game_dir(const char* dir);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_MEMORY_H */

#ifndef RECOMP_VFS_H
#define RECOMP_VFS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void vfs_init(const char* game_dir);
bool vfs_resolve_path(const char* guest_path, char* out_host_path, size_t out_sz, bool for_write);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_VFS_H */

#define _GNU_SOURCE
#include "runtime/cpu.h"
#include "runtime/launcher_ui.h"
#include "runtime/memory.h"
#include "runtime/settings.h"
#include "runtime/vfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

static inline uint32_t arg32(CPUContext* ctx, int idx) {
    return mem_r32(ctx->esp + (uint32_t)(4 * (idx + 1)));
}

static inline double argf64(CPUContext* ctx, int byte_off) {
    return mem_rf64(ctx->esp + (uint32_t)(4 + byte_off));
}

static inline const char* cstr32(uint32_t p) {
    return p ? (const char*)(g_mem_base + p) : NULL;
}

static inline char* mstr32(uint32_t p) {
    return p ? (char*)(g_mem_base + p) : NULL;
}

/* ---------------- Case-Insensitive VFS ---------------- */

static char g_vfs_root[1024] = ".";

static void mkdir_p(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            if (p > tmp && p[-1] == ':') continue; /* keep "sdmc:" intact */
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

#ifdef __SWITCH__
#define NET_VFS_PORT 8769

typedef struct NetManifestEntry {
    char* rel_lower;
    char* rel_canon;
} NetManifestEntry;

static NetManifestEntry* g_net_manifest = NULL;
static size_t g_net_manifest_count = 0;
static const char* g_net_vfs_host;
static bool g_net_vfs_offline = false;
static pthread_mutex_t g_net_vfs_lock = PTHREAD_MUTEX_INITIALIZER;

static int net_manifest_cmp(const void* a, const void* b) {
    const NetManifestEntry* ea = (const NetManifestEntry*)a;
    const NetManifestEntry* eb = (const NetManifestEntry*)b;
    return strcmp(ea->rel_lower, eb->rel_lower);
}

static const char* net_manifest_lookup(const char* rel_path) {
    if (!g_net_manifest || g_net_manifest_count == 0 || !rel_path) return NULL;
    char key[1024];
    size_t i = 0;
    for (; rel_path[i] && i + 1 < sizeof(key); i++) {
        key[i] = (char)tolower((unsigned char)rel_path[i]);
    }
    key[i] = '\0';
    NetManifestEntry target = { key, NULL };
    NetManifestEntry* hit = (NetManifestEntry*)bsearch(
        &target, g_net_manifest, g_net_manifest_count,
        sizeof(NetManifestEntry), net_manifest_cmp);
    return hit ? hit->rel_canon : NULL;
}

static int net_connect_host(int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(NET_VFS_PORT);
    sa.sin_addr.s_addr = inet_addr(g_net_vfs_host);

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sr = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sr <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval rcv_tv = { 60, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    return fd;
}

static char* net_http_get_mem(const char* url_path, size_t* out_len) {
    if (out_len) *out_len = 0;
    int fd = net_connect_host(1500);
    if (fd < 0) return NULL;
    char req[1280];
    int rlen = snprintf(req, sizeof(req),
                        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                        url_path, g_net_vfs_host, NET_VFS_PORT);
    size_t sent = 0;
    while (sent < (size_t)rlen) {
        ssize_t w = send(fd, req + sent, (size_t)rlen - sent, 0);
        if (w <= 0) { close(fd); return NULL; }
        sent += (size_t)w;
    }
    size_t cap = 65536, total = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (total + 16384 >= cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        ssize_t r = recv(fd, buf + total, cap - total - 1, 0);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fd);
    buf[total] = '\0';
    char* hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end || strncmp(buf, "HTTP/1.", 7) != 0 || !strstr(buf, " 200 ")) {
        free(buf);
        return NULL;
    }
    hdr_end += 4;
    size_t body_len = total - (size_t)(hdr_end - buf);
    memmove(buf, hdr_end, body_len);
    buf[body_len] = '\0';
    if (out_len) *out_len = body_len;
    return buf;
}

static bool net_http_download_file(const char* rel_canon, char* out_host_path, size_t out_sz) {
    if (g_net_vfs_offline || !rel_canon || !*rel_canon) return false;
    char dst[1024];
    snprintf(dst, sizeof(dst), "%s/%s", g_vfs_root, rel_canon);
    struct stat st;
    if (stat(dst, &st) == 0 && st.st_size > 0) {
        if (out_host_path) snprintf(out_host_path, out_sz, "%s", dst);
        return true;
    }

    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", dst);
    char* sl = strrchr(parent, '/');
    if (sl) { *sl = '\0'; mkdir_p(parent); }

    char enc[1536];
    size_t ei = 0;
    enc[ei++] = '/';
    for (size_t i = 0; rel_canon[i] && ei + 4 < sizeof(enc); i++) {
        unsigned char c = (unsigned char)rel_canon[i];
        if (isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '~') {
            enc[ei++] = (char)c;
        } else {
            snprintf(enc + ei, 4, "%%%02X", c);
            ei += 3;
        }
    }
    enc[ei] = '\0';

    int fd = net_connect_host(2000);
    if (fd < 0) {
        g_net_vfs_offline = true;
        fprintf(stderr, "[net-vfs] Host unreachable, switching to offline SD mode.\n");
        return false;
    }
    char req[2048];
    int rlen = snprintf(req, sizeof(req),
                        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                        enc, g_net_vfs_host, NET_VFS_PORT);
    size_t sent = 0;
    while (sent < (size_t)rlen) {
        ssize_t w = send(fd, req + sent, (size_t)rlen - sent, 0);
        if (w <= 0) { close(fd); return false; }
        sent += (size_t)w;
    }

    char hdr[4096];
    size_t hlen = 0;
    char* hdr_end = NULL;
    while (hlen + 1 < sizeof(hdr)) {
        ssize_t r = recv(fd, hdr + hlen, 1, 0);
        if (r <= 0) break;
        hlen += (size_t)r;
        hdr[hlen] = '\0';
        if (hlen >= 4 && strcmp(hdr + hlen - 4, "\r\n\r\n") == 0) {
            hdr_end = hdr + hlen;
            break;
        }
    }
    if (!hdr_end || !strstr(hdr, " 200 ")) {
        close(fd);
        return false;
    }

    long long content_len = -1;
    char* clp = strcasestr(hdr, "\nContent-Length:");
    if (clp) content_len = atoll(clp + 16);

    char part[1088];
    snprintf(part, sizeof(part), "%s.part", dst);
    fprintf(stderr, "[net-vfs] Downloading %s (%lld bytes)...\n", rel_canon, content_len);
    FILE* fp = fopen(part, "wb");
    if (!fp) { close(fd); return false; }

    size_t io_buf_sz = 262144;
    char* io_buf = (char*)malloc(io_buf_sz);
    if (!io_buf) {
        fclose(fp);
        remove(part);
        close(fd);
        return false;
    }
    setvbuf(fp, NULL, _IOFBF, io_buf_sz);

    long long written = 0;
    for (;;) {
        ssize_t r = recv(fd, io_buf, io_buf_sz, 0);
        if (r <= 0) break;
        if (fwrite(io_buf, 1, (size_t)r, fp) != (size_t)r) {
            free(io_buf);
            fclose(fp);
            remove(part);
            close(fd);
            return false;
        }
        written += r;
    }
    free(io_buf);
    fclose(fp);
    close(fd);

    if (content_len >= 0 && written != content_len) {
        remove(part);
        return false;
    }
    remove(dst);
    if (rename(part, dst) != 0) {
        remove(part);
        return false;
    }
    fprintf(stderr, "[net-vfs] Cached %s (%lld bytes)\n", rel_canon, written);
    if (out_host_path) snprintf(out_host_path, out_sz, "%s", dst);
    return true;
}

static void switch_net_init_manifest_and_bootstrap(void) {
    const char* enable_net = getenv("RECOMP_NET_VFS");
    g_net_vfs_host = getenv("RECOMP_NET_VFS_HOST");
    if (!enable_net || strcmp(enable_net, "1") != 0 || !g_net_vfs_host || !*g_net_vfs_host) {
        g_net_vfs_offline = true;
        fprintf(stderr, "[net-vfs] Standalone offline SD mode (set RECOMP_NET_VFS=1 and RECOMP_NET_VFS_HOST to enable LAN cache)\n");
        return;
    }
    size_t mlen = 0;
    char* mdata = net_http_get_mem("/__manifest.txt", &mlen);
    if (!mdata) {
        g_net_vfs_offline = true;
        fprintf(stderr, "[net-vfs] No host asset server at %s:%d (offline SD mode)\n",
                g_net_vfs_host, NET_VFS_PORT);
        return;
    }
    size_t cap = 4096;
    g_net_manifest = (NetManifestEntry*)malloc(cap * sizeof(NetManifestEntry));
    char* line = mdata;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') line[--ll] = '\0';
        if (ll > 0) {
            if (g_net_manifest_count >= cap) {
                cap *= 2;
                g_net_manifest = (NetManifestEntry*)realloc(
                    g_net_manifest, cap * sizeof(NetManifestEntry));
            }
            char* canon = strdup(line);
            char* lower = strdup(line);
            for (size_t k = 0; lower[k]; k++) lower[k] = (char)tolower((unsigned char)lower[k]);
            g_net_manifest[g_net_manifest_count++] = (NetManifestEntry){ lower, canon };
        }
        line = nl ? (nl + 1) : NULL;
    }
    free(mdata);
    qsort(g_net_manifest, g_net_manifest_count, sizeof(NetManifestEntry), net_manifest_cmp);
    fprintf(stderr, "[net-vfs] Loaded host manifest (%zu assets)\n", g_net_manifest_count);

    char elf_marker[1024];
    snprintf(elf_marker, sizeof(elf_marker), "%s/Psychonauts", g_vfs_root);
    struct stat st;
    if (stat(elf_marker, &st) != 0) {
        net_http_download_file("Psychonauts", NULL, 0);
    }

    char marker[1024];
    snprintf(marker, sizeof(marker), "%s/WorkResource/Scripts/packfile/common.lpf", g_vfs_root);
    if (stat(marker, &st) == 0) return;

    size_t blen = 0;
    char* bdata = net_http_get_mem("/__bootstrap.txt", &blen);
    if (!bdata) return;
    int downloaded = 0;
    char* bl = bdata;
    while (bl && *bl) {
        char* nl = strchr(bl, '\n');
        if (nl) *nl = '\0';
        size_t ll = strlen(bl);
        if (ll > 0 && bl[ll - 1] == '\r') bl[--ll] = '\0';
        if (ll > 0) {
            if (net_http_download_file(bl, NULL, 0)) downloaded++;
        }
        bl = nl ? (nl + 1) : NULL;
    }
    free(bdata);
    fprintf(stderr, "[net-vfs] Bootstrap complete (%d files synced)\n", downloaded);
}

static bool switch_net_fetch_asset(const char* rel_path, char* out_host_path, size_t out_sz) {
    if (g_net_vfs_offline || !rel_path || !*rel_path) return false;
    if (strstr(rel_path, ".local/") || strstr(rel_path, "SAVEDGAMES")) return false;
    pthread_mutex_lock(&g_net_vfs_lock);
    const char* canon = net_manifest_lookup(rel_path);
    bool ok = false;
    if (canon) {
        ok = net_http_download_file(canon, out_host_path, out_sz);
    }
    pthread_mutex_unlock(&g_net_vfs_lock);
    return ok;
}
#endif

static void write_display_ini_file(const char* path, int target_w, int target_h) {
    if (target_w <= 0) target_w = 1280;
    if (target_h <= 0) target_h = 720;

    char want_w[64], want_h[64];
    snprintf(want_w, sizeof(want_w), "ScreenWidth=%d", target_w);
    snprintf(want_h, sizeof(want_h), "ScreenHeight=%d", target_h);

    FILE* rf = fopen(path, "rb");
    if (rf) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        buf[n] = '\0';
        fclose(rf);

        if (strstr(buf, want_w) != NULL && strstr(buf, want_h) != NULL) {
            return;
        }
        /* Rewrite existing ini updating ScreenWidth and ScreenHeight */
        FILE* wf = fopen(path, "wb");
        if (wf) {
            bool wrote_w = false, wrote_h = false;
            char* save = NULL;
            char* line = strtok_r(buf, "\n", &save);
            while (line) {
                size_t ll = strlen(line);
                while (ll > 0 && line[ll - 1] == '\r') line[--ll] = '\0';
                if (strncmp(line, "ScreenWidth=", 12) == 0) {
                    fprintf(wf, "%s\n", want_w);
                    wrote_w = true;
                } else if (strncmp(line, "ScreenHeight=", 13) == 0) {
                    fprintf(wf, "%s\n", want_h);
                    wrote_h = true;
#ifdef __SWITCH__
                } else if (strncmp(line, "FSAA=", 5) == 0) {
                    fprintf(wf, "FSAA=false\n");
                } else if (strncmp(line, "FullScreen=", 11) == 0) {
                    fprintf(wf, "FullScreen=true\n");
#endif
                } else {
                    fprintf(wf, "%s\n", line);
                }
                line = strtok_r(NULL, "\n", &save);
            }
            if (!wrote_w) fprintf(wf, "%s\n", want_w);
            if (!wrote_h) fprintf(wf, "%s\n", want_h);
            fclose(wf);
            fprintf(stderr, "[vfs] Updated DisplaySettings.ini (%dx%d) -> %s\n", target_w, target_h, path);
            return;
        }
    }

    FILE* wf = fopen(path, "wb");
    if (!wf) return;
#ifdef __SWITCH__
    const char* fs = "true";
#else
    const char* fs = "false";
#endif
    fprintf(wf,
            "[DisplaySettings]\n"
            "ScreenWidth=%d\n"
            "ScreenHeight=%d\n"
            "FullScreen=%s\n"
            "VSync=true\n"
            "FSAA=false\n"
            "FSFX=true\n"
            "AdvancedShading=true\n"
            "Shadows=true\n"
            "GammaCorrection=1.000000\n",
            target_w, target_h, fs);
    fclose(wf);
    fprintf(stderr, "[vfs] Wrote %dx%d DisplaySettings.ini -> %s\n", target_w, target_h, path);
}

void recomp_sync_display_settings_ini(const char* game_dir, int target_w, int target_h) {
    const char* dir = (game_dir && *game_dir) ? game_dir : ".";
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/.local/share/Psychonauts", dir);
    mkdir_p(sub);
    snprintf(sub, sizeof(sub), "%s/DisplaySettings.ini", dir);
    write_display_ini_file(sub, target_w, target_h);
    snprintf(sub, sizeof(sub), "%s/.local/share/Psychonauts/DisplaySettings.ini", dir);
    write_display_ini_file(sub, target_w, target_h);
}

static void ensure_audio_settings_ini(const char* path, bool subtitles) {
    FILE* rf = fopen(path, "rb");
    if (rf) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        buf[n] = '\0';
        fclose(rf);
        const char* want = subtitles ? "ShowSubtitles=true" : "ShowSubtitles=false";
        if (strstr(buf, want) != NULL) return;
        /* Rewrite ShowSubtitles line in place if file already exists */
        const char* pos = strstr(buf, "ShowSubtitles=");
        if (pos) {
            const char* eol = strchr(pos, '\n');
            FILE* wf = fopen(path, "wb");
            if (wf) {
                fwrite(buf, 1, (size_t)(pos - buf), wf);
                fprintf(wf, "%s", want);
                if (eol) {
                    if (pos < eol && eol[-1] == '\r') fputc('\r', wf);
                    fwrite(eol, 1, strlen(eol), wf);
                } else {
                    fputc('\n', wf);
                }
                fclose(wf);
            }
            return;
        }
    }
    FILE* wf = fopen(path, "wb");
    if (!wf) return;
    fprintf(wf,
            "[AudioSettings]\n"
            "MasterVolume=1.000000\n"
            "FXVolume=1.000000\n"
            "MusicVolume=1.000000\n"
            "VoiceVolume=1.000000\n"
            "ShowSubtitles=%s\n"
            "HardwareAccel=true\n"
            "UseEAX=false\n"
            "PacketSize=250\n"
            "MusicPacketSize=250\n"
            "PreloadVoice=false\n",
            subtitles ? "true" : "false");
    fclose(wf);
}

static void ensure_savegame_prefs_ini(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) return;
    FILE* wf = fopen(path, "wb");
    if (!wf) return;
    fprintf(wf,
            "-- Savegame directory ignore list\n"
            "player.stats\n"
            "saved\n");
    fclose(wf);
}

void vfs_init(const char* game_dir) {
    if (game_dir && *game_dir) {
        snprintf(g_vfs_root, sizeof(g_vfs_root), "%s", game_dir);
    }
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/.local/share/Psychonauts", g_vfs_root);
    mkdir_p(sub);
    snprintf(sub, sizeof(sub), "%s/WorkResource/SAVEDGAMES", g_vfs_root);
    mkdir_p(sub);
    snprintf(sub, sizeof(sub), "%s/WorkResource/SAVEDGAMES/savegameprefs.ini", g_vfs_root);
    ensure_savegame_prefs_ini(sub);
    const RecompSettings* st = recomp_settings_get();
#ifdef __SWITCH__
    setenv("HOME", g_vfs_root, 1);
    switch_net_init_manifest_and_bootstrap();
#else
    /* Keep host Xvfb verification clean under game_dir HOME if needed, never breaking user's HOME */
#endif
    int res_w = 1280, res_h = 720;
    recomp_resolve_target_resolution(&res_w, &res_h);
    recomp_sync_display_settings_ini(g_vfs_root, res_w, res_h);
    snprintf(sub, sizeof(sub), "%s/AudioSettings.ini", g_vfs_root);
    ensure_audio_settings_ini(sub, st->subtitles);
    snprintf(sub, sizeof(sub), "%s/.local/share/Psychonauts/AudioSettings.ini", g_vfs_root);
    ensure_audio_settings_ini(sub, st->subtitles);
    if (chdir(g_vfs_root) != 0) {
        fprintf(stderr, "[vfs] Warning: chdir(%s) failed: %s\n", g_vfs_root, strerror(errno));
    }
}

static bool find_child_ci(const char* dir_path, const char* target, char* out_name, size_t out_sz) {
    struct stat st;
    char probe[1024];
    if (strcmp(dir_path, "/") == 0) snprintf(probe, sizeof(probe), "/%s", target);
    else if (strcmp(dir_path, "sdmc:") == 0) snprintf(probe, sizeof(probe), "sdmc:/%s", target);
    else snprintf(probe, sizeof(probe), "%s/%s", dir_path, target);
    if (stat(probe, &st) == 0) {
        snprintf(out_name, out_sz, "%s", target);
        return true;
    }
    const char* open_dir = (strcmp(dir_path, "sdmc:") == 0) ? "sdmc:/" : dir_path;
    DIR* d = opendir(open_dir);
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcasecmp(ent->d_name, target) == 0) {
            snprintf(out_name, out_sz, "%s", ent->d_name);
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

static bool vfs_resolve_raw(const char* guest_path, char* out_host_path, size_t out_sz, bool for_write) {
    if (!guest_path || !*guest_path) return false;
    char norm[1024];
    size_t ni = 0;
    for (size_t i = 0; guest_path[i] && ni + 1 < sizeof(norm); i++) {
        char ch = (guest_path[i] == '\\') ? '/' : guest_path[i];
        if (ch == '/' && ni > 0 && norm[ni - 1] == '/') continue;
        norm[ni++] = ch;
    }
    norm[ni] = '\0';

    /* Strip leading ./ */
    const char* p = norm;
    while (p[0] == '.' && p[1] == '/') p += 2;

    char cur[1024];
    const char* net_rel = NULL;
    if (strcmp(p, "sdmc:") == 0 || strcmp(p, "sdmc:/") == 0) {
        snprintf(out_host_path, out_sz, "sdmc:/");
        return true;
    }
    if (strncmp(p, "sdmc:/", 6) == 0) {
        struct stat st;
        if (!for_write && stat(p, &st) == 0) {
            snprintf(out_host_path, out_sz, "%s", p);
            return true;
        }
        size_t rl = strlen(g_vfs_root);
        if (strncmp(p, g_vfs_root, rl) == 0 && p[rl] == '/') {
            net_rel = p + rl + 1;
        }
        snprintf(cur, sizeof(cur), "sdmc:");
        p += 6;
    } else if (p[0] == '/') {
        /* If absolute path already exists, return as-is */
        struct stat st;
        if (!for_write && stat(p, &st) == 0) {
            snprintf(out_host_path, out_sz, "%s", p);
            return true;
        }
        size_t rl = strlen(g_vfs_root);
        if (strncmp(p, g_vfs_root, rl) == 0 && p[rl] == '/') {
            net_rel = p + rl + 1;
        }
        snprintf(cur, sizeof(cur), "/");
        p++;
    } else {
        net_rel = p;
        snprintf(cur, sizeof(cur), "%s", g_vfs_root);
    }
#ifndef __SWITCH__
    (void)net_rel;
#endif

    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", p);
    char* save = NULL;
    char* tok = strtok_r(copy, "/", &save);
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            tok = strtok_r(NULL, "/", &save);
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            size_t cl = strlen(cur);
            if (cl + 4 < sizeof(cur)) {
                strcat(cur, "/..");
            }
            tok = strtok_r(NULL, "/", &save);
            continue;
        }
        char matched[256];
        if (find_child_ci(cur, tok, matched, sizeof(matched))) {
            char next[1024];
            if (strcmp(cur, "/") == 0) snprintf(next, sizeof(next), "/%s", matched);
            else snprintf(next, sizeof(next), "%s/%s", cur, matched);
            snprintf(cur, sizeof(cur), "%s", next);
        } else if (for_write) {
            char* next_tok = strtok_r(NULL, "/", &save);
            char next[1024];
            if (strcmp(cur, "/") == 0) snprintf(next, sizeof(next), "/%s", tok);
            else snprintf(next, sizeof(next), "%s/%s", cur, tok);
            snprintf(cur, sizeof(cur), "%s", next);
            if (!next_tok) {
                snprintf(out_host_path, out_sz, "%s", cur);
                return true;
            }
            /* Append remainder for write */
            tok = next_tok;
            continue;
        } else {
#ifdef __SWITCH__
            if (net_rel && switch_net_fetch_asset(net_rel, out_host_path, out_sz)) {
                return true;
            }
#endif
            return false;
        }
        tok = strtok_r(NULL, "/", &save);
    }

    snprintf(out_host_path, out_sz, "%s", cur);
    return true;
}

static bool vfs_try_override(const char* rel_workres, char* out_host_path, size_t out_sz) {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", g_vfs_root, rel_workres);
    return vfs_resolve_raw(full, out_host_path, out_sz, false);
}

#define VFS_CACHE_SIZE 2048u
#define VFS_CACHE_PROBE 4u
typedef struct VfsCacheEntry {
    uint32_t hash;
    uint32_t gen;
    int text_lang;
    int voice_lang;
    int button_icons;
    bool found;
    char guest_path[256];
    char host_path[512];
} VfsCacheEntry;

static VfsCacheEntry g_vfs_cache[VFS_CACHE_SIZE];
static uint32_t g_vfs_cache_gen = 1u;
static pthread_mutex_t g_vfs_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static inline void vfs_cache_invalidate(void) {
    pthread_mutex_lock(&g_vfs_cache_lock);
    g_vfs_cache_gen++;
    if (g_vfs_cache_gen == 0u) g_vfs_cache_gen = 1u;
    pthread_mutex_unlock(&g_vfs_cache_lock);
}

static uint32_t vfs_hash_str(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static bool vfs_resolve_path_uncached(const char* guest_path, char* out_host_path, size_t out_sz, bool for_write);

bool vfs_resolve_path(const char* guest_path, char* out_host_path, size_t out_sz, bool for_write) {
    if (!guest_path || !*guest_path) return false;
    if (for_write) {
        vfs_cache_invalidate();
        return vfs_resolve_raw(guest_path, out_host_path, out_sz, true);
    }
    size_t gplen = strlen(guest_path);
    const RecompSettings* st = recomp_settings_get();
    uint32_t h = 0;
    uint32_t slot = 0;
    uint32_t cur_gen = 0;
    if (gplen < 256) {
        h = vfs_hash_str(guest_path);
        slot = h & (VFS_CACHE_SIZE - 1u);
        pthread_mutex_lock(&g_vfs_cache_lock);
        cur_gen = g_vfs_cache_gen;
        for (uint32_t probe = 0; probe < VFS_CACHE_PROBE; ++probe) {
            VfsCacheEntry* e = &g_vfs_cache[(slot + probe) & (VFS_CACHE_SIZE - 1u)];
            if (e->hash == h &&
                e->gen == cur_gen &&
                e->text_lang == (int)st->text_lang &&
                e->voice_lang == (int)st->voice_lang &&
                e->button_icons == (int)st->button_icons &&
                strcmp(e->guest_path, guest_path) == 0) {
                bool found = e->found;
                if (found) snprintf(out_host_path, out_sz, "%s", e->host_path);
                pthread_mutex_unlock(&g_vfs_cache_lock);
                return found;
            }
        }
        pthread_mutex_unlock(&g_vfs_cache_lock);
    }
    bool ok = vfs_resolve_path_uncached(guest_path, out_host_path, out_sz, false);
    if (h != 0 && (!ok || strlen(out_host_path) < 512)) {
        pthread_mutex_lock(&g_vfs_cache_lock);
        if (g_vfs_cache_gen == cur_gen) {
            VfsCacheEntry* dst = &g_vfs_cache[slot];
            for (uint32_t probe = 0; probe < VFS_CACHE_PROBE; ++probe) {
                VfsCacheEntry* cand = &g_vfs_cache[(slot + probe) & (VFS_CACHE_SIZE - 1u)];
                if (cand->gen != cur_gen || cand->hash == 0u ||
                    (cand->hash == h && strcmp(cand->guest_path, guest_path) == 0)) {
                    dst = cand;
                    break;
                }
            }
            dst->hash = h;
            dst->gen = cur_gen;
            dst->text_lang = (int)st->text_lang;
            dst->voice_lang = (int)st->voice_lang;
            dst->button_icons = (int)st->button_icons;
            dst->found = ok;
            snprintf(dst->guest_path, sizeof(dst->guest_path), "%s", guest_path);
            if (ok) snprintf(dst->host_path, sizeof(dst->host_path), "%s", out_host_path);
            else dst->host_path[0] = '\0';
        }
        pthread_mutex_unlock(&g_vfs_cache_lock);
    }
    return ok;
}

static bool vfs_resolve_path_uncached(const char* guest_path, char* out_host_path, size_t out_sz, bool for_write) {
    if (!guest_path || !*guest_path) return false;
    if (for_write) {
        return vfs_resolve_raw(guest_path, out_host_path, out_sz, true);
    }

    char norm[1024];
    size_t ni = 0;
    for (size_t i = 0; guest_path[i] && ni + 1 < sizeof(norm); i++) {
        char ch = (guest_path[i] == '\\') ? '/' : guest_path[i];
        if (ch == '/' && ni > 0 && norm[ni - 1] == '/') continue;
        norm[ni++] = ch;
    }
    norm[ni] = '\0';

    const char* rel = norm;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    size_t rl = strlen(g_vfs_root);
    if (strncmp(rel, g_vfs_root, rl) == 0 && rel[rl] == '/') {
        rel += rl + 1;
    }

    /* Normalize subpath inside WorkResource/ if present, or bare resource dir */
    const char* wr = NULL;
    bool bare_wr_dir = false;
    if (strncasecmp(rel, "WorkResource/", 13) == 0) {
        wr = rel + 13;
    } else if (strncasecmp(rel, "Localization/", 13) == 0 ||
               strncasecmp(rel, "Fonts/", 6) == 0 ||
               strncasecmp(rel, "Sounds/", 7) == 0 ||
               strncasecmp(rel, "Cutscenes/", 10) == 0 ||
               strncasecmp(rel, "Textures/", 9) == 0 ||
               strncasecmp(rel, "PCLevelPackFiles/", 17) == 0) {
        wr = rel;
        bare_wr_dir = true;
    }

    if (wr) {
        const RecompSettings* st = recomp_settings_get();
        char alt[1024];

        /* 1. Controller Button Icons INI override */
        if (strcasecmp(wr, "Textures/Icons/UI_Icons/IconPrefsPC.ini") == 0 ||
            strcasecmp(wr, "Textures/icons/UI_Icons/IconPrefsPC.ini") == 0) {
            if (st->button_icons == RECOMP_ICONS_SWITCH &&
                vfs_try_override("WorkResource/Textures/Icons/UI_Icons/IconPrefsSwitch.ini", out_host_path, out_sz)) {
                return true;
            }
            if (st->button_icons == RECOMP_ICONS_XBOX &&
                vfs_try_override("WorkResource/Textures/Icons/UI_Icons/IconPrefsXbox.ini", out_host_path, out_sz)) {
                return true;
            }
        }

        /* 2. Russian Text Localization + Matching Cyrillic Fonts + common_rus.ppf */
        if (st->text_lang == RECOMP_TEXT_RU_PATCHED || st->text_lang == RECOMP_TEXT_RU_BUKA) {
            const char* loc_dir = (st->text_lang == RECOMP_TEXT_RU_PATCHED)
                                  ? "Localization/Russian_Patched"
                                  : "Localization/Russian_Buka";
            const char* fnt_dir = (st->text_lang == RECOMP_TEXT_RU_PATCHED)
                                  ? "Fonts_Russian_Patched"
                                  : "Fonts_Russian_Buka";
            if (strncasecmp(wr, "Localization/English/", 21) == 0) {
                snprintf(alt, sizeof(alt), "WorkResource/%s/%s", loc_dir, wr + 21);
                if (vfs_try_override(alt, out_host_path, out_sz)) return true;
            } else if (strncasecmp(wr, "Fonts/", 6) == 0) {
                snprintf(alt, sizeof(alt), "WorkResource/%s/%s", fnt_dir, wr + 6);
                if (vfs_try_override(alt, out_host_path, out_sz)) return true;
            } else if (strcasecmp(wr, "PCLevelPackFiles/common.ppf") == 0) {
                if (vfs_try_override("WorkResource/PCLevelPackFiles/common_rus.ppf", out_host_path, out_sz)) {
                    return true;
                }
            }
        }

        /* 3. Russian Voiceover & Prerendered Cutscenes (with transparent EN fallback) */
        if (st->voice_lang == RECOMP_VOICE_RU_BUKA) {
            if (strncasecmp(wr, "Sounds/", 7) == 0) {
                snprintf(alt, sizeof(alt), "WorkResource/Sounds_Russian/%s", wr + 7);
                if (vfs_try_override(alt, out_host_path, out_sz)) return true;
            } else if (strncasecmp(wr, "Cutscenes/", 10) == 0) {
                snprintf(alt, sizeof(alt), "WorkResource/cutscenes_Russian/%s", wr + 10);
                if (vfs_try_override(alt, out_host_path, out_sz)) return true;
            }
        }
    }

    if (vfs_resolve_raw(guest_path, out_host_path, out_sz, false)) {
        return true;
    }

    /* Fallback for stripped WorkResource\ paths (e.g. Textures/Icons/Switch_Icons/SW_B.dds) */
    if (bare_wr_dir && wr) {
        char alt[1024];
        snprintf(alt, sizeof(alt), "WorkResource/%s", wr);
        if (vfs_try_override(alt, out_host_path, out_sz)) return true;
    }
    return false;
}

/* ---------------- FILE* and DIR* Handle Tables ---------------- */

#define MAX_FILE_HANDLES 512
#define MAX_DIR_HANDLES  64

static FILE* g_files[MAX_FILE_HANDLES];
static DIR*  g_dirs[MAX_DIR_HANDLES];
static uint32_t g_dir_dirent_ptr[MAX_DIR_HANDLES];
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;

void recomp_init_stdio_handles(void) {
    g_files[1] = stdin;
    g_files[2] = stdout;
    g_files[3] = stderr;
    /* Copy relocations .rel.dyn in Psychonauts ELF32:
     * 0x0869bf74 = stdin, 0x0869bf40 = stdout, 0x0869bfd4 = stderr */
    mem_w32(0x0869BF74u, 0x087A0004u);
    mem_w32(0x0869BF40u, 0x087A0008u);
    mem_w32(0x0869BFD4u, 0x087A000Cu);
}

static uint32_t wrap_file(FILE* fp) {
    if (!fp) return 0;
    pthread_mutex_lock(&g_io_lock);
    for (int i = 4; i < MAX_FILE_HANDLES; i++) {
        if (!g_files[i]) {
            g_files[i] = fp;
            pthread_mutex_unlock(&g_io_lock);
            return 0x087A0000u + (uint32_t)(i * 4);
        }
    }
    pthread_mutex_unlock(&g_io_lock);
    return 0;
}

static FILE* unwrap_file(uint32_t h) {
    if (h < 0x087A0004u) return NULL;
    uint32_t idx = (h - 0x087A0000u) >> 2;
    if (idx >= MAX_FILE_HANDLES) return NULL;
    return g_files[idx];
}

static void unreg_file(uint32_t h) {
    if (h < 0x087A0010u) return;
    uint32_t idx = (h - 0x087A0000u) >> 2;
    if (idx < MAX_FILE_HANDLES) g_files[idx] = NULL;
}

/* ---------------- errno, ctype_b_loc, strerror, localtime ---------------- */

static pthread_mutex_t g_tls_misc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_ctype_loc_cell = 0;
static uint32_t g_tm_buf = 0;
static uint32_t g_tm_zone_str = 0;
static uint32_t g_err_str = 0;
static uint32_t g_env_str = 0;
static uint32_t g_getcwd_buf = 0;
static __thread uint32_t t_errno_guest_ptr = 0;

void imp___errno_location(CPUContext* ctx) {
    if (!t_errno_guest_ptr) {
        t_errno_guest_ptr = guest_rt_alloc(4, 4);
    }
    mem_w32(t_errno_guest_ptr, (uint32_t)errno);
    ctx->eax = t_errno_guest_ptr;
}

void imp___ctype_b_loc(CPUContext* ctx) {
    pthread_mutex_lock(&g_tls_misc_lock);
    if (!g_ctype_loc_cell) {
        uint32_t tab = guest_rt_alloc(384 * 2, 16);
        for (int c = -128; c < 256; c++) {
            uint16_t flags = 0;
            if (c >= 0 && c < 128) {
                if (isupper(c))  flags |= 0x0100u;
                if (islower(c))  flags |= 0x0200u;
                if (isalpha(c))  flags |= 0x0400u;
                if (isdigit(c))  flags |= 0x0800u;
                if (isxdigit(c)) flags |= 0x1000u;
                if (isspace(c))  flags |= 0x2000u;
                if (isprint(c))  flags |= 0x4000u;
                if (isgraph(c))  flags |= 0x8000u;
                if (c == ' ' || c == '\t') flags |= 0x0001u;
                if (iscntrl(c))  flags |= 0x0002u;
                if (ispunct(c))  flags |= 0x0004u;
                if (isalnum(c))  flags |= 0x0008u;
            }
            mem_w16(tab + (uint32_t)((c + 128) * 2), flags);
        }
        g_ctype_loc_cell = guest_rt_alloc(4, 4);
        mem_w32(g_ctype_loc_cell, tab + 128u * 2u);
    }
    pthread_mutex_unlock(&g_tls_misc_lock);
    ctx->eax = g_ctype_loc_cell;
}

void imp_isalpha(CPUContext* ctx) { ctx->eax = (uint32_t)isalpha((int)arg32(ctx, 0)); }
void imp_tolower(CPUContext* ctx) { ctx->eax = (uint32_t)tolower((int)arg32(ctx, 0)); }
void imp_toupper(CPUContext* ctx) { ctx->eax = (uint32_t)toupper((int)arg32(ctx, 0)); }

void imp_strerror(CPUContext* ctx) {
    if (!g_err_str) g_err_str = guest_rt_alloc(256, 4);
    snprintf((char*)(g_mem_base + g_err_str), 256, "%s", strerror((int)arg32(ctx, 0)));
    ctx->eax = g_err_str;
}

/* ---------------- Guest-Stack Printf & Scanf Engines ---------------- */

static int guest_vformat(char* out, size_t out_sz, const char* fmt, uint32_t ap32) {
    size_t pos = 0;
    #define EMIT_CH(ch) do { char _ch = (ch); if (pos + 1 < out_sz) out[pos] = _ch; pos++; } while (0)
    while (*fmt) {
        if (*fmt != '%') {
            EMIT_CH(*fmt++);
            continue;
        }
        const char* spec_start = fmt++;
        if (*fmt == '%') { EMIT_CH('%'); fmt++; continue; }

        char spec[64];
        size_t si = 0;
        spec[si++] = '%';
        /* flags */
        while (*fmt && strchr("-+ 0#'", *fmt) && si + 1 < sizeof(spec)) spec[si++] = *fmt++;
        /* width */
        if (*fmt == '*') {
            int w = (int)mem_r32(ap32); ap32 += 4;
            si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", w);
            fmt++;
        } else {
            while (isdigit((unsigned char)*fmt) && si + 1 < sizeof(spec)) spec[si++] = *fmt++;
        }
        /* precision */
        if (*fmt == '.') {
            spec[si++] = *fmt++;
            if (*fmt == '*') {
                int p = (int)mem_r32(ap32); ap32 += 4;
                si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", p);
                fmt++;
            } else {
                while (isdigit((unsigned char)*fmt) && si + 1 < sizeof(spec)) spec[si++] = *fmt++;
            }
        }
        /* length modifier */
        int len_mod = 0; /* 1=l, 2=ll, 3=h, 4=hh, 5=z */
        if (*fmt == 'l') {
            if (fmt[1] == 'l') { len_mod = 2; fmt += 2; }
            else { len_mod = 1; fmt += 1; }
        } else if (*fmt == 'h') {
            if (fmt[1] == 'h') { len_mod = 4; fmt += 2; }
            else { len_mod = 3; fmt += 1; }
        } else if (*fmt == 'z' || *fmt == 't') {
            len_mod = 5; fmt += 1;
        }
        char conv = *fmt ? *fmt++ : '\0';
        if (!conv) { (void)spec_start; break; }

        char tmp[1024];
        int n = 0;
        if (conv == 's') {
            if (si + 2 < sizeof(spec)) { spec[si++] = 's'; spec[si] = '\0'; }
            uint32_t sp = mem_r32(ap32); ap32 += 4;
            const char* s = sp ? (const char*)(g_mem_base + sp) : "(null)";
            if (pos < out_sz) {
                int r = snprintf(out + pos, out_sz - pos, spec, s);
                if (r > 0) pos += (size_t)r;
            } else {
                pos += (size_t)snprintf(NULL, 0, spec, s);
            }
            continue;
        }
        if (conv == 'p') {
            uint32_t pv = mem_r32(ap32); ap32 += 4;
            if (si + 2 < sizeof(spec)) { spec[si++] = 'x'; spec[si] = '\0'; }
            n = snprintf(tmp, sizeof(tmp), "0x%x", pv);
        } else if (conv == 'c') {
            int cv = (int)mem_r32(ap32); ap32 += 4;
            if (si + 2 < sizeof(spec)) { spec[si++] = 'c'; spec[si] = '\0'; }
            n = snprintf(tmp, sizeof(tmp), spec, cv);
        } else if (conv == 'd' || conv == 'i') {
            if (len_mod == 2) {
                int64_t v = (int64_t)mem_r64(ap32); ap32 += 8;
                if (si + 4 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = '\0'; }
                n = snprintf(tmp, sizeof(tmp), spec, (long long)v);
            } else {
                int32_t v = (int32_t)mem_r32(ap32); ap32 += 4;
                if (si + 2 < sizeof(spec)) { spec[si++] = conv; spec[si] = '\0'; }
                n = snprintf(tmp, sizeof(tmp), spec, (int)v);
            }
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            if (len_mod == 2) {
                uint64_t v = mem_r64(ap32); ap32 += 8;
                if (si + 4 < sizeof(spec)) { spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = '\0'; }
                n = snprintf(tmp, sizeof(tmp), spec, (unsigned long long)v);
            } else {
                uint32_t v = mem_r32(ap32); ap32 += 4;
                if (si + 2 < sizeof(spec)) { spec[si++] = conv; spec[si] = '\0'; }
                n = snprintf(tmp, sizeof(tmp), spec, (unsigned)v);
            }
        } else if (strchr("fFeEgGaA", conv)) {
            double v = mem_rf64(ap32); ap32 += 8;
            if (si + 2 < sizeof(spec)) { spec[si++] = conv; spec[si] = '\0'; }
            n = snprintf(tmp, sizeof(tmp), spec, v);
        } else if (conv == 'n') {
            uint32_t np = mem_r32(ap32); ap32 += 4;
            if (np) mem_w32(np, (uint32_t)pos);
            continue;
        } else {
            EMIT_CH(conv);
            continue;
        }
        for (int k = 0; k < n; k++) EMIT_CH(tmp[k]);
    }
    if (out_sz > 0) {
        out[(pos < out_sz) ? pos : (out_sz - 1)] = '\0';
    }
    #undef EMIT_CH
    return (int)pos;
}

static int guest_vscanf(const char* src, FILE* fp, const char* fmt, uint32_t ap32) {
    char file_buf[4096];
    if (fp) {
        long cur = ftell(fp);
        if (!fgets(file_buf, sizeof(file_buf), fp)) return EOF;
        /* Actually for fscanf we scan line/token-by-token; rewind unused part if needed */
        src = file_buf;
        (void)cur;
    }
    if (!src) return EOF;

    const char* s = src;
    int assigned = 0;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt)) fmt++;
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s == *fmt) { s++; fmt++; continue; }
            break;
        }
        fmt++;
        if (*fmt == '%') {
            if (*s == '%') { s++; fmt++; continue; }
            break;
        }
        bool suppress = false;
        if (*fmt == '*') { suppress = true; fmt++; }
        int width = 0;
        while (isdigit((unsigned char)*fmt)) { width = width * 10 + (*fmt - '0'); fmt++; }
        int len_mod = 0;
        if (*fmt == 'l') { len_mod = (fmt[1] == 'l') ? (fmt += 2, 2) : (fmt += 1, 1); }
        else if (*fmt == 'h') { len_mod = (fmt[1] == 'h') ? (fmt += 2, 4) : (fmt += 1, 3); }
        char conv = *fmt++;
        if (!conv) break;

        if (conv == 'n') {
            if (!suppress) { uint32_t p = mem_r32(ap32); ap32 += 4; mem_w32(p, (uint32_t)(s - src)); }
            continue;
        }
        if (conv != 'c' && conv != '[') {
            while (isspace((unsigned char)*s)) s++;
        }
        if (!*s) return assigned ? assigned : EOF;

        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            char tok[128];
            int lim = (width > 0 && width < 127) ? width : 127;
            int ti = 0;
            if ((s[0] == '-' || s[0] == '+') && ti < lim) tok[ti++] = *s++;
            int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o' ? 8 : (conv == 'i' ? 0 : 10));
            if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && ti + 2 <= lim) {
                tok[ti++] = *s++; tok[ti++] = *s++;
            }
            while (ti < lim && *s) {
                char c = *s;
                bool ok = (base == 16) ? isxdigit((unsigned char)c) :
                          (base == 8) ? (c >= '0' && c <= '7') :
                          (isdigit((unsigned char)c) || (base == 0 && ti <= 2 && (c=='x'||c=='X'||isxdigit((unsigned char)c))));
                if (!ok) break;
                tok[ti++] = *s++;
            }
            tok[ti] = '\0';
            if (ti == 0 || (ti == 1 && (tok[0] == '-' || tok[0] == '+'))) break;
            if (!suppress) {
                uint32_t p = mem_r32(ap32); ap32 += 4;
                if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                    uint64_t v = strtoull(tok, NULL, base ? base : 10);
                    if (len_mod == 2) mem_w64(p, v);
                    else if (len_mod == 3) mem_w16(p, (uint16_t)v);
                    else mem_w32(p, (uint32_t)v);
                } else {
                    int64_t v = strtoll(tok, NULL, base);
                    if (len_mod == 2) mem_w64(p, (uint64_t)v);
                    else if (len_mod == 3) mem_w16(p, (uint16_t)v);
                    else mem_w32(p, (uint32_t)v);
                }
                assigned++;
            }
        } else if (conv == 'f' || conv == 'g' || conv == 'e' || conv == 'F' || conv == 'G' || conv == 'E') {
            char tok[128];
            int lim = (width > 0 && width < 127) ? width : 127;
            int ti = 0;
            while (ti < lim && *s && !isspace((unsigned char)*s)) {
                char c = *s;
                if (!isdigit((unsigned char)c) && c!='.' && c!='-' && c!='+' && c!='e' && c!='E') break;
                tok[ti++] = *s++;
            }
            tok[ti] = '\0';
            char* endp = NULL;
            double v = strtod(tok, &endp);
            if (endp == tok) break;
            s -= (ti - (int)(endp - tok));
            if (!suppress) {
                uint32_t p = mem_r32(ap32); ap32 += 4;
                if (len_mod == 1) mem_wf64(p, v);
                else mem_wf32(p, (float)v);
                assigned++;
            }
        } else if (conv == 's') {
            uint32_t p = suppress ? 0 : mem_r32(ap32);
            if (!suppress) ap32 += 4;
            int lim = (width > 0) ? width : 0x7FFFFFFF;
            int k = 0;
            while (k < lim && *s && !isspace((unsigned char)*s)) {
                if (!suppress) mem_w8(p + (uint32_t)k, (uint8_t)*s);
                s++; k++;
            }
            if (k == 0) break;
            if (!suppress) { mem_w8(p + (uint32_t)k, 0); assigned++; }
        } else if (conv == 'c') {
            uint32_t p = suppress ? 0 : mem_r32(ap32);
            if (!suppress) ap32 += 4;
            int lim = (width > 0) ? width : 1;
            int k = 0;
            while (k < lim && *s) {
                if (!suppress) mem_w8(p + (uint32_t)k, (uint8_t)*s);
                s++; k++;
            }
            if (k < lim) break;
            if (!suppress) assigned++;
        } else if (conv == '[') {
            bool inv = (*fmt == '^');
            if (inv) fmt++;
            uint8_t set[256] = {0};
            if (*fmt == ']') { set[(uint8_t)']'] = 1; fmt++; }
            while (*fmt && *fmt != ']') {
                if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                    for (int c = (uint8_t)fmt[0]; c <= (uint8_t)fmt[2]; c++) set[c] = 1;
                    fmt += 3;
                } else {
                    set[(uint8_t)*fmt++] = 1;
                }
            }
            if (*fmt == ']') fmt++;
            uint32_t p = suppress ? 0 : mem_r32(ap32);
            if (!suppress) ap32 += 4;
            int lim = (width > 0) ? width : 0x7FFFFFFF;
            int k = 0;
            while (k < lim && *s && (inv ? !set[(uint8_t)*s] : set[(uint8_t)*s])) {
                if (!suppress) mem_w8(p + (uint32_t)k, (uint8_t)*s);
                s++; k++;
            }
            if (k == 0) break;
            if (!suppress) { mem_w8(p + (uint32_t)k, 0); assigned++; }
        } else {
            break;
        }
    }
    return assigned;
}

static int g_guest_log_enabled = -1;
static inline bool guest_log_active(void) {
    if (__builtin_expect(g_guest_log_enabled < 0, 0)) {
        const char* e = getenv("RECOMP_GUEST_LOG");
        g_guest_log_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return g_guest_log_enabled != 0;
}

void imp_printf(CPUContext* ctx) {
    if (!guest_log_active()) {
        ctx->eax = 1;
        return;
    }
    char buf[4096];
    int n = guest_vformat(buf, sizeof(buf), cstr32(arg32(ctx, 0)), ctx->esp + 8u);
    fwrite(buf, 1, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), stdout);
    ctx->eax = (uint32_t)n;
}

void imp_fprintf(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 0));
    if (!fp) fp = stderr;
    if ((fp == stdout || fp == stderr) && !guest_log_active()) {
        ctx->eax = 1;
        return;
    }
    char buf[4096];
    int n = guest_vformat(buf, sizeof(buf), cstr32(arg32(ctx, 1)), ctx->esp + 12u);
    fwrite(buf, 1, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), fp);
    ctx->eax = (uint32_t)n;
}

void imp_sprintf(CPUContext* ctx) {
    char* dst = mstr32(arg32(ctx, 0));
    int n = guest_vformat(dst, 65536u, cstr32(arg32(ctx, 1)), ctx->esp + 12u);
    ctx->eax = (uint32_t)n;
}

void imp_snprintf(CPUContext* ctx) {
    char* dst = mstr32(arg32(ctx, 0));
    size_t sz = (size_t)arg32(ctx, 1);
    int n = guest_vformat(dst, sz, cstr32(arg32(ctx, 2)), ctx->esp + 16u);
    ctx->eax = (uint32_t)n;
}

void imp_vsprintf(CPUContext* ctx) {
    char* dst = mstr32(arg32(ctx, 0));
    int n = guest_vformat(dst, 65536u, cstr32(arg32(ctx, 1)), arg32(ctx, 2));
    ctx->eax = (uint32_t)n;
}

void imp_vsnprintf(CPUContext* ctx) {
    char* dst = mstr32(arg32(ctx, 0));
    size_t sz = (size_t)arg32(ctx, 1);
    int n = guest_vformat(dst, sz, cstr32(arg32(ctx, 2)), arg32(ctx, 3));
    ctx->eax = (uint32_t)n;
}

void imp_sscanf(CPUContext* ctx) {
    ctx->eax = (uint32_t)guest_vscanf(cstr32(arg32(ctx, 0)), NULL, cstr32(arg32(ctx, 1)), ctx->esp + 12u);
}
void imp___isoc99_sscanf(CPUContext* ctx) { imp_sscanf(ctx); }

void imp_fscanf(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 0));
    ctx->eax = (uint32_t)(fp ? guest_vscanf(NULL, fp, cstr32(arg32(ctx, 1)), ctx->esp + 12u) : EOF);
}
void imp___isoc99_fscanf(CPUContext* ctx) { imp_fscanf(ctx); }

/* ---------------- Memory / String / C++ Allocators ---------------- */

void imp_malloc(CPUContext* ctx) { ctx->eax = guest_malloc(arg32(ctx, 0)); }
void imp_calloc(CPUContext* ctx) { ctx->eax = guest_calloc(arg32(ctx, 0), arg32(ctx, 1)); }
void imp_realloc(CPUContext* ctx) { ctx->eax = guest_realloc(arg32(ctx, 0), arg32(ctx, 1)); }
void imp_free(CPUContext* ctx) { guest_free(arg32(ctx, 0)); }
void imp__Znwj(CPUContext* ctx) { ctx->eax = guest_malloc(arg32(ctx, 0)); }
void imp__Znaj(CPUContext* ctx) { ctx->eax = guest_malloc(arg32(ctx, 0)); }
void imp__ZdlPv(CPUContext* ctx) { guest_free(arg32(ctx, 0)); }
void imp__ZdaPv(CPUContext* ctx) { guest_free(arg32(ctx, 0)); }

void imp_memcpy(CPUContext* ctx) {
    uint32_t dst = arg32(ctx, 0);
    memmove(g_mem_base + dst, g_mem_base + arg32(ctx, 1), arg32(ctx, 2));
    ctx->eax = dst;
}
void imp_memmove(CPUContext* ctx) { imp_memcpy(ctx); }
void imp_memset(CPUContext* ctx) {
    uint32_t dst = arg32(ctx, 0);
    memset(g_mem_base + dst, (int)arg32(ctx, 1), arg32(ctx, 2));
    ctx->eax = dst;
}
void imp_memcmp(CPUContext* ctx) {
    ctx->eax = (uint32_t)memcmp(g_mem_base + arg32(ctx, 0), g_mem_base + arg32(ctx, 1), arg32(ctx, 2));
}
void imp_memchr(CPUContext* ctx) {
    uint32_t s = arg32(ctx, 0);
    const void* r = memchr(g_mem_base + s, (int)arg32(ctx, 1), arg32(ctx, 2));
    ctx->eax = r ? (uint32_t)((const uint8_t*)r - g_mem_base) : 0u;
}

void imp_strlen(CPUContext* ctx) { ctx->eax = (uint32_t)strlen(cstr32(arg32(ctx, 0))); }
void imp_strcmp(CPUContext* ctx) { ctx->eax = (uint32_t)strcmp(cstr32(arg32(ctx, 0)), cstr32(arg32(ctx, 1))); }
void imp_strcoll(CPUContext* ctx) { ctx->eax = (uint32_t)strcoll(cstr32(arg32(ctx, 0)), cstr32(arg32(ctx, 1))); }
void imp_strncmp(CPUContext* ctx) { ctx->eax = (uint32_t)strncmp(cstr32(arg32(ctx, 0)), cstr32(arg32(ctx, 1)), arg32(ctx, 2)); }
void imp_strcasecmp(CPUContext* ctx) { ctx->eax = (uint32_t)strcasecmp(cstr32(arg32(ctx, 0)), cstr32(arg32(ctx, 1))); }
void imp_strncasecmp(CPUContext* ctx) { ctx->eax = (uint32_t)strncasecmp(cstr32(arg32(ctx, 0)), cstr32(arg32(ctx, 1)), arg32(ctx, 2)); }
void imp_strcpy(CPUContext* ctx) { uint32_t d = arg32(ctx, 0); strcpy(mstr32(d), cstr32(arg32(ctx, 1))); ctx->eax = d; }
void imp_strncpy(CPUContext* ctx) { uint32_t d = arg32(ctx, 0); strncpy(mstr32(d), cstr32(arg32(ctx, 1)), arg32(ctx, 2)); ctx->eax = d; }
void imp_strcat(CPUContext* ctx) { uint32_t d = arg32(ctx, 0); strcat(mstr32(d), cstr32(arg32(ctx, 1))); ctx->eax = d; }
void imp_strncat(CPUContext* ctx) { uint32_t d = arg32(ctx, 0); strncat(mstr32(d), cstr32(arg32(ctx, 1)), arg32(ctx, 2)); ctx->eax = d; }
void imp_strdup(CPUContext* ctx) { ctx->eax = guest_strdup(cstr32(arg32(ctx, 0))); }
void imp_strcspn(CPUContext* ctx) { ctx->eax = (uint32_t)strcspn(cstr32(arg32(ctx, 0)), cstr32(arg32(ctx, 1))); }

void imp_strchr(CPUContext* ctx) {
    const char* s = cstr32(arg32(ctx, 0));
    const char* r = s ? strchr(s, (int)arg32(ctx, 1)) : NULL;
    ctx->eax = r ? (uint32_t)((const uint8_t*)r - g_mem_base) : 0u;
}
void imp_strrchr(CPUContext* ctx) {
    const char* s = cstr32(arg32(ctx, 0));
    const char* r = s ? strrchr(s, (int)arg32(ctx, 1)) : NULL;
    ctx->eax = r ? (uint32_t)((const uint8_t*)r - g_mem_base) : 0u;
}
void imp_strstr(CPUContext* ctx) {
    const char* s = cstr32(arg32(ctx, 0));
    const char* n = cstr32(arg32(ctx, 1));
    const char* r = (s && n) ? strstr(s, n) : NULL;
    ctx->eax = r ? (uint32_t)((const uint8_t*)r - g_mem_base) : 0u;
}
void imp_strpbrk(CPUContext* ctx) {
    const char* s = cstr32(arg32(ctx, 0));
    const char* a = cstr32(arg32(ctx, 1));
    const char* r = (s && a) ? strpbrk(s, a) : NULL;
    ctx->eax = r ? (uint32_t)((const uint8_t*)r - g_mem_base) : 0u;
}

static __thread uint32_t t_strtok_state = 0;
void imp_strtok(CPUContext* ctx) {
    uint32_t sp = arg32(ctx, 0);
    const char* delim = cstr32(arg32(ctx, 1));
    if (!sp) sp = t_strtok_state;
    if (!sp) { ctx->eax = 0; return; }
    char* p = mstr32(sp);
    p += strspn(p, delim);
    if (!*p) { t_strtok_state = 0; ctx->eax = 0; return; }
    char* end = p + strcspn(p, delim);
    if (*end) { *end = '\0'; t_strtok_state = (uint32_t)((uint8_t*)(end + 1) - g_mem_base); }
    else { t_strtok_state = 0; }
    ctx->eax = (uint32_t)((uint8_t*)p - g_mem_base);
}

void imp_atoi(CPUContext* ctx) { ctx->eax = (uint32_t)atoi(cstr32(arg32(ctx, 0))); }
void imp_strtoul(CPUContext* ctx) {
    uint32_t sp = arg32(ctx, 0);
    uint32_t ep = arg32(ctx, 1);
    int base = (int)arg32(ctx, 2);
    const char* s = cstr32(sp);
    char* end = NULL;
    unsigned long r = strtoul(s, &end, base);
    if (ep && end) mem_w32(ep, sp + (uint32_t)(end - s));
    ctx->eax = (uint32_t)r;
}
void imp_atof(CPUContext* ctx) { fpu_push(ctx, atof(cstr32(arg32(ctx, 0)))); }
void imp_strtod(CPUContext* ctx) {
    uint32_t sp = arg32(ctx, 0);
    uint32_t ep = arg32(ctx, 1);
    const char* s = cstr32(sp);
    char* end = NULL;
    double r = strtod(s, &end);
    if (ep && end) mem_w32(ep, sp + (uint32_t)(end - s));
    fpu_push(ctx, r);
}

void imp_puts(CPUContext* ctx) {
    if (!guest_log_active()) { ctx->eax = 1; return; }
    ctx->eax = (uint32_t)puts(cstr32(arg32(ctx, 0)));
}
void imp_rand(CPUContext* ctx) { ctx->eax = (uint32_t)rand(); }
void imp_srand(CPUContext* ctx) { srand(arg32(ctx, 0)); }

void imp___divdi3(CPUContext* ctx) {
    int64_t a = (int64_t)mem_r64(ctx->esp + 4u);
    int64_t b = (int64_t)mem_r64(ctx->esp + 12u);
    int64_t q = a / b;
    ctx->eax = (uint32_t)(uint64_t)q;
    ctx->edx = (uint32_t)((uint64_t)q >> 32);
}

void imp___moddi3(CPUContext* ctx) {
    int64_t a = (int64_t)mem_r64(ctx->esp + 4u);
    int64_t b = (int64_t)mem_r64(ctx->esp + 12u);
    int64_t r = a % b;
    ctx->eax = (uint32_t)(uint64_t)r;
    ctx->edx = (uint32_t)((uint64_t)r >> 32);
}

static int qsort_cmp_guest(CPUContext* ctx, uint32_t cmp_fn, uint32_t a_addr, uint32_t b_addr) {
    uint32_t saved_esp = ctx->esp;
    push32(ctx, b_addr);
    push32(ctx, a_addr);
    push32(ctx, 0xDEAD0002u);
    recomp_icall(ctx, cmp_fn);
    ctx->esp = saved_esp;
    return (int)(int32_t)ctx->eax;
}

static void swap_bytes(uint8_t* a, uint8_t* b, uint32_t sz) {
    for (uint32_t i = 0; i < sz; i++) {
        uint8_t t = a[i]; a[i] = b[i]; b[i] = t;
    }
}

static void guest_quicksort(CPUContext* ctx, uint32_t base, uint32_t lo, uint32_t hi, uint32_t sz, uint32_t cmp_fn) {
    if (lo >= hi) return;
    uint32_t mid = lo + (hi - lo) / 2u;
    swap_bytes(g_mem_base + base + mid * sz, g_mem_base + base + hi * sz, sz);
    uint32_t pivot_addr = base + hi * sz;
    uint32_t store = lo;
    for (uint32_t i = lo; i < hi; i++) {
        if (qsort_cmp_guest(ctx, cmp_fn, base + i * sz, pivot_addr) < 0) {
            if (i != store) swap_bytes(g_mem_base + base + i * sz, g_mem_base + base + store * sz, sz);
            store++;
        }
    }
    swap_bytes(g_mem_base + base + store * sz, g_mem_base + base + hi * sz, sz);
    if (store > lo) guest_quicksort(ctx, base, lo, store - 1u, sz, cmp_fn);
    if (store + 1u <= hi) guest_quicksort(ctx, base, store + 1u, hi, sz, cmp_fn);
}

void imp_qsort(CPUContext* ctx) {
    uint32_t base = arg32(ctx, 0);
    uint32_t n = arg32(ctx, 1);
    uint32_t sz = arg32(ctx, 2);
    uint32_t cmp = arg32(ctx, 3);
    if (n > 1u && sz > 0u) {
        guest_quicksort(ctx, base, 0, n - 1u, sz, cmp);
    }
}

/* ---------------- Math Functions (push result on x87 stack!) ---------------- */

void imp_acos(CPUContext* c) { fpu_push(c, acos(argf64(c, 0))); }
void imp_acosf(CPUContext* c) { fpu_push(c, (double)acosf(mem_rf32(c->esp + 4u))); }
void imp_asin(CPUContext* c) { fpu_push(c, asin(argf64(c, 0))); }
void imp_asinf(CPUContext* c) { fpu_push(c, (double)asinf(mem_rf32(c->esp + 4u))); }
void imp_atan(CPUContext* c) { fpu_push(c, atan(argf64(c, 0))); }
void imp_atanf(CPUContext* c) { fpu_push(c, (double)atanf(mem_rf32(c->esp + 4u))); }
void imp_atan2(CPUContext* c) { fpu_push(c, atan2(argf64(c, 0), argf64(c, 8))); }
void imp_atan2f(CPUContext* c) { fpu_push(c, (double)atan2f(mem_rf32(c->esp + 4u), mem_rf32(c->esp + 8u))); }
void imp_ceil(CPUContext* c) { fpu_push(c, ceil(argf64(c, 0))); }
void imp_cos(CPUContext* c) { fpu_push(c, cos(argf64(c, 0))); }
void imp_cosf(CPUContext* c) { fpu_push(c, (double)cosf(mem_rf32(c->esp + 4u))); }
void imp_exp(CPUContext* c) { fpu_push(c, exp(argf64(c, 0))); }
void imp_expf(CPUContext* c) { fpu_push(c, (double)expf(mem_rf32(c->esp + 4u))); }
void imp_floor(CPUContext* c) { fpu_push(c, floor(argf64(c, 0))); }
void imp_fmod(CPUContext* c) { fpu_push(c, fmod(argf64(c, 0), argf64(c, 8))); }
void imp_fmodf(CPUContext* c) { fpu_push(c, (double)fmodf(mem_rf32(c->esp + 4u), mem_rf32(c->esp + 8u))); }
void imp_frexp(CPUContext* c) {
    int e = 0;
    double r = frexp(argf64(c, 0), &e);
    uint32_t ep = mem_r32(c->esp + 12u);
    if (ep) mem_w32(ep, (uint32_t)e);
    fpu_push(c, r);
}
void imp_ldexp(CPUContext* c) { fpu_push(c, ldexp(argf64(c, 0), (int)mem_r32(c->esp + 12u))); }
void imp_log(CPUContext* c) { fpu_push(c, log(argf64(c, 0))); }
void imp_log10(CPUContext* c) { fpu_push(c, log10(argf64(c, 0))); }
void imp_log10f(CPUContext* c) { fpu_push(c, (double)log10f(mem_rf32(c->esp + 4u))); }
void imp_modf(CPUContext* c) {
    double ip = 0.0;
    double r = modf(argf64(c, 0), &ip);
    uint32_t ipp = mem_r32(c->esp + 12u);
    if (ipp) mem_wf64(ipp, ip);
    fpu_push(c, r);
}
void imp_pow(CPUContext* c) { fpu_push(c, pow(argf64(c, 0), argf64(c, 8))); }
void imp_powf(CPUContext* c) { fpu_push(c, (double)powf(mem_rf32(c->esp + 4u), mem_rf32(c->esp + 8u))); }
void imp_rint(CPUContext* c) { fpu_push(c, rint(argf64(c, 0))); }
void imp_sin(CPUContext* c) { fpu_push(c, sin(argf64(c, 0))); }
void imp_sinf(CPUContext* c) { fpu_push(c, (double)sinf(mem_rf32(c->esp + 4u))); }
void imp_sqrt(CPUContext* c) { fpu_push(c, sqrt(argf64(c, 0))); }
void imp_sqrtf(CPUContext* c) { fpu_push(c, (double)sqrtf(mem_rf32(c->esp + 4u))); }
void imp_tan(CPUContext* c) { fpu_push(c, tan(argf64(c, 0))); }
void imp_tanf(CPUContext* c) { fpu_push(c, (double)tanf(mem_rf32(c->esp + 4u))); }

/* ---------------- File / Directory / Stat / Time ---------------- */

static void write_i386_stat(uint32_t buf_p, const struct stat* st) {
    if (!buf_p) return;
    memset(g_mem_base + buf_p, 0, 88);
    mem_w64(buf_p + 0x00u, (uint64_t)st->st_dev);
    mem_w32(buf_p + 0x0Cu, (uint32_t)st->st_ino);
    mem_w32(buf_p + 0x10u, (uint32_t)st->st_mode);
    mem_w32(buf_p + 0x14u, (uint32_t)st->st_nlink);
    mem_w32(buf_p + 0x18u, (uint32_t)st->st_uid);
    mem_w32(buf_p + 0x1Cu, (uint32_t)st->st_gid);
    mem_w64(buf_p + 0x20u, (uint64_t)st->st_rdev);
    mem_w32(buf_p + 0x2Cu, (uint32_t)st->st_size);
    mem_w32(buf_p + 0x30u, (uint32_t)st->st_blksize);
    mem_w32(buf_p + 0x34u, (uint32_t)st->st_blocks);
    mem_w32(buf_p + 0x38u, (uint32_t)st->st_atime);
    mem_w32(buf_p + 0x40u, (uint32_t)st->st_mtime);
    mem_w32(buf_p + 0x48u, (uint32_t)st->st_ctime);
}

void imp___xstat(CPUContext* ctx) {
    const char* p = cstr32(arg32(ctx, 1));
    uint32_t buf = arg32(ctx, 2);
    char hp[1024];
    struct stat st;
    if (!vfs_resolve_path(p, hp, sizeof(hp), false) || stat(hp, &st) != 0) {
        ctx->eax = 0xFFFFFFFFu;
        return;
    }
    write_i386_stat(buf, &st);
    ctx->eax = 0;
}

void imp___stat(CPUContext* ctx) {
    const char* p = cstr32(arg32(ctx, 0));
    uint32_t buf = arg32(ctx, 1);
    char hp[1024];
    struct stat st;
    if (!vfs_resolve_path(p, hp, sizeof(hp), false) || stat(hp, &st) != 0) {
        ctx->eax = 0xFFFFFFFFu;
        return;
    }
    write_i386_stat(buf, &st);
    ctx->eax = 0;
}

void imp___fxstat(CPUContext* ctx) {
    int fd = (int)arg32(ctx, 1);
    uint32_t buf = arg32(ctx, 2);
    struct stat st;
    if (fstat(fd, &st) != 0) { ctx->eax = 0xFFFFFFFFu; return; }
    write_i386_stat(buf, &st);
    ctx->eax = 0;
}

void imp___fstat(CPUContext* ctx) {
    int fd = (int)arg32(ctx, 0);
    uint32_t buf = arg32(ctx, 1);
    struct stat st;
    if (fstat(fd, &st) != 0) { ctx->eax = 0xFFFFFFFFu; return; }
    write_i386_stat(buf, &st);
    ctx->eax = 0;
}

void imp_fopen(CPUContext* ctx) {
    const char* path = cstr32(arg32(ctx, 0));
    const char* mode = cstr32(arg32(ctx, 1));
    if (!path || !mode) { ctx->eax = 0; return; }
    char hp[1024];
    bool wr = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    if (!vfs_resolve_path(path, hp, sizeof(hp), wr)) {
        ctx->eax = 0;
        return;
    }
    FILE* fp = fopen(hp, mode);
    ctx->eax = wrap_file(fp);
}

void imp_freopen(CPUContext* ctx) {
    const char* path = cstr32(arg32(ctx, 0));
    const char* mode = cstr32(arg32(ctx, 1));
    uint32_t fh = arg32(ctx, 2);
    FILE* old = unwrap_file(fh);
    if (!old || !path || !mode) { ctx->eax = 0; return; }
    char hp[1024];
    bool wr = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    if (!vfs_resolve_path(path, hp, sizeof(hp), wr)) { ctx->eax = 0; return; }
    FILE* nf = freopen(hp, mode, old);
    ctx->eax = nf ? fh : 0u;
}

void imp_fclose(CPUContext* ctx) {
    uint32_t fh = arg32(ctx, 0);
    FILE* fp = unwrap_file(fh);
    if (!fp) { ctx->eax = (uint32_t)EOF; return; }
    int rc = (fh >= 0x087A0010u) ? fclose(fp) : 0;
    unreg_file(fh);
    ctx->eax = (uint32_t)rc;
}

void imp_fread(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 3));
    ctx->eax = fp ? (uint32_t)fread(mstr32(arg32(ctx, 0)), arg32(ctx, 1), arg32(ctx, 2), fp) : 0u;
}
void imp_fwrite(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 3));
    if ((fp == stdout || fp == stderr) && !guest_log_active()) {
        ctx->eax = arg32(ctx, 2);
        return;
    }
    ctx->eax = fp ? (uint32_t)fwrite(cstr32(arg32(ctx, 0)), arg32(ctx, 1), arg32(ctx, 2), fp) : 0u;
}
void imp_fseek(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 0));
    ctx->eax = fp ? (uint32_t)fseek(fp, (long)(int32_t)arg32(ctx, 1), (int)arg32(ctx, 2)) : 0xFFFFFFFFu;
}
void imp_ftell(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 0));
    ctx->eax = fp ? (uint32_t)ftell(fp) : 0xFFFFFFFFu;
}
void imp_feof(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 0));
    ctx->eax = fp ? (uint32_t)feof(fp) : 1u;
}
void imp_fflush(CPUContext* ctx) {
    uint32_t fh = arg32(ctx, 0);
    ctx->eax = (uint32_t)fflush(fh ? unwrap_file(fh) : NULL);
}
void imp_fgetc(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 0));
    ctx->eax = fp ? (uint32_t)fgetc(fp) : (uint32_t)EOF;
}
void imp_fputc(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 1));
    ctx->eax = fp ? (uint32_t)fputc((int)arg32(ctx, 0), fp) : (uint32_t)EOF;
}
void imp_ungetc(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 1));
    ctx->eax = fp ? (uint32_t)ungetc((int)arg32(ctx, 0), fp) : (uint32_t)EOF;
}
void imp_fgets(CPUContext* ctx) {
    uint32_t sp = arg32(ctx, 0);
    FILE* fp = unwrap_file(arg32(ctx, 2));
    char* r = fp ? fgets(mstr32(sp), (int)arg32(ctx, 1), fp) : NULL;
    ctx->eax = r ? sp : 0u;
}
void imp_fputs(CPUContext* ctx) {
    FILE* fp = unwrap_file(arg32(ctx, 1));
    if ((fp == stdout || fp == stderr) && !guest_log_active()) {
        ctx->eax = 1;
        return;
    }
    ctx->eax = fp ? (uint32_t)fputs(cstr32(arg32(ctx, 0)), fp) : (uint32_t)EOF;
}

void imp_open(CPUContext* ctx) {
    const char* p = cstr32(arg32(ctx, 0));
    int gflags = (int)arg32(ctx, 1);
    int mode = (int)arg32(ctx, 2);
    /* Translate Linux i386 O_* flags to host/newlib O_* flags */
    int hflags = 0;
    int acc = gflags & 3;
    if (acc == 0) hflags = O_RDONLY;
    else if (acc == 1) hflags = O_WRONLY;
    else if (acc == 2) hflags = O_RDWR;
    if (gflags & 0x40)  hflags |= O_CREAT;
    if (gflags & 0x80)  hflags |= O_EXCL;
    if (gflags & 0x200) hflags |= O_TRUNC;
    if (gflags & 0x400) hflags |= O_APPEND;
    char hp[1024];
    bool wr = (hflags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
    if (!vfs_resolve_path(p, hp, sizeof(hp), wr)) {
        ctx->eax = 0xFFFFFFFFu;
        return;
    }
    ctx->eax = (uint32_t)open(hp, hflags, mode);
}

void imp_close(CPUContext* ctx) { ctx->eax = (uint32_t)close((int)arg32(ctx, 0)); }
void imp_read(CPUContext* ctx) { ctx->eax = (uint32_t)read((int)arg32(ctx, 0), mstr32(arg32(ctx, 1)), arg32(ctx, 2)); }
void imp_write(CPUContext* ctx) { ctx->eax = (uint32_t)write((int)arg32(ctx, 0), cstr32(arg32(ctx, 1)), arg32(ctx, 2)); }
void imp_lseek(CPUContext* ctx) { ctx->eax = (uint32_t)lseek((int)arg32(ctx, 0), (off_t)(int32_t)arg32(ctx, 1), (int)arg32(ctx, 2)); }

void imp_access(CPUContext* ctx) {
    const char* p = cstr32(arg32(ctx, 0));
    int mode = (int)arg32(ctx, 1);
    char hp[1024];
    if (!vfs_resolve_path(p, hp, sizeof(hp), false)) { ctx->eax = 0xFFFFFFFFu; return; }
    if ((mode & ~(F_OK | R_OK)) == 0) { ctx->eax = 0; return; }
    ctx->eax = (uint32_t)access(hp, mode);
}

void imp_mkdir(CPUContext* ctx) {
    const char* p = cstr32(arg32(ctx, 0));
    if (!p || !*p || strcmp(p, "/") == 0 || strcmp(p, ".") == 0 ||
        strcmp(p, "sdmc:") == 0 || strcmp(p, "sdmc:/") == 0) {
        ctx->eax = 0;
        return;
    }
    char hp[1024];
    if (!vfs_resolve_path(p, hp, sizeof(hp), true)) snprintf(hp, sizeof(hp), "%s", p);
    if (strcmp(hp, "sdmc:") == 0 || strcmp(hp, "sdmc:/") == 0 || strcmp(hp, "/") == 0) {
        ctx->eax = 0;
        return;
    }
    ctx->eax = (uint32_t)mkdir(hp, (mode_t)arg32(ctx, 1));
}

void imp_remove(CPUContext* ctx) {
    char hp[1024];
    if (!vfs_resolve_path(cstr32(arg32(ctx, 0)), hp, sizeof(hp), false)) { ctx->eax = 0xFFFFFFFFu; return; }
    vfs_cache_invalidate();
    ctx->eax = (uint32_t)remove(hp);
}
void imp_unlink(CPUContext* ctx) { imp_remove(ctx); }

void imp_rename(CPUContext* ctx) {
    char oldp[1024], newp[1024];
    if (!vfs_resolve_path(cstr32(arg32(ctx, 0)), oldp, sizeof(oldp), false) ||
        !vfs_resolve_path(cstr32(arg32(ctx, 1)), newp, sizeof(newp), true)) {
        ctx->eax = 0xFFFFFFFFu;
        return;
    }
    ctx->eax = (uint32_t)rename(oldp, newp);
}

void imp_mkstemp(CPUContext* ctx) {
    char* tmpl = mstr32(arg32(ctx, 0));
    ctx->eax = tmpl ? (uint32_t)mkstemp(tmpl) : 0xFFFFFFFFu;
}

void imp_chdir(CPUContext* ctx) {
    const char* p = cstr32(arg32(ctx, 0));
    char hp[1024];
    if (p && strcmp(p, recomp_get_game_dir()) == 0) {
        ctx->eax = (uint32_t)chdir(p);
        return;
    }
    /* Keep cwd anchored at game_dir if chdir is called with argv[0] dir during startup */
    if (p && (strstr(p, "Psychonauts") || strstr(p, "game/linux"))) {
        ctx->eax = (uint32_t)chdir(recomp_get_game_dir());
        return;
    }
    if (vfs_resolve_path(p, hp, sizeof(hp), false)) {
        ctx->eax = (uint32_t)chdir(hp);
        return;
    }
    ctx->eax = 0;
}

void imp_getcwd(CPUContext* ctx) {
    uint32_t buf = arg32(ctx, 0);
    uint32_t sz = arg32(ctx, 1);
    if (!buf) {
        if (!g_getcwd_buf) g_getcwd_buf = guest_rt_alloc(1024, 4);
        buf = g_getcwd_buf;
        sz = 1024;
    }
    char* r = getcwd(mstr32(buf), sz);
    ctx->eax = r ? buf : 0u;
}

void imp_opendir(CPUContext* ctx) {
    char hp[1024];
    if (!vfs_resolve_path(cstr32(arg32(ctx, 0)), hp, sizeof(hp), false)) {
        ctx->eax = 0;
        return;
    }
    DIR* d = opendir(hp);
    if (!d) { ctx->eax = 0; return; }
    pthread_mutex_lock(&g_io_lock);
    for (int i = 1; i < MAX_DIR_HANDLES; i++) {
        if (!g_dirs[i]) {
            g_dirs[i] = d;
            if (!g_dir_dirent_ptr[i]) g_dir_dirent_ptr[i] = guest_rt_alloc(280, 4);
            pthread_mutex_unlock(&g_io_lock);
            ctx->eax = 0x087B0000u + (uint32_t)(i * 4);
            return;
        }
    }
    pthread_mutex_unlock(&g_io_lock);
    closedir(d);
    ctx->eax = 0;
}

void imp_readdir(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    if (h < 0x087B0004u) { ctx->eax = 0; return; }
    uint32_t idx = (h - 0x087B0000u) >> 2;
    if (idx >= MAX_DIR_HANDLES || !g_dirs[idx]) { ctx->eax = 0; return; }
    struct dirent* ent = readdir(g_dirs[idx]);
    if (!ent) { ctx->eax = 0; return; }
    uint32_t dp = g_dir_dirent_ptr[idx];
    /* i386 struct dirent: d_ino(4) +0, d_off(4) +4, d_reclen(2) +8, d_type(1) +10, d_name[256] +11 */
    mem_w32(dp + 0, (uint32_t)ent->d_ino);
    mem_w32(dp + 4, 0);
    mem_w16(dp + 8, 268);
    mem_w8(dp + 10, (uint8_t)ent->d_type);
    snprintf((char*)(g_mem_base + dp + 11), 256, "%s", ent->d_name);
    ctx->eax = dp;
}

void imp_closedir(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    if (h < 0x087B0004u) { ctx->eax = 0xFFFFFFFFu; return; }
    uint32_t idx = (h - 0x087B0000u) >> 2;
    if (idx < MAX_DIR_HANDLES && g_dirs[idx]) {
        closedir(g_dirs[idx]);
        g_dirs[idx] = NULL;
        ctx->eax = 0;
    } else {
        ctx->eax = 0xFFFFFFFFu;
    }
}

void imp_mmap(CPUContext* ctx) {
    uint32_t len = arg32(ctx, 1);
    int fd = (int)arg32(ctx, 4);
    off_t off = (off_t)(uint32_t)arg32(ctx, 5);
    uint32_t gp = guest_malloc(len ? len : 4096u);
    if (!gp) { ctx->eax = 0xFFFFFFFFu; return; }
    if (fd >= 0) {
        off_t saved = lseek(fd, 0, SEEK_CUR);
        lseek(fd, off, SEEK_SET);
        size_t done = 0;
        while (done < len) {
            ssize_t r = read(fd, g_mem_base + gp + done, len - done);
            if (r <= 0) break;
            done += (size_t)r;
        }
        if (saved >= 0) lseek(fd, saved, SEEK_SET);
    }
    ctx->eax = gp;
}

void imp_munmap(CPUContext* ctx) {
    guest_free(arg32(ctx, 0));
    ctx->eax = 0;
}

static pthread_mutex_t g_time_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_last_real_us = 0;
static int64_t g_sleep_offset_us = 0;
static int64_t g_paused_at_us = 0;
static bool    g_time_paused = false;

void recomp_time_notify_pause(bool paused) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return;
    int64_t now_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;

    pthread_mutex_lock(&g_time_lock);
    if (paused && !g_time_paused) {
        g_time_paused = true;
        g_paused_at_us = now_us;
    } else if (!paused && g_time_paused) {
        g_time_paused = false;
        int64_t paused_dur = now_us - g_paused_at_us;
        if (paused_dur > 16667LL) {
            g_sleep_offset_us += (paused_dur - 16667LL);
        }
        g_last_real_us = now_us;
    }
    pthread_mutex_unlock(&g_time_lock);
}

void imp_gettimeofday(CPUContext* ctx) {
    uint32_t tvp = arg32(ctx, 0);
    struct timeval tv;
    int rc = gettimeofday(&tv, NULL);
    if (rc == 0 && tvp) {
        pthread_mutex_lock(&g_time_lock);
        int64_t now_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
        if (!g_time_paused && g_last_real_us != 0) {
            int64_t delta_us = now_us - g_last_real_us;
            /* Clamp multi-second wake jumps (e.g. Switch sleep mode or HOME pause) to one 60Hz frame */
            if (delta_us > 1000000LL) {
                g_sleep_offset_us += (delta_us - 16667LL);
            }
        }
        if (!g_time_paused) {
            g_last_real_us = now_us;
        }
        int64_t adj_us = (g_time_paused ? g_paused_at_us : now_us) - g_sleep_offset_us;
        if (adj_us < 0) adj_us = 0;
        pthread_mutex_unlock(&g_time_lock);

        mem_w32(tvp + 0, (uint32_t)(adj_us / 1000000LL));
        mem_w32(tvp + 4, (uint32_t)(adj_us % 1000000LL));
    }
    ctx->eax = (uint32_t)rc;
}

void imp_time(CPUContext* ctx) {
    uint32_t tp = arg32(ctx, 0);
    time_t t = time(NULL);
    if (tp) mem_w32(tp, (uint32_t)t);
    ctx->eax = (uint32_t)t;
}

void imp_clock(CPUContext* ctx) { ctx->eax = (uint32_t)clock(); }
void imp_usleep(CPUContext* ctx) { usleep(arg32(ctx, 0)); ctx->eax = 0; }

static void write_i386_tm(uint32_t p, const struct tm* t) {
    if (!g_tm_zone_str) g_tm_zone_str = guest_strdup("UTC");
    mem_w32(p + 0x00, (uint32_t)t->tm_sec);
    mem_w32(p + 0x04, (uint32_t)t->tm_min);
    mem_w32(p + 0x08, (uint32_t)t->tm_hour);
    mem_w32(p + 0x0C, (uint32_t)t->tm_mday);
    mem_w32(p + 0x10, (uint32_t)t->tm_mon);
    mem_w32(p + 0x14, (uint32_t)t->tm_year);
    mem_w32(p + 0x18, (uint32_t)t->tm_wday);
    mem_w32(p + 0x1C, (uint32_t)t->tm_yday);
    mem_w32(p + 0x20, (uint32_t)t->tm_isdst);
    mem_w32(p + 0x24, 0);
    mem_w32(p + 0x28, g_tm_zone_str);
}

void imp_localtime(CPUContext* ctx) {
    uint32_t tp = arg32(ctx, 0);
    time_t t = tp ? (time_t)(int32_t)mem_r32(tp) : 0;
    struct tm out_tm;
    localtime_r(&t, &out_tm);
    if (!g_tm_buf) g_tm_buf = guest_rt_alloc(48, 4);
    write_i386_tm(g_tm_buf, &out_tm);
    ctx->eax = g_tm_buf;
}

void imp_localtime_r(CPUContext* ctx) {
    uint32_t tp = arg32(ctx, 0);
    uint32_t res = arg32(ctx, 1);
    time_t t = tp ? (time_t)(int32_t)mem_r32(tp) : 0;
    struct tm out_tm;
    if (res && localtime_r(&t, &out_tm)) {
        write_i386_tm(res, &out_tm);
        ctx->eax = res;
    } else {
        ctx->eax = 0;
    }
}

void imp_strftime(CPUContext* ctx) {
    char* dst = mstr32(arg32(ctx, 0));
    size_t max = arg32(ctx, 1);
    const char* fmt = cstr32(arg32(ctx, 2));
    uint32_t tmp = arg32(ctx, 3);
    struct tm t;
    memset(&t, 0, sizeof(t));
    if (tmp) {
        t.tm_sec = (int)mem_r32(tmp + 0x00);
        t.tm_min = (int)mem_r32(tmp + 0x04);
        t.tm_hour = (int)mem_r32(tmp + 0x08);
        t.tm_mday = (int)mem_r32(tmp + 0x0C);
        t.tm_mon = (int)mem_r32(tmp + 0x10);
        t.tm_year = (int)mem_r32(tmp + 0x14);
        t.tm_wday = (int)mem_r32(tmp + 0x18);
        t.tm_yday = (int)mem_r32(tmp + 0x1C);
        t.tm_isdst = (int)mem_r32(tmp + 0x20);
    }
    ctx->eax = (uint32_t)strftime(dst, max, fmt, &t);
}

/* ---------------- Pthreads ---------------- */

#define MAX_PT_MUTEXES 1024
#define MAX_PT_KEYS    64
#define MAX_PT_THREADS 64

static pthread_mutex_t g_pt_mutexes[MAX_PT_MUTEXES];
static bool g_pt_mutex_used[MAX_PT_MUTEXES];
static pthread_mutex_t g_pt_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_pt_keys[MAX_PT_KEYS];
static uint32_t g_pt_key_next = 1;

typedef struct PtThreadLaunch {
    uint32_t start_fn;
    uint32_t arg;
    uint32_t stack_top;
    pthread_t pt;
    bool used;
} PtThreadLaunch;

static PtThreadLaunch g_pt_threads[MAX_PT_THREADS];

void imp_pthread_mutexattr_init(CPUContext* ctx) {
    uint32_t attr = arg32(ctx, 0);
    if (attr) mem_w32(attr, 0);
    ctx->eax = 0;
}
void imp_pthread_mutexattr_destroy(CPUContext* ctx) { (void)ctx; ctx->eax = 0; }
void imp_pthread_mutexattr_settype(CPUContext* ctx) {
    uint32_t attr = arg32(ctx, 0);
    if (attr) mem_w32(attr, arg32(ctx, 1));
    ctx->eax = 0;
}

static int ensure_pt_mutex(uint32_t m_addr) {
    if (!m_addr) return -1;
    uint32_t id = mem_r32(m_addr);
    if (id > 0 && id < MAX_PT_MUTEXES && g_pt_mutex_used[id]) return (int)id;
    pthread_mutex_lock(&g_pt_table_lock);
    id = mem_r32(m_addr);
    if (id > 0 && id < MAX_PT_MUTEXES && g_pt_mutex_used[id]) {
        pthread_mutex_unlock(&g_pt_table_lock);
        return (int)id;
    }
    for (uint32_t i = 1; i < MAX_PT_MUTEXES; i++) {
        if (!g_pt_mutex_used[i]) {
            g_pt_mutex_used[i] = true;
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_pt_mutexes[i], &attr);
            pthread_mutexattr_destroy(&attr);
            mem_w32(m_addr, i);
            pthread_mutex_unlock(&g_pt_table_lock);
            return (int)i;
        }
    }
    pthread_mutex_unlock(&g_pt_table_lock);
    return -1;
}

void imp_pthread_mutex_init(CPUContext* ctx) {
    uint32_t m = arg32(ctx, 0);
    uint32_t attr_p = arg32(ctx, 1);
    int type = attr_p ? (int)mem_r32(attr_p) : 0;
    pthread_mutex_lock(&g_pt_table_lock);
    for (uint32_t i = 1; i < MAX_PT_MUTEXES; i++) {
        if (!g_pt_mutex_used[i]) {
            g_pt_mutex_used[i] = true;
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            if (type == 1) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_pt_mutexes[i], &a);
            pthread_mutexattr_destroy(&a);
            mem_w32(m, i);
            pthread_mutex_unlock(&g_pt_table_lock);
            ctx->eax = 0;
            return;
        }
    }
    pthread_mutex_unlock(&g_pt_table_lock);
    ctx->eax = (uint32_t)ENOMEM;
}

void imp_pthread_mutex_destroy(CPUContext* ctx) {
    uint32_t m = arg32(ctx, 0);
    uint32_t id = m ? mem_r32(m) : 0;
    if (id > 0 && id < MAX_PT_MUTEXES && g_pt_mutex_used[id]) {
        pthread_mutex_destroy(&g_pt_mutexes[id]);
        g_pt_mutex_used[id] = false;
        mem_w32(m, 0);
    }
    ctx->eax = 0;
}

void imp_pthread_mutex_lock(CPUContext* ctx) {
    int id = ensure_pt_mutex(arg32(ctx, 0));
    ctx->eax = (id > 0) ? (uint32_t)pthread_mutex_lock(&g_pt_mutexes[id]) : (uint32_t)EINVAL;
}
void imp_pthread_mutex_trylock(CPUContext* ctx) {
    int id = ensure_pt_mutex(arg32(ctx, 0));
    ctx->eax = (id > 0) ? (uint32_t)pthread_mutex_trylock(&g_pt_mutexes[id]) : (uint32_t)EINVAL;
}
void imp_pthread_mutex_unlock(CPUContext* ctx) {
    int id = ensure_pt_mutex(arg32(ctx, 0));
    ctx->eax = (id > 0) ? (uint32_t)pthread_mutex_unlock(&g_pt_mutexes[id]) : (uint32_t)EINVAL;
}
void imp_pthread_mutex_timedlock(CPUContext* ctx) {
    int id = ensure_pt_mutex(arg32(ctx, 0));
    if (id <= 0) { ctx->eax = (uint32_t)EINVAL; return; }
    uint32_t ts_p = arg32(ctx, 1);
    struct timespec ts;
    ts.tv_sec = ts_p ? (time_t)(int32_t)mem_r32(ts_p) : 0;
    ts.tv_nsec = ts_p ? (long)(int32_t)mem_r32(ts_p + 4) : 0;
#ifdef __SWITCH__
    while (pthread_mutex_trylock(&g_pt_mutexes[id]) != 0) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > ts.tv_sec || (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec)) {
            ctx->eax = (uint32_t)ETIMEDOUT;
            return;
        }
        usleep(1000);
    }
    ctx->eax = 0;
#else
    ctx->eax = (uint32_t)pthread_mutex_timedlock(&g_pt_mutexes[id], &ts);
#endif
}

static void* pt_guest_entry(void* p) {
    PtThreadLaunch* l = (PtThreadLaunch*)p;
#ifdef __SWITCH__
    /* Pin guest pthreads to Core 2 (Core 0 = Main Guest, Core 1 = Async GL)
     * at prio 50: preempts Mesa/perf workers (59), yields to SDL_RunAudio (40). */
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x4))) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7);
    }
    Result prio_rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x32);
    fprintf(stderr, "[pt-guest] fn=0x%08x core=2 prio=50 rc=0x%x\n",
            l->start_fn, (unsigned)prio_rc);
    fflush(stderr);
#endif
    CPUContext tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.fpu_cw = 0x037Fu;
    tctx.esp = l->stack_top - 16u;
    mem_w32(tctx.esp + 0, 0xDEAD0003u);
    mem_w32(tctx.esp + 4, l->arg);
    recomp_icall(&tctx, l->start_fn);
    return (void*)(uintptr_t)tctx.eax;
}

void imp_pthread_create(CPUContext* ctx) {
    uint32_t tid_p = arg32(ctx, 0);
    uint32_t fn = arg32(ctx, 2);
    uint32_t arg = arg32(ctx, 3);
    uint32_t st = guest_alloc_stack(2u * 1024u * 1024u);
    if (!st) { ctx->eax = (uint32_t)ENOMEM; return; }
    pthread_mutex_lock(&g_pt_table_lock);
    int slot = -1;
    for (int i = 1; i < MAX_PT_THREADS; i++) {
        if (!g_pt_threads[i].used) { slot = i; g_pt_threads[i].used = true; break; }
    }
    pthread_mutex_unlock(&g_pt_table_lock);
    if (slot < 0) { ctx->eax = (uint32_t)EAGAIN; return; }
    g_pt_threads[slot].start_fn = fn;
    g_pt_threads[slot].arg = arg;
    g_pt_threads[slot].stack_top = st;
    int rc = pthread_create(&g_pt_threads[slot].pt, NULL, pt_guest_entry, &g_pt_threads[slot]);
    if (rc == 0 && tid_p) mem_w32(tid_p, (uint32_t)slot);
    if (rc != 0) g_pt_threads[slot].used = false;
    ctx->eax = (uint32_t)rc;
}

void imp_pthread_join(CPUContext* ctx) {
    uint32_t id = arg32(ctx, 0);
    uint32_t ret_p = arg32(ctx, 1);
    if (id == 0 || id >= MAX_PT_THREADS || !g_pt_threads[id].used) { ctx->eax = (uint32_t)ESRCH; return; }
    void* rv = NULL;
    int rc = pthread_join(g_pt_threads[id].pt, &rv);
    g_pt_threads[id].used = false;
    if (ret_p) mem_w32(ret_p, (uint32_t)(uintptr_t)rv);
    ctx->eax = (uint32_t)rc;
}

void imp_pthread_key_create(CPUContext* ctx) {
    uint32_t kp = arg32(ctx, 0);
    pthread_mutex_lock(&g_pt_table_lock);
    if (g_pt_key_next >= MAX_PT_KEYS) { pthread_mutex_unlock(&g_pt_table_lock); ctx->eax = (uint32_t)EAGAIN; return; }
    uint32_t id = g_pt_key_next++;
    pthread_key_create(&g_pt_keys[id], NULL);
    pthread_mutex_unlock(&g_pt_table_lock);
    if (kp) mem_w32(kp, id);
    ctx->eax = 0;
}
void imp_pthread_setspecific(CPUContext* ctx) {
    uint32_t id = arg32(ctx, 0);
    ctx->eax = (id < MAX_PT_KEYS) ? (uint32_t)pthread_setspecific(g_pt_keys[id], (const void*)(uintptr_t)arg32(ctx, 1)) : (uint32_t)EINVAL;
}
void imp_pthread_getspecific(CPUContext* ctx) {
    uint32_t id = arg32(ctx, 0);
    ctx->eax = (id < MAX_PT_KEYS) ? (uint32_t)(uintptr_t)pthread_getspecific(g_pt_keys[id]) : 0u;
}

/* ---------------- C++ ABI / Exit / Process / Env ---------------- */

static pthread_mutex_t g_guard_lock = PTHREAD_MUTEX_INITIALIZER;
void imp___cxa_guard_acquire(CPUContext* ctx) {
    uint32_t g = arg32(ctx, 0);
    if (mem_r8(g)) { ctx->eax = 0; return; }
    pthread_mutex_lock(&g_guard_lock);
    if (mem_r8(g)) {
        pthread_mutex_unlock(&g_guard_lock);
        ctx->eax = 0;
        return;
    }
    ctx->eax = 1;
}
void imp___cxa_guard_release(CPUContext* ctx) {
    uint32_t g = arg32(ctx, 0);
    mem_w8(g, 1);
    pthread_mutex_unlock(&g_guard_lock);
}
void imp___cxa_guard_abort(CPUContext* ctx) {
    (void)ctx;
    pthread_mutex_unlock(&g_guard_lock);
}

void imp___cxa_pure_virtual(CPUContext* ctx) {
    (void)ctx;
    fprintf(stderr, "[cpp] Pure virtual call!\n");
    abort();
}
void imp___gxx_personality_v0(CPUContext* ctx) { ctx->eax = 0; }
void imp__Unwind_Resume(CPUContext* ctx) { (void)ctx; fprintf(stderr, "[cpp] _Unwind_Resume hit\n"); abort(); }

typedef struct AtexitEntry { uint32_t fn; uint32_t arg; } AtexitEntry;
static AtexitEntry g_atexit[512];
static int g_atexit_n = 0;

void imp___cxa_atexit(CPUContext* ctx) {
    if (g_atexit_n < 512) {
        g_atexit[g_atexit_n++] = (AtexitEntry){ arg32(ctx, 0), arg32(ctx, 1) };
    }
    ctx->eax = 0;
}
void imp_atexit(CPUContext* ctx) {
    if (g_atexit_n < 512) {
        g_atexit[g_atexit_n++] = (AtexitEntry){ arg32(ctx, 0), 0u };
    }
    ctx->eax = 0;
}
void imp_exit(CPUContext* ctx) { exit((int)arg32(ctx, 0)); }
void imp__exit(CPUContext* ctx) { _exit((int)arg32(ctx, 0)); }
void imp___assert_fail(CPUContext* ctx) {
    fprintf(stderr, "[guest assert] %s (%s:%u in %s)\n",
            cstr32(arg32(ctx, 0)), cstr32(arg32(ctx, 1)), arg32(ctx, 2), cstr32(arg32(ctx, 3)));
    abort();
}
void imp___gmon_start__(CPUContext* ctx) { (void)ctx; }
void imp___libc_start_main(CPUContext* ctx) { (void)ctx; }

void imp_getenv(CPUContext* ctx) {
    const char* name = cstr32(arg32(ctx, 0));
    if (!name) { ctx->eax = 0; return; }
    if (strcmp(name, "HOME") == 0) {
        if (!g_env_str) g_env_str = guest_strdup(recomp_get_game_dir());
        ctx->eax = g_env_str;
        return;
    }
    const char* v = getenv(name);
    ctx->eax = v ? guest_strdup(v) : 0u;
}
void imp_system(CPUContext* ctx) { (void)ctx; ctx->eax = 0xFFFFFFFFu; }
void imp_signal(CPUContext* ctx) { (void)ctx; ctx->eax = 0; }

/* ---------------- Sockets (for DebugTransport) ---------------- */

void imp_socket(CPUContext* ctx) { ctx->eax = (uint32_t)socket((int)arg32(ctx, 0), (int)arg32(ctx, 1), (int)arg32(ctx, 2)); }
void imp_bind(CPUContext* ctx) { ctx->eax = (uint32_t)bind((int)arg32(ctx, 0), (const struct sockaddr*)cstr32(arg32(ctx, 1)), (socklen_t)arg32(ctx, 2)); }
void imp_listen(CPUContext* ctx) { ctx->eax = (uint32_t)listen((int)arg32(ctx, 0), (int)arg32(ctx, 1)); }
void imp_accept(CPUContext* ctx) {
    socklen_t alen = 0;
    uint32_t lp = arg32(ctx, 2);
    if (lp) alen = mem_r32(lp);
    int r = accept((int)arg32(ctx, 0), lp ? (struct sockaddr*)mstr32(arg32(ctx, 1)) : NULL, lp ? &alen : NULL);
    if (lp) mem_w32(lp, (uint32_t)alen);
    ctx->eax = (uint32_t)r;
}
void imp_connect(CPUContext* ctx) { ctx->eax = (uint32_t)connect((int)arg32(ctx, 0), (const struct sockaddr*)cstr32(arg32(ctx, 1)), (socklen_t)arg32(ctx, 2)); }
void imp_send(CPUContext* ctx) { ctx->eax = (uint32_t)send((int)arg32(ctx, 0), cstr32(arg32(ctx, 1)), arg32(ctx, 2), (int)arg32(ctx, 3)); }
void imp_sendto(CPUContext* ctx) {
    ctx->eax = (uint32_t)sendto((int)arg32(ctx, 0), cstr32(arg32(ctx, 1)), arg32(ctx, 2), (int)arg32(ctx, 3),
                                (const struct sockaddr*)cstr32(arg32(ctx, 4)), (socklen_t)arg32(ctx, 5));
}
void imp_recv(CPUContext* ctx) { ctx->eax = (uint32_t)recv((int)arg32(ctx, 0), mstr32(arg32(ctx, 1)), arg32(ctx, 2), (int)arg32(ctx, 3)); }
void imp_recvfrom(CPUContext* ctx) {
    socklen_t fl = 0;
    uint32_t fp = arg32(ctx, 5);
    if (fp) fl = mem_r32(fp);
    ssize_t r = recvfrom((int)arg32(ctx, 0), mstr32(arg32(ctx, 1)), arg32(ctx, 2), (int)arg32(ctx, 3),
                         fp ? (struct sockaddr*)mstr32(arg32(ctx, 4)) : NULL, fp ? &fl : NULL);
    if (fp) mem_w32(fp, (uint32_t)fl);
    ctx->eax = (uint32_t)r;
}
void imp_setsockopt(CPUContext* ctx) {
    ctx->eax = (uint32_t)setsockopt((int)arg32(ctx, 0), (int)arg32(ctx, 1), (int)arg32(ctx, 2),
                                    cstr32(arg32(ctx, 3)), (socklen_t)arg32(ctx, 4));
}
void imp_select(CPUContext* ctx) {
    int nfds = (int)arg32(ctx, 0);
    fd_set* r = (fd_set*)mstr32(arg32(ctx, 1));
    fd_set* w = (fd_set*)mstr32(arg32(ctx, 2));
    fd_set* e = (fd_set*)mstr32(arg32(ctx, 3));
    uint32_t tvp = arg32(ctx, 4);
    struct timeval tv;
    if (tvp) {
        tv.tv_sec = (time_t)(int32_t)mem_r32(tvp);
        tv.tv_usec = (suseconds_t)(int32_t)mem_r32(tvp + 4);
    }
    ctx->eax = (uint32_t)select(nfds, r, w, e, tvp ? &tv : NULL);
}
void imp_htons(CPUContext* ctx) { ctx->eax = htons((uint16_t)arg32(ctx, 0)); }
void imp_inet_addr(CPUContext* ctx) { ctx->eax = (uint32_t)inet_addr(cstr32(arg32(ctx, 0))); }
void imp_gethostname(CPUContext* ctx) { ctx->eax = (uint32_t)gethostname(mstr32(arg32(ctx, 0)), arg32(ctx, 1)); }
void imp_gethostbyname(CPUContext* ctx) { (void)ctx; ctx->eax = 0; }
void imp_gethostbyaddr(CPUContext* ctx) { (void)ctx; ctx->eax = 0; }

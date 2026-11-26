#include "runtime/cpu.h"
#include "runtime/memory.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <stdio.h>
#include <string.h>

#define MAX_ALC_HANDLES 64
static ALCdevice*  g_alc_devs[MAX_ALC_HANDLES];
static ALCcontext* g_alc_ctxs[MAX_ALC_HANDLES];
static uint32_t    g_alc_str_cache[16];
static const char* g_alc_str_keys[16];
static bool        g_al_suspended = false;
static bool        g_al_info_logged = false;
static bool        g_al_first_play_logged = false;

void recomp_openal_set_suspended(bool suspended) {
    if (g_al_suspended == suspended) return;
    g_al_suspended = suspended;
    for (int i = 1; i < MAX_ALC_HANDLES; i++) {
        if (!g_alc_ctxs[i]) continue;
        if (suspended) {
            alcSuspendContext(g_alc_ctxs[i]);
        } else {
            alcProcessContext(g_alc_ctxs[i]);
        }
    }
}

static uint32_t wrap_dev(ALCdevice* d) {
    if (!d) return 0;
    for (int i = 1; i < MAX_ALC_HANDLES; i++) {
        if (g_alc_devs[i] == d) return 0x08750000u + (uint32_t)i * 4u;
    }
    for (int i = 1; i < MAX_ALC_HANDLES; i++) {
        if (!g_alc_devs[i]) {
            g_alc_devs[i] = d;
            return 0x08750000u + (uint32_t)i * 4u;
        }
    }
    return 0;
}

static ALCdevice* unwrap_dev(uint32_t h) {
    if (h < 0x08750004u) return NULL;
    uint32_t idx = (h - 0x08750000u) >> 2;
    return (idx < MAX_ALC_HANDLES) ? g_alc_devs[idx] : NULL;
}

static uint32_t wrap_ctx(ALCcontext* c) {
    if (!c) return 0;
    for (int i = 1; i < MAX_ALC_HANDLES; i++) {
        if (g_alc_ctxs[i] == c) return 0x08751000u + (uint32_t)i * 4u;
    }
    for (int i = 1; i < MAX_ALC_HANDLES; i++) {
        if (!g_alc_ctxs[i]) {
            g_alc_ctxs[i] = c;
            return 0x08751000u + (uint32_t)i * 4u;
        }
    }
    return 0;
}

static ALCcontext* unwrap_ctx(uint32_t h) {
    if (h < 0x08751004u) return NULL;
    uint32_t idx = (h - 0x08751000u) >> 2;
    return (idx < MAX_ALC_HANDLES) ? g_alc_ctxs[idx] : NULL;
}

static inline uint32_t arg32(CPUContext* ctx, int idx) {
    return mem_r32(ctx->esp + (uint32_t)(4 * (idx + 1)));
}

static inline float argf32(CPUContext* ctx, int idx) {
    return mem_rf32(ctx->esp + (uint32_t)(4 * (idx + 1)));
}

static inline const void* ptr32(uint32_t p) {
    return p ? (const void*)(g_mem_base + p) : NULL;
}

static inline void* mptr32(uint32_t p) {
    return p ? (void*)(g_mem_base + p) : NULL;
}

void imp_alcOpenDevice(CPUContext* ctx) {
    const char* name = (const char*)ptr32(arg32(ctx, 0));
    ALCdevice* d = alcOpenDevice(name);
    if (!d && name) d = alcOpenDevice(NULL);
    uint32_t h = wrap_dev(d);
    const ALCchar* spec = d ? alcGetString(d, ALC_DEVICE_SPECIFIER) : NULL;
    const ALCchar* def_spec = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);
    fprintf(stderr, "[al] alcOpenDevice(req=%s) -> h=0x%08x dev=%p spec='%s' default='%s'\n",
            name ? name : "NULL", h, (void*)d,
            spec ? spec : "(null)", def_spec ? def_spec : "(null)");
    fflush(stderr);
    ctx->eax = h;
}

void imp_alcCloseDevice(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    ALCdevice* d = unwrap_dev(h);
    ALCboolean ok = d ? alcCloseDevice(d) : ALC_FALSE;
    if (ok && h >= 0x08750004u) {
        uint32_t idx = (h - 0x08750000u) >> 2;
        if (idx < MAX_ALC_HANDLES) g_alc_devs[idx] = NULL;
    }
    ctx->eax = (uint32_t)ok;
}

void imp_alcCreateContext(CPUContext* ctx) {
    ALCdevice* d = unwrap_dev(arg32(ctx, 0));
    const ALCint* attr = (const ALCint*)ptr32(arg32(ctx, 1));
    ctx->eax = wrap_ctx(d ? alcCreateContext(d, attr) : NULL);
}

void imp_alcDestroyContext(CPUContext* ctx) {
    uint32_t h = arg32(ctx, 0);
    ALCcontext* c = unwrap_ctx(h);
    if (c) alcDestroyContext(c);
    if (h >= 0x08751004u) {
        uint32_t idx = (h - 0x08751000u) >> 2;
        if (idx < MAX_ALC_HANDLES) g_alc_ctxs[idx] = NULL;
    }
}

void imp_alcMakeContextCurrent(CPUContext* ctx) {
    uint32_t ch = arg32(ctx, 0);
    ALCcontext* c = unwrap_ctx(ch);
    ALCboolean ok = alcMakeContextCurrent(c);
    if (c && ok && !g_al_info_logged) {
        g_al_info_logged = true;
        const ALchar* vend = alGetString(AL_VENDOR);
        const ALchar* rend = alGetString(AL_RENDERER);
        const ALchar* ver  = alGetString(AL_VERSION);
        fprintf(stderr, "[al] Context current: vendor='%s' renderer='%s' version='%s'\n",
                vend ? vend : "?", rend ? rend : "?", ver ? ver : "?");
        fflush(stderr);
    }
    ctx->eax = (uint32_t)ok;
}

void imp_alcProcessContext(CPUContext* ctx) {
    ALCcontext* c = unwrap_ctx(arg32(ctx, 0));
    if (c) alcProcessContext(c);
}

void imp_alcSuspendContext(CPUContext* ctx) {
    ALCcontext* c = unwrap_ctx(arg32(ctx, 0));
    if (c) alcSuspendContext(c);
}

void imp_alcGetContextsDevice(CPUContext* ctx) {
    ALCcontext* c = unwrap_ctx(arg32(ctx, 0));
    ctx->eax = c ? wrap_dev(alcGetContextsDevice(c)) : 0;
}

void imp_alcGetString(CPUContext* ctx) {
    ALCdevice* d = unwrap_dev(arg32(ctx, 0));
    ALCenum p = (ALCenum)arg32(ctx, 1);
    const ALCchar* s = alcGetString(d, p);
    if (!s) { ctx->eax = 0; return; }
    for (int i = 0; i < 16; i++) {
        if (g_alc_str_keys[i] == s && g_alc_str_cache[i]) {
            ctx->eax = g_alc_str_cache[i];
            return;
        }
    }
    uint32_t gp = guest_strdup(s);
    for (int i = 0; i < 16; i++) {
        if (!g_alc_str_cache[i]) {
            g_alc_str_keys[i] = s;
            g_alc_str_cache[i] = gp;
            break;
        }
    }
    ctx->eax = gp;
}

void imp_alGetError(CPUContext* ctx) { ctx->eax = (uint32_t)alGetError(); }
void imp_alDistanceModel(CPUContext* ctx) { alDistanceModel((ALenum)arg32(ctx, 0)); }
void imp_alGetEnumValue(CPUContext* ctx) { ctx->eax = (uint32_t)alGetEnumValue((const ALchar*)ptr32(arg32(ctx, 0))); }
void imp_alIsExtensionPresent(CPUContext* ctx) { ctx->eax = (uint32_t)alIsExtensionPresent((const ALchar*)ptr32(arg32(ctx, 0))); }

void imp_alGenBuffers(CPUContext* ctx) { alGenBuffers((ALsizei)arg32(ctx, 0), (ALuint*)mptr32(arg32(ctx, 1))); }
void imp_alDeleteBuffers(CPUContext* ctx) { alDeleteBuffers((ALsizei)arg32(ctx, 0), (const ALuint*)ptr32(arg32(ctx, 1))); }
void imp_alBufferData(CPUContext* ctx) {
    alBufferData((ALuint)arg32(ctx, 0), (ALenum)arg32(ctx, 1), ptr32(arg32(ctx, 2)),
                 (ALsizei)arg32(ctx, 3), (ALsizei)arg32(ctx, 4));
}

void imp_alGenSources(CPUContext* ctx) { alGenSources((ALsizei)arg32(ctx, 0), (ALuint*)mptr32(arg32(ctx, 1))); }
void imp_alDeleteSources(CPUContext* ctx) { alDeleteSources((ALsizei)arg32(ctx, 0), (const ALuint*)ptr32(arg32(ctx, 1))); }
void imp_alSourcePlay(CPUContext* ctx) { alSourcePlay((ALuint)arg32(ctx, 0)); }
void imp_alSourcePlayv(CPUContext* ctx) {
    ALsizei count = (ALsizei)arg32(ctx, 0);
    const ALuint* srcs = (const ALuint*)ptr32(arg32(ctx, 1));
    alSourcePlayv(count, srcs);
    if (__builtin_expect(!g_al_first_play_logged && count > 0 && srcs, 0)) {
        g_al_first_play_logged = true;
        ALint st = 0;
        alGetSourcei(srcs[0], AL_SOURCE_STATE, &st);
        fprintf(stderr, "[al] First playback active (src=%u state=0x%x)\n", (unsigned)srcs[0], (unsigned)st);
        fflush(stderr);
    }
}
void imp_alSourcePause(CPUContext* ctx) { alSourcePause((ALuint)arg32(ctx, 0)); }
void imp_alSourceStop(CPUContext* ctx) { alSourceStop((ALuint)arg32(ctx, 0)); }
void imp_alSourcei(CPUContext* ctx) { alSourcei((ALuint)arg32(ctx, 0), (ALenum)arg32(ctx, 1), (ALint)arg32(ctx, 2)); }
void imp_alSourcef(CPUContext* ctx) { alSourcef((ALuint)arg32(ctx, 0), (ALenum)arg32(ctx, 1), argf32(ctx, 2)); }
void imp_alSourcefv(CPUContext* ctx) { alSourcefv((ALuint)arg32(ctx, 0), (ALenum)arg32(ctx, 1), (const ALfloat*)ptr32(arg32(ctx, 2))); }
void imp_alGetSourcei(CPUContext* ctx) { alGetSourcei((ALuint)arg32(ctx, 0), (ALenum)arg32(ctx, 1), (ALint*)mptr32(arg32(ctx, 2))); }
void imp_alGetSourcef(CPUContext* ctx) { alGetSourcef((ALuint)arg32(ctx, 0), (ALenum)arg32(ctx, 1), (ALfloat*)mptr32(arg32(ctx, 2))); }
void imp_alGetSourcefv(CPUContext* ctx) { alGetSourcefv((ALuint)arg32(ctx, 0), (ALenum)arg32(ctx, 1), (ALfloat*)mptr32(arg32(ctx, 2))); }
void imp_alSourceQueueBuffers(CPUContext* ctx) { alSourceQueueBuffers((ALuint)arg32(ctx, 0), (ALsizei)arg32(ctx, 1), (const ALuint*)ptr32(arg32(ctx, 2))); }
void imp_alSourceUnqueueBuffers(CPUContext* ctx) { alSourceUnqueueBuffers((ALuint)arg32(ctx, 0), (ALsizei)arg32(ctx, 1), (ALuint*)mptr32(arg32(ctx, 2))); }

void imp_alListenerf(CPUContext* ctx) { alListenerf((ALenum)arg32(ctx, 0), argf32(ctx, 1)); }
void imp_alListenerfv(CPUContext* ctx) { alListenerfv((ALenum)arg32(ctx, 0), (const ALfloat*)ptr32(arg32(ctx, 1))); }
void imp_alGetListenerf(CPUContext* ctx) { alGetListenerf((ALenum)arg32(ctx, 0), (ALfloat*)mptr32(arg32(ctx, 1))); }
void imp_alGetListenerfv(CPUContext* ctx) { alGetListenerfv((ALenum)arg32(ctx, 0), (ALfloat*)mptr32(arg32(ctx, 1))); }

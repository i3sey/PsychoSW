#include "runtime/cpu.h"
#include "runtime/memory.h"
#include "runtime/settings.h"
#include "runtime/vfs.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <AL/al.h>
#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t arg32(CPUContext* ctx, int idx) {
    return mem_r32(ctx->esp + (uint32_t)(4 * (idx + 1)));
}

/* Guest BINK struct offsets observed in GameApp::playVideoBink:
 * +0x00: Width (uint32_t)
 * +0x04: Height (uint32_t)
 * +0x08: Frames (uint32_t)
 * +0x0c: FrameNum (uint32_t, 1-based)
 * +0x10: LastFrameNum (uint32_t)
 * +0x14: FrameRateDiv (uint32_t)
 * +0x18: FrameRate (uint32_t)
 * +0xd0: NumTracks (int32_t)
 */
#define GUEST_BINK_STRUCT_SIZE 256

typedef struct BinkPlayer {
    uint32_t guest_struct_ptr;
    AVFormatContext* fmt;
    AVCodecContext* vctx;
    AVCodecContext* actx;
    AVFormatContext* ext_afmt;
    AVCodecContext* ext_actx;
    AVPacket* ext_apkt;
    int ext_astream;
    bool ext_aeof;
    struct SwsContext* sws;
    struct SwrContext* swr;
    AVPacket* pkt;
    AVFrame* frame;
    AVFrame* audio_frame;
    uint8_t* pcm_buf;
    int pcm_buf_cap;
    int vstream;
    int astream;
    uint32_t width;
    uint32_t height;
    uint32_t frames;
    uint32_t frame_num;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t start_ticks;
    uint32_t next_frame_ticks;
    double ext_audio_pts;
    bool decoded_current;
    bool eof;
    ALuint al_source;
    ALuint al_buffers[8];
    int al_next_buf;
    int al_queued;
    int volume;
} BinkPlayer;

#define MAX_BINK_PLAYERS 8
static BinkPlayer* g_binks[MAX_BINK_PLAYERS];
static uint32_t g_selected_track_id = 0;

static BinkPlayer* find_bink(uint32_t guest_ptr) {
    for (int i = 0; i < MAX_BINK_PLAYERS; i++) {
        if (g_binks[i] && g_binks[i]->guest_struct_ptr == guest_ptr)
            return g_binks[i];
    }
    return NULL;
}

static void sync_guest_bink(BinkPlayer* bp) {
    uint32_t p = bp->guest_struct_ptr;
    mem_w32(p + 0x00u, bp->width);
    mem_w32(p + 0x04u, bp->height);
    mem_w32(p + 0x08u, bp->frames ? bp->frames : 1u);
    mem_w32(p + 0x0Cu, bp->frame_num);
    mem_w32(p + 0x10u, bp->frame_num);
    mem_w32(p + 0x14u, bp->fps_den ? bp->fps_den : 1u);
    mem_w32(p + 0x18u, bp->fps_num ? bp->fps_num : 30u);
    mem_w32(p + 0xD0u, bp->astream >= 0 ? 1u : 0u);
}

static void reclaim_audio_buffers(BinkPlayer* bp) {
    if (bp->al_source == 0) return;
    ALint processed = 0;
    alGetSourcei(bp->al_source, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0) {
        ALuint bid = 0;
        alSourceUnqueueBuffers(bp->al_source, 1, &bid);
        if (bp->al_queued > 0) bp->al_queued--;
        processed--;
    }
}

static void pump_audio_frame(BinkPlayer* bp, AVFrame* af) {
    if (bp->al_source == 0) return;
    reclaim_audio_buffers(bp);
    if (bp->al_queued >= 8) return;

    int out_samples = af->nb_samples;
    AVCodecContext* ac = bp->ext_actx ? bp->ext_actx : bp->actx;
    int out_rate = (ac && ac->sample_rate > 0) ? ac->sample_rate : 44100;
    int buf_bytes = out_samples * 2 * 2; /* stereo S16 */
    if (buf_bytes > bp->pcm_buf_cap) {
        uint8_t* nb = (uint8_t*)realloc(bp->pcm_buf, (size_t)buf_bytes);
        if (!nb) return;
        bp->pcm_buf = nb;
        bp->pcm_buf_cap = buf_bytes;
    }

    uint8_t* out_planes[1] = { bp->pcm_buf };
    int got = swr_convert(bp->swr, out_planes, out_samples,
                          (const uint8_t**)af->extended_data, af->nb_samples);
    if (got > 0) {
        ALuint bid = bp->al_buffers[bp->al_next_buf];
        bp->al_next_buf = (bp->al_next_buf + 1) % 8;
        alBufferData(bid, AL_FORMAT_STEREO16, bp->pcm_buf, got * 4, out_rate);
        alSourceQueueBuffers(bp->al_source, 1, &bid);
        bp->al_queued++;
        ALint st = 0;
        alGetSourcei(bp->al_source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) alSourcePlay(bp->al_source);
    }
}

static void pump_ext_audio_to_video_time(BinkPlayer* bp) {
    if (!bp->ext_afmt || !bp->ext_actx || !bp->swr || bp->ext_aeof) return;
    reclaim_audio_buffers(bp);
    double fps = (bp->fps_num && bp->fps_den) ? ((double)bp->fps_num / (double)bp->fps_den) : 30.0;
    double target_sec = (double)bp->frame_num / fps + 0.15;
    int rate = bp->ext_actx->sample_rate > 0 ? bp->ext_actx->sample_rate : 44100;
    while (!bp->ext_aeof && bp->ext_audio_pts < target_sec && bp->al_queued < 7) {
        int r = av_read_frame(bp->ext_afmt, bp->ext_apkt);
        if (r < 0) {
            bp->ext_aeof = true;
            break;
        }
        if (bp->ext_apkt->stream_index == bp->ext_astream) {
            if (avcodec_send_packet(bp->ext_actx, bp->ext_apkt) == 0) {
                while (avcodec_receive_frame(bp->ext_actx, bp->audio_frame) == 0) {
                    bp->ext_audio_pts += (double)bp->audio_frame->nb_samples / (double)rate;
                    pump_audio_frame(bp, bp->audio_frame);
                }
            }
        }
        av_packet_unref(bp->ext_apkt);
    }
}

static bool decode_next_video_frame(BinkPlayer* bp) {
    if (bp->eof) return false;
    pump_ext_audio_to_video_time(bp);
    while (1) {
        int r = avcodec_receive_frame(bp->vctx, bp->frame);
        if (r == 0) {
            return true;
        }
        if (r != AVERROR(EAGAIN)) {
            bp->eof = true;
            return false;
        }
        int rd = av_read_frame(bp->fmt, bp->pkt);
        if (rd < 0) {
            avcodec_send_packet(bp->vctx, NULL);
            bp->eof = true;
            continue;
        }
        if (bp->pkt->stream_index == bp->vstream) {
            avcodec_send_packet(bp->vctx, bp->pkt);
        } else if (!bp->ext_afmt && bp->pkt->stream_index == bp->astream && bp->actx && bp->swr) {
            if (avcodec_send_packet(bp->actx, bp->pkt) == 0) {
                while (avcodec_receive_frame(bp->actx, bp->audio_frame) == 0) {
                    pump_audio_frame(bp, bp->audio_frame);
                }
            }
        }
        av_packet_unref(bp->pkt);
    }
}

void imp_BinkSetSoundSystem(CPUContext* ctx) {
    ctx->eax = 1;
}

void imp_BinkSetSoundTrack(CPUContext* ctx) {
    uint32_t total = arg32(ctx, 0);
    uint32_t tracks_ptr = arg32(ctx, 1);
    if (total > 0 && tracks_ptr) {
        g_selected_track_id = mem_r32(tracks_ptr);
    }
    ctx->eax = 1;
}

static void bink_to_ffmpeg_path(const char* host_path, char* out, size_t out_sz) {
#ifdef __SWITCH__
    /* FFmpeg treats "sdmc:/..." as a URL protocol "sdmc" and fails with
     * AVERROR_PROTOCOL_NOT_FOUND. Prefixing "file:" forces the file protocol
     * so libnx/newlib open("sdmc:/...") is called directly. */
    if (strncmp(host_path, "sdmc:/", 6) == 0 || strncmp(host_path, "romfs:/", 7) == 0) {
        snprintf(out, out_sz, "file:%s", host_path);
        return;
    }
#endif
    snprintf(out, out_sz, "%s", host_path);
}

void imp_BinkOpen(CPUContext* ctx) {
    uint32_t name_ptr = arg32(ctx, 0);
    uint32_t flags = arg32(ctx, 1);
    (void)flags;
    if (!name_ptr) { ctx->eax = 0; return; }

    const char* guest_name = (const char*)(g_mem_base + name_ptr);
    char host_path[1024];
    if (!vfs_resolve_path(guest_name, host_path, sizeof(host_path), false)) {
        snprintf(host_path, sizeof(host_path), "%s", guest_name);
    }

    char ff_path[1040];
    bink_to_ffmpeg_path(host_path, ff_path, sizeof(ff_path));

    AVFormatContext* fmt = NULL;
    int open_rc = avformat_open_input(&fmt, ff_path, NULL, NULL);
    if (open_rc < 0) {
        char errbuf[128];
        av_strerror(open_rc, errbuf, sizeof(errbuf));
        fprintf(stderr, "[bink] Failed to open %s (resolved: %s, ff: %s): %s\n",
                guest_name, host_path, ff_path, errbuf);
        ctx->eax = 0;
        return;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        ctx->eax = 0;
        return;
    }

    int vstream = -1, astream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vstream < 0)
            vstream = (int)i;
        else if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && astream < 0)
            astream = (int)i;
    }
    if (vstream < 0) {
        avformat_close_input(&fmt);
        ctx->eax = 0;
        return;
    }

    AVStream* vst = fmt->streams[vstream];
    const AVCodec* vdec = avcodec_find_decoder(vst->codecpar->codec_id);
    if (!vdec) {
        avformat_close_input(&fmt);
        ctx->eax = 0;
        return;
    }
    AVCodecContext* vctx = avcodec_alloc_context3(vdec);
    avcodec_parameters_to_context(vctx, vst->codecpar);
    vctx->thread_count = 2;
    vctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
    if (avcodec_open2(vctx, vdec, NULL) < 0) {
        avcodec_free_context(&vctx);
        avformat_close_input(&fmt);
        ctx->eax = 0;
        return;
    }

    BinkPlayer* bp = (BinkPlayer*)calloc(1, sizeof(BinkPlayer));
    bp->fmt = fmt;
    bp->vctx = vctx;
    bp->vstream = vstream;
    bp->astream = astream;
    bp->width = (uint32_t)vctx->width;
    bp->height = (uint32_t)vctx->height;
    uint32_t bik_frames = 0;
    {
        FILE* bfp = fopen(host_path, "rb");
        if (bfp) {
            uint8_t bhdr[12];
            if (fread(bhdr, 1, sizeof(bhdr), bfp) == sizeof(bhdr) &&
                bhdr[0] == 'B' && bhdr[1] == 'I' && bhdr[2] == 'K') {
                bik_frames = (uint32_t)bhdr[8] |
                             ((uint32_t)bhdr[9] << 8) |
                             ((uint32_t)bhdr[10] << 16) |
                             ((uint32_t)bhdr[11] << 24);
            }
            fclose(bfp);
        }
    }
    bp->frames = (bik_frames > 0) ? bik_frames : (uint32_t)(vst->nb_frames > 0 ? vst->nb_frames : 300);
    bp->frame_num = 1;
    AVRational fr = vst->avg_frame_rate.num > 0 ? vst->avg_frame_rate : vst->r_frame_rate;
    bp->fps_num = (uint32_t)(fr.num > 0 ? fr.num : 30);
    bp->fps_den = (uint32_t)(fr.den > 0 ? fr.den : 1);
    if (bp->fps_den > 10000u && bp->fps_num > 0u) {
        double fps = (double)bp->fps_num / (double)bp->fps_den;
        bp->fps_num = (uint32_t)(fps * 1000.0 + 0.5);
        bp->fps_den = 1000u;
    }
    bp->pkt = av_packet_alloc();
    bp->frame = av_frame_alloc();
    bp->audio_frame = av_frame_alloc();

    bp->sws = sws_getContext((int)bp->width, (int)bp->height, vctx->pix_fmt,
                             (int)bp->width, (int)bp->height, AV_PIX_FMT_BGRA,
                             SWS_FAST_BILINEAR, NULL, NULL, NULL);

    /* Optional Russian MP3 dub sidecar (e.g. WorkResource/cutscenes_Russian/prerendered/INTRO.mp3) */
    bool opened_ext_audio = false;
    if (recomp_settings_get()->voice_lang == RECOMP_VOICE_RU_BUKA) {
        const char* base = strrchr(guest_name, '/');
        const char* bsl = strrchr(guest_name, '\\');
        if (bsl && (!base || bsl > base)) base = bsl;
        base = base ? (base + 1) : guest_name;
        char stem[256];
        snprintf(stem, sizeof(stem), "%s", base);
        char* dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        char mp3_rel[512], mp3_host[1024];
        snprintf(mp3_rel, sizeof(mp3_rel), "WorkResource/cutscenes_Russian/prerendered/%s.mp3", stem);
        if (vfs_resolve_path(mp3_rel, mp3_host, sizeof(mp3_host), false)) {
            char mp3_ff[1040];
            bink_to_ffmpeg_path(mp3_host, mp3_ff, sizeof(mp3_ff));
            AVFormatContext* efmt = NULL;
            if (avformat_open_input(&efmt, mp3_ff, NULL, NULL) == 0 &&
                avformat_find_stream_info(efmt, NULL) >= 0) {
                int eas = -1;
                for (unsigned i = 0; i < efmt->nb_streams; i++) {
                    if (efmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { eas = (int)i; break; }
                }
                if (eas >= 0) {
                    const AVCodec* adec = avcodec_find_decoder(efmt->streams[eas]->codecpar->codec_id);
                    AVCodecContext* eactx = adec ? avcodec_alloc_context3(adec) : NULL;
                    if (eactx && avcodec_parameters_to_context(eactx, efmt->streams[eas]->codecpar) >= 0 &&
                        avcodec_open2(eactx, adec, NULL) == 0) {
                        bp->ext_afmt = efmt;
                        bp->ext_actx = eactx;
                        bp->ext_astream = eas;
                        bp->ext_apkt = av_packet_alloc();
                        AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
                        swr_alloc_set_opts2(&bp->swr, &out_ch, AV_SAMPLE_FMT_S16,
                                            eactx->sample_rate > 0 ? eactx->sample_rate : 44100,
                                            &eactx->ch_layout, eactx->sample_fmt,
                                            eactx->sample_rate > 0 ? eactx->sample_rate : 44100,
                                            0, NULL);
                        if (bp->swr) swr_init(bp->swr);
                        alGenSources(1, &bp->al_source);
                        alGenBuffers(8, bp->al_buffers);
                        opened_ext_audio = true;
                        fprintf(stderr, "[bink] Using Russian MP3 dub track: %s\n", mp3_host);
                    } else {
                        if (eactx) avcodec_free_context(&eactx);
                        avformat_close_input(&efmt);
                    }
                } else {
                    avformat_close_input(&efmt);
                }
            }
        }
    }

    if (!opened_ext_audio && astream >= 0) {
        AVStream* ast = fmt->streams[astream];
        const AVCodec* adec = avcodec_find_decoder(ast->codecpar->codec_id);
        if (adec) {
            bp->actx = avcodec_alloc_context3(adec);
            avcodec_parameters_to_context(bp->actx, ast->codecpar);
            if (avcodec_open2(bp->actx, adec, NULL) == 0) {
                AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(&bp->swr, &out_ch, AV_SAMPLE_FMT_S16,
                                    bp->actx->sample_rate > 0 ? bp->actx->sample_rate : 44100,
                                    &bp->actx->ch_layout, bp->actx->sample_fmt,
                                    bp->actx->sample_rate > 0 ? bp->actx->sample_rate : 44100,
                                    0, NULL);
                if (bp->swr) swr_init(bp->swr);
                alGenSources(1, &bp->al_source);
                alGenBuffers(8, bp->al_buffers);
            } else {
                avcodec_free_context(&bp->actx);
            }
        }
    }

    bp->guest_struct_ptr = guest_calloc(1, GUEST_BINK_STRUCT_SIZE);
    bp->start_ticks = SDL_GetTicks();
    bp->next_frame_ticks = bp->start_ticks;
    sync_guest_bink(bp);

    for (int i = 0; i < MAX_BINK_PLAYERS; i++) {
        if (!g_binks[i]) { g_binks[i] = bp; break; }
    }
    fprintf(stderr, "[bink] Opened %s (%ux%u, %u frames, %u/%u fps)\n",
            host_path, bp->width, bp->height, bp->frames, bp->fps_num, bp->fps_den);
    ctx->eax = bp->guest_struct_ptr;
}

void imp_BinkClose(CPUContext* ctx) {
    uint32_t ptr = arg32(ctx, 0);
    BinkPlayer* bp = find_bink(ptr);
    if (!bp) return;
    fprintf(stderr, "[bink] Closed (frame %u/%u)\n", bp->frame_num, bp->frames);
    for (int i = 0; i < MAX_BINK_PLAYERS; i++) {
        if (g_binks[i] == bp) { g_binks[i] = NULL; break; }
    }
    if (bp->al_source) {
        alSourceStop(bp->al_source);
        alSourcei(bp->al_source, AL_BUFFER, 0);
        alDeleteSources(1, &bp->al_source);
        alDeleteBuffers(8, bp->al_buffers);
    }
    if (bp->swr) swr_free(&bp->swr);
    if (bp->sws) sws_freeContext(bp->sws);
    if (bp->audio_frame) av_frame_free(&bp->audio_frame);
    if (bp->frame) av_frame_free(&bp->frame);
    free(bp->pcm_buf);
    if (bp->ext_apkt) av_packet_free(&bp->ext_apkt);
    if (bp->ext_actx) avcodec_free_context(&bp->ext_actx);
    if (bp->ext_afmt) avformat_close_input(&bp->ext_afmt);
    if (bp->pkt) av_packet_free(&bp->pkt);
    if (bp->actx) avcodec_free_context(&bp->actx);
    if (bp->vctx) avcodec_free_context(&bp->vctx);
    if (bp->fmt) avformat_close_input(&bp->fmt);
    guest_free(bp->guest_struct_ptr);
    free(bp);
}

void imp_BinkWait(CPUContext* ctx) {
    BinkPlayer* bp = find_bink(arg32(ctx, 0));
    if (!bp) { ctx->eax = 0; return; }
    uint32_t now = SDL_GetTicks();
    ctx->eax = (now < bp->next_frame_ticks) ? 1u : 0u;
}

void imp_BinkDoFrame(CPUContext* ctx) {
    BinkPlayer* bp = find_bink(arg32(ctx, 0));
    if (!bp) { ctx->eax = 0; return; }
    if (!bp->decoded_current) {
        decode_next_video_frame(bp);
        bp->decoded_current = true;
        if (bp->frame_num == 1 || bp->frame_num == 30 || bp->frame_num == 120 || bp->frame_num == bp->frames) {
            fprintf(stderr, "[bink] Frame %u/%u\n", bp->frame_num, bp->frames);
        }
    }
    uint32_t frame_ms = (uint32_t)(((uint64_t)bp->fps_den * 1000ULL) / (uint64_t)(bp->fps_num ? bp->fps_num : 30u));
    if (frame_ms == 0) frame_ms = 33u;
    bp->next_frame_ticks += frame_ms;
    ctx->eax = 0;
}

void imp_BinkNextFrame(CPUContext* ctx) {
    BinkPlayer* bp = find_bink(arg32(ctx, 0));
    if (!bp) return;
    if (bp->frame_num < bp->frames) {
        bp->frame_num++;
    } else {
        /* Loop cleanly if caller restarts or didn't break */
        av_seek_frame(bp->fmt, bp->vstream, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(bp->vctx);
        if (bp->ext_afmt) {
            av_seek_frame(bp->ext_afmt, bp->ext_astream, 0, AVSEEK_FLAG_BACKWARD);
            if (bp->ext_actx) avcodec_flush_buffers(bp->ext_actx);
            bp->ext_aeof = false;
            bp->ext_audio_pts = 0.0;
        }
        bp->eof = false;
        bp->frame_num = 1;
    }
    bp->decoded_current = false;
    sync_guest_bink(bp);
}

void imp_BinkCopyToBuffer(CPUContext* ctx) {
    BinkPlayer* bp = find_bink(arg32(ctx, 0));
    uint32_t dest_ptr = arg32(ctx, 1);
    int32_t dest_pitch = (int32_t)arg32(ctx, 2);
    uint32_t dest_h = arg32(ctx, 3);
    uint32_t dest_x = arg32(ctx, 4);
    uint32_t dest_y = arg32(ctx, 5);
    uint32_t flags = arg32(ctx, 6);
    (void)flags;

    if (!bp || !dest_ptr || !bp->frame || !bp->frame->data[0] || !bp->sws) { ctx->eax = 0; return; }
    uint8_t* dst = g_mem_base + dest_ptr + dest_y * (uint32_t)dest_pitch + dest_x * 4u;
    int copy_h = (int)(bp->height < dest_h ? bp->height : dest_h);
    uint8_t* dst_planes[1] = { dst };
    int dst_linesizes[1] = { dest_pitch };
    sws_scale(bp->sws,
              (const uint8_t* const*)bp->frame->data, bp->frame->linesize,
              0, copy_h,
              dst_planes, dst_linesizes);
    ctx->eax = 0;
}

void imp_BinkGetTrackID(CPUContext* ctx) {
    BinkPlayer* bp = find_bink(arg32(ctx, 0));
    uint32_t track_idx = arg32(ctx, 1);
    ctx->eax = (bp && bp->astream >= 0 && track_idx == 0) ? 0u : 0xFFFFFFFFu;
}

void imp_BinkSetVolume(CPUContext* ctx) {
    BinkPlayer* bp = find_bink(arg32(ctx, 0));
    int32_t vol = (int32_t)arg32(ctx, 2);
    if (bp && bp->al_source) {
        float g = (float)vol / 32768.0f;
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        alSourcef(bp->al_source, AL_GAIN, g);
    }
}

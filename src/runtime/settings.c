#define _GNU_SOURCE
#include "runtime/settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static RecompSettings g_settings = {
    .ui_lang = RECOMP_UI_RU,
    .text_lang = RECOMP_TEXT_RU_PATCHED,
    .voice_lang = RECOMP_VOICE_RU_BUKA,
    .button_icons = RECOMP_ICONS_SWITCH,
    .gyro_mode = RECOMP_GYRO_AIM_MED,
    .aniso_level = 16,
    .res_mode = RECOMP_RES_AUTO,
    .fxaa = false,
    .fps_cap = 60,
    .show_fps = false,
    .invert_cam_x = false,
    .invert_cam_y = false,
    .stick_deadzone = 10,
    .swap_ab = false,
    .subtitles = true,
    .rumble = true,
    .widescreen = true,
    .configured_once = false,
};

static bool file_exists_ci_approx(const char* dir, const char* rel) {
    char p[1024];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    return stat(p, &st) == 0;
}

static bool parse_bool_str(const char* v, bool def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
        strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0) {
        return true;
    }
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0) {
        return false;
    }
    return def;
}

static RecompUiLang parse_ui_lang(const char* v, RecompUiLang def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "ru") == 0 || strcasecmp(v, "russian") == 0) {
        return RECOMP_UI_RU;
    }
    if (strcasecmp(v, "en") == 0 || strcasecmp(v, "english") == 0) {
        return RECOMP_UI_EN;
    }
    return def;
}

static RecompTextLang parse_text_lang(const char* v, RecompTextLang def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "ru_patched") == 0 || strcasecmp(v, "ru1") == 0 ||
        strcasecmp(v, "ru") == 0 || strcasecmp(v, "patched") == 0) {
        return RECOMP_TEXT_RU_PATCHED;
    }
    if (strcasecmp(v, "ru_buka") == 0 || strcasecmp(v, "ru2") == 0 ||
        strcasecmp(v, "buka") == 0) {
        return RECOMP_TEXT_RU_BUKA;
    }
    if (strcasecmp(v, "en") == 0 || strcasecmp(v, "english") == 0) {
        return RECOMP_TEXT_EN;
    }
    return def;
}

static RecompVoiceLang parse_voice_lang(const char* v, RecompVoiceLang def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "ru_buka") == 0 || strcasecmp(v, "ru") == 0 ||
        strcasecmp(v, "russian") == 0 || strcasecmp(v, "buka") == 0) {
        return RECOMP_VOICE_RU_BUKA;
    }
    if (strcasecmp(v, "en") == 0 || strcasecmp(v, "english") == 0) {
        return RECOMP_VOICE_EN;
    }
    return def;
}

static RecompButtonIcons parse_icons(const char* v, RecompButtonIcons def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "switch") == 0 || strcasecmp(v, "nx") == 0) return RECOMP_ICONS_SWITCH;
    if (strcasecmp(v, "xbox") == 0 || strcasecmp(v, "xb") == 0)   return RECOMP_ICONS_XBOX;
    if (strcasecmp(v, "pc") == 0)                                 return RECOMP_ICONS_PC;
    return def;
}

static RecompGyroMode parse_gyro(const char* v, RecompGyroMode def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "off") == 0 || strcasecmp(v, "0") == 0 ||
        strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0) {
        return RECOMP_GYRO_OFF;
    }
    if (strcasecmp(v, "low") == 0)    return RECOMP_GYRO_AIM_LOW;
    if (strcasecmp(v, "medium") == 0 || strcasecmp(v, "med") == 0 ||
        strcasecmp(v, "aim") == 0 || strcasecmp(v, "1") == 0 ||
        strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0) {
        return RECOMP_GYRO_AIM_MED;
    }
    if (strcasecmp(v, "high") == 0)   return RECOMP_GYRO_AIM_HIGH;
    if (strcasecmp(v, "always") == 0 || strcasecmp(v, "all") == 0) {
        return RECOMP_GYRO_ALWAYS;
    }
    return def;
}

static int parse_aniso(const char* v, int def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0) {
        return 0;
    }
    int val = atoi(v);
    if (val <= 0) return 0;
    if (val <= 8) return 8;
    return 16;
}

static int parse_fps_cap(const char* v, int def) {
    if (!v || !*v) return def;
    int val = atoi(v);
    if (val > 0 && val <= 35) return 30;
    if (val > 35) return 60;
    return def;
}

static int parse_deadzone(const char* v, int def) {
    if (!v || !*v) return def;
    int val = atoi(v);
    if (val < 5) val = 5;
    if (val > 25) val = 25;
    val = ((val + 2) / 5) * 5;
    return val;
}

static RecompResMode parse_res(const char* v, RecompResMode def) {
    if (!v || !*v) return def;
    if (strcasecmp(v, "auto") == 0) return RECOMP_RES_AUTO;
    if (strcasecmp(v, "720") == 0 || strcasecmp(v, "720p") == 0 || strcasecmp(v, "1280x720") == 0) {
        return RECOMP_RES_720P;
    }
    if (strcasecmp(v, "1080") == 0 || strcasecmp(v, "1080p") == 0 || strcasecmp(v, "1920x1080") == 0) {
        return RECOMP_RES_1080P;
    }
    return def;
}

static void trim_ws(char* s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void probe_capabilities(const char* game_dir) {
    g_settings.has_ru_patched = file_exists_ci_approx(
        game_dir, "WorkResource/Localization/Russian_Patched/GLOBAL_StringTable.lub");
    g_settings.has_ru_buka = file_exists_ci_approx(
        game_dir, "WorkResource/Localization/Russian_Buka/GLOBAL_StringTable.lub");
    g_settings.has_ru_voice = file_exists_ci_approx(
        game_dir, "WorkResource/Sounds_Russian/commonvoice.isb");
    g_settings.has_sw_icons = file_exists_ci_approx(
        game_dir, "WorkResource/Textures/Icons/UI_Icons/IconPrefsSwitch.ini");
    g_settings.has_xb_icons = file_exists_ci_approx(
        game_dir, "WorkResource/Textures/Icons/UI_Icons/IconPrefsXbox.ini");
}

static void clamp_to_available_assets(void) {
    if (g_settings.text_lang == RECOMP_TEXT_RU_PATCHED && !g_settings.has_ru_patched) {
        g_settings.text_lang = g_settings.has_ru_buka ? RECOMP_TEXT_RU_BUKA : RECOMP_TEXT_EN;
    }
    if (g_settings.text_lang == RECOMP_TEXT_RU_BUKA && !g_settings.has_ru_buka) {
        g_settings.text_lang = g_settings.has_ru_patched ? RECOMP_TEXT_RU_PATCHED : RECOMP_TEXT_EN;
    }
    if (g_settings.voice_lang == RECOMP_VOICE_RU_BUKA && !g_settings.has_ru_voice) {
        g_settings.voice_lang = RECOMP_VOICE_EN;
    }
    if (g_settings.button_icons == RECOMP_ICONS_SWITCH && !g_settings.has_sw_icons) {
        g_settings.button_icons = g_settings.has_xb_icons ? RECOMP_ICONS_XBOX : RECOMP_ICONS_PC;
    }
    if (g_settings.button_icons == RECOMP_ICONS_XBOX && !g_settings.has_xb_icons) {
        g_settings.button_icons = g_settings.has_sw_icons ? RECOMP_ICONS_SWITCH : RECOMP_ICONS_PC;
    }
}

void recomp_settings_load(const char* game_dir) {
    const char* dir = (game_dir && *game_dir) ? game_dir : ".";
    probe_capabilities(dir);

    g_settings.ui_lang = (g_settings.has_ru_patched || g_settings.has_ru_buka)
                       ? RECOMP_UI_RU : RECOMP_UI_EN;
    g_settings.text_lang = g_settings.has_ru_patched ? RECOMP_TEXT_RU_PATCHED
                         : (g_settings.has_ru_buka ? RECOMP_TEXT_RU_BUKA : RECOMP_TEXT_EN);
    g_settings.voice_lang = g_settings.has_ru_voice ? RECOMP_VOICE_RU_BUKA : RECOMP_VOICE_EN;
    g_settings.button_icons = g_settings.has_sw_icons ? RECOMP_ICONS_SWITCH
                            : (g_settings.has_xb_icons ? RECOMP_ICONS_XBOX : RECOMP_ICONS_PC);
    g_settings.gyro_mode = RECOMP_GYRO_AIM_MED;
    g_settings.aniso_level = 16;
    g_settings.res_mode = RECOMP_RES_AUTO;
    g_settings.fxaa = false;
    g_settings.fps_cap = 60;
    g_settings.show_fps = false;
    g_settings.invert_cam_x = false;
    g_settings.invert_cam_y = false;
    g_settings.stick_deadzone = 10;
    g_settings.swap_ab = false;
    g_settings.subtitles = true;
    g_settings.rumble = true;
    g_settings.widescreen = true;
    g_settings.configured_once = false;

    char path[1024];
    snprintf(path, sizeof(path), "%s/RecompSettings.ini", dir);
    FILE* fp = fopen(path, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            trim_ws(line);
            if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char* k = line;
            char* v = eq + 1;
            trim_ws(k);
            trim_ws(v);
            if (strcasecmp(k, "ui_lang") == 0) {
                g_settings.ui_lang = parse_ui_lang(v, g_settings.ui_lang);
            } else if (strcasecmp(k, "text_lang") == 0) {
                g_settings.text_lang = parse_text_lang(v, g_settings.text_lang);
            } else if (strcasecmp(k, "voice_lang") == 0) {
                g_settings.voice_lang = parse_voice_lang(v, g_settings.voice_lang);
            } else if (strcasecmp(k, "button_icons") == 0) {
                g_settings.button_icons = parse_icons(v, g_settings.button_icons);
            } else if (strcasecmp(k, "gyro") == 0) {
                g_settings.gyro_mode = parse_gyro(v, g_settings.gyro_mode);
            } else if (strcasecmp(k, "anisotropic") == 0) {
                g_settings.aniso_level = parse_aniso(v, g_settings.aniso_level);
            } else if (strcasecmp(k, "resolution") == 0) {
                g_settings.res_mode = parse_res(v, g_settings.res_mode);
            } else if (strcasecmp(k, "fxaa") == 0) {
                g_settings.fxaa = parse_bool_str(v, g_settings.fxaa);
            } else if (strcasecmp(k, "fps_cap") == 0) {
                g_settings.fps_cap = parse_fps_cap(v, g_settings.fps_cap);
            } else if (strcasecmp(k, "show_fps") == 0) {
                g_settings.show_fps = parse_bool_str(v, g_settings.show_fps);
            } else if (strcasecmp(k, "invert_cam_x") == 0) {
                g_settings.invert_cam_x = parse_bool_str(v, g_settings.invert_cam_x);
            } else if (strcasecmp(k, "invert_cam_y") == 0) {
                g_settings.invert_cam_y = parse_bool_str(v, g_settings.invert_cam_y);
            } else if (strcasecmp(k, "stick_deadzone") == 0) {
                g_settings.stick_deadzone = parse_deadzone(v, g_settings.stick_deadzone);
            } else if (strcasecmp(k, "swap_ab") == 0) {
                g_settings.swap_ab = parse_bool_str(v, g_settings.swap_ab);
            } else if (strcasecmp(k, "subtitles") == 0) {
                g_settings.subtitles = parse_bool_str(v, g_settings.subtitles);
            } else if (strcasecmp(k, "rumble") == 0) {
                g_settings.rumble = parse_bool_str(v, g_settings.rumble);
            } else if (strcasecmp(k, "widescreen") == 0) {
                g_settings.widescreen = parse_bool_str(v, g_settings.widescreen);
            } else if (strcasecmp(k, "configured_once") == 0) {
                g_settings.configured_once = parse_bool_str(v, false);
            }
        }
        fclose(fp);
    }

    const char* env_u = getenv("RECOMP_UI_LANG");
    if (env_u && *env_u) g_settings.ui_lang = parse_ui_lang(env_u, g_settings.ui_lang);
    const char* env_t = getenv("RECOMP_TEXT_LANG");
    if (env_t && *env_t) g_settings.text_lang = parse_text_lang(env_t, g_settings.text_lang);
    const char* env_v = getenv("RECOMP_VOICE_LANG");
    if (env_v && *env_v) g_settings.voice_lang = parse_voice_lang(env_v, g_settings.voice_lang);
    const char* env_i = getenv("RECOMP_ICONS");
    if (env_i && *env_i) g_settings.button_icons = parse_icons(env_i, g_settings.button_icons);
    const char* env_g = getenv("RECOMP_GYRO");
    if (env_g && *env_g) g_settings.gyro_mode = parse_gyro(env_g, g_settings.gyro_mode);
    const char* env_a = getenv("RECOMP_ANISO");
    if (env_a && *env_a) g_settings.aniso_level = parse_aniso(env_a, g_settings.aniso_level);
    const char* env_res = getenv("RECOMP_RES");
    if (env_res && *env_res) g_settings.res_mode = parse_res(env_res, g_settings.res_mode);
    const char* env_fxaa = getenv("RECOMP_FXAA");
    if (env_fxaa && *env_fxaa) g_settings.fxaa = parse_bool_str(env_fxaa, g_settings.fxaa);
    const char* env_fps = getenv("RECOMP_FPS_CAP");
    if (env_fps && *env_fps) g_settings.fps_cap = parse_fps_cap(env_fps, g_settings.fps_cap);
    const char* env_sfps = getenv("RECOMP_SHOW_FPS");
    if (env_sfps && *env_sfps) g_settings.show_fps = parse_bool_str(env_sfps, g_settings.show_fps);
    const char* env_ix = getenv("RECOMP_INVERT_CAM_X");
    if (env_ix && *env_ix) g_settings.invert_cam_x = parse_bool_str(env_ix, g_settings.invert_cam_x);
    const char* env_iy = getenv("RECOMP_INVERT_CAM_Y");
    if (env_iy && *env_iy) g_settings.invert_cam_y = parse_bool_str(env_iy, g_settings.invert_cam_y);
    const char* env_dz = getenv("RECOMP_DEADZONE");
    if (env_dz && *env_dz) g_settings.stick_deadzone = parse_deadzone(env_dz, g_settings.stick_deadzone);
    const char* env_ab = getenv("RECOMP_SWAP_AB");
    if (env_ab && *env_ab) g_settings.swap_ab = parse_bool_str(env_ab, g_settings.swap_ab);
    const char* env_s = getenv("RECOMP_SUBTITLES");
    if (env_s && *env_s) g_settings.subtitles = parse_bool_str(env_s, g_settings.subtitles);
    const char* env_r = getenv("RECOMP_RUMBLE");
    if (env_r && *env_r) g_settings.rumble = parse_bool_str(env_r, g_settings.rumble);
    const char* env_w = getenv("RECOMP_WIDESCREEN");
    if (env_w && *env_w) g_settings.widescreen = parse_bool_str(env_w, g_settings.widescreen);

    clamp_to_available_assets();

    fprintf(stderr, "[settings] ui=%d text=%d voice=%d icons=%d gyro=%d aniso=%d res=%d fxaa=%d fps=%d show_fps=%d subtitles=%d rumble=%d widescreen=%d (avail: pat=%d buka=%d snd=%d sw=%d xb=%d)\n",
            (int)g_settings.ui_lang,
            (int)g_settings.text_lang, (int)g_settings.voice_lang,
            (int)g_settings.button_icons, (int)g_settings.gyro_mode,
            g_settings.aniso_level, (int)g_settings.res_mode,
            (int)g_settings.fxaa, g_settings.fps_cap, (int)g_settings.show_fps,
            (int)g_settings.subtitles, (int)g_settings.rumble, (int)g_settings.widescreen,
            (int)g_settings.has_ru_patched, (int)g_settings.has_ru_buka, (int)g_settings.has_ru_voice,
            (int)g_settings.has_sw_icons, (int)g_settings.has_xb_icons);
}

static const char* ui_lang_str(RecompUiLang u) {
    return (u == RECOMP_UI_EN) ? "en" : "ru";
}

static const char* text_lang_str(RecompTextLang t) {
    switch (t) {
        case RECOMP_TEXT_RU_PATCHED: return "ru_patched";
        case RECOMP_TEXT_RU_BUKA:    return "ru_buka";
        default:                     return "en";
    }
}

static const char* voice_lang_str(RecompVoiceLang v) {
    return (v == RECOMP_VOICE_RU_BUKA) ? "ru_buka" : "en";
}

static const char* icons_str(RecompButtonIcons b) {
    switch (b) {
        case RECOMP_ICONS_SWITCH: return "switch";
        case RECOMP_ICONS_XBOX:   return "xbox";
        default:                  return "pc";
    }
}

static const char* gyro_str(RecompGyroMode g) {
    switch (g) {
        case RECOMP_GYRO_OFF:      return "off";
        case RECOMP_GYRO_AIM_LOW:  return "low";
        case RECOMP_GYRO_AIM_HIGH: return "high";
        case RECOMP_GYRO_ALWAYS:   return "always";
        default:                   return "medium";
    }
}

static const char* res_str(RecompResMode r) {
    switch (r) {
        case RECOMP_RES_720P:  return "720p";
        case RECOMP_RES_1080P: return "1080p";
        default:               return "auto";
    }
}

bool recomp_settings_save(const char* game_dir) {
    const char* dir = (game_dir && *game_dir) ? game_dir : ".";
    char path[1024];
    snprintf(path, sizeof(path), "%s/RecompSettings.ini", dir);
    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    fprintf(fp,
            "[RecompSettings]\n"
            "ui_lang=%s\n"
            "text_lang=%s\n"
            "voice_lang=%s\n"
            "button_icons=%s\n"
            "gyro=%s\n"
            "anisotropic=%d\n"
            "resolution=%s\n"
            "fxaa=%s\n"
            "fps_cap=%d\n"
            "show_fps=%s\n"
            "invert_cam_x=%s\n"
            "invert_cam_y=%s\n"
            "stick_deadzone=%d\n"
            "swap_ab=%s\n"
            "subtitles=%s\n"
            "rumble=%s\n"
            "widescreen=%s\n"
            "configured_once=%s\n",
            ui_lang_str(g_settings.ui_lang),
            text_lang_str(g_settings.text_lang),
            voice_lang_str(g_settings.voice_lang),
            icons_str(g_settings.button_icons),
            gyro_str(g_settings.gyro_mode),
            g_settings.aniso_level,
            res_str(g_settings.res_mode),
            g_settings.fxaa ? "true" : "false",
            g_settings.fps_cap,
            g_settings.show_fps ? "true" : "false",
            g_settings.invert_cam_x ? "true" : "false",
            g_settings.invert_cam_y ? "true" : "false",
            g_settings.stick_deadzone,
            g_settings.swap_ab ? "true" : "false",
            g_settings.subtitles ? "true" : "false",
            g_settings.rumble ? "true" : "false",
            g_settings.widescreen ? "true" : "false",
            g_settings.configured_once ? "true" : "false");
    fclose(fp);
    return true;
}

const RecompSettings* recomp_settings_get(void) {
    return &g_settings;
}

RecompSettings* recomp_settings_mut(void) {
    return &g_settings;
}

#ifndef RECOMP_SETTINGS_H
#define RECOMP_SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RecompUiLang {
    RECOMP_UI_RU = 0,
    RECOMP_UI_EN = 1
} RecompUiLang;

typedef enum RecompTextLang {
    RECOMP_TEXT_RU_PATCHED = 0,
    RECOMP_TEXT_RU_BUKA    = 1,
    RECOMP_TEXT_EN         = 2
} RecompTextLang;

typedef enum RecompVoiceLang {
    RECOMP_VOICE_RU_BUKA = 0,
    RECOMP_VOICE_EN      = 1
} RecompVoiceLang;

typedef enum RecompButtonIcons {
    RECOMP_ICONS_SWITCH = 0,
    RECOMP_ICONS_XBOX   = 1,
    RECOMP_ICONS_PC     = 2
} RecompButtonIcons;

typedef enum RecompGyroMode {
    RECOMP_GYRO_OFF      = 0,
    RECOMP_GYRO_AIM_LOW  = 1,
    RECOMP_GYRO_AIM_MED  = 2,
    RECOMP_GYRO_AIM_HIGH = 3,
    RECOMP_GYRO_ALWAYS   = 4
} RecompGyroMode;

typedef enum RecompResMode {
    RECOMP_RES_AUTO  = 0,
    RECOMP_RES_720P  = 1,
    RECOMP_RES_1080P = 2
} RecompResMode;

typedef struct RecompSettings {
    RecompUiLang      ui_lang;
    RecompTextLang    text_lang;
    RecompVoiceLang   voice_lang;
    RecompButtonIcons button_icons;
    RecompGyroMode    gyro_mode;
    int               aniso_level;
    RecompResMode     res_mode;
    bool              fxaa;
    int               fps_cap;
    bool              show_fps;
    bool              invert_cam_x;
    bool              invert_cam_y;
    int               stick_deadzone;
    bool              swap_ab;
    bool              subtitles;
    bool              rumble;
    bool              widescreen;
    bool              configured_once;

    /* Detected asset capabilities in game_dir */
    bool has_ru_patched;
    bool has_ru_buka;
    bool has_ru_voice;
    bool has_sw_icons;
    bool has_xb_icons;
} RecompSettings;

void recomp_settings_load(const char* game_dir);
bool recomp_settings_save(const char* game_dir);
const RecompSettings* recomp_settings_get(void);
RecompSettings* recomp_settings_mut(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_SETTINGS_H */

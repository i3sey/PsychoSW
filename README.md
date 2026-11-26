# PsychoSW

[Русский](README.ru.md) · [GitHub Releases](https://github.com/i3sey/PsychoSW/releases)

An unofficial static recompilation project targeting the original Nintendo Switch with a Steam Linux copy of Psychonauts.

## Switch features

- **Language and audio.** The launcher and quick settings support English and Russian. Game text and voice can be selected independently. With compatible local files, the launcher offers Buka's Russian dub and localized cutscenes, plus Buka or HSE/ZoG Russian text. The Steam setup below uses the original English resources. Russian localization files are supplied separately by the player.
- **Controls.** On the default layout, B jumps, Y attacks, X uses an item, L locks on, and R/ZL/ZR activate PSI powers. The launcher can swap A/B and select Switch, Xbox, or PC button prompts when the corresponding assets are available. Press X on the profile name screen to open the Switch keyboard.
- **Gyro and controller settings.** SixAxis aiming can activate on demand or also control the camera. The launcher offers axis inversion, a configurable stick deadzone, HD Rumble, and a subtitles toggle.
- **Display and graphics.** Choose a 16:9 HUD and menus or the original 4:3 layout. Auto resolution uses 720p handheld and 1080p docked, with fixed modes available. Anisotropic texture filtering has Off, 8× and 16× settings. FXAA is an optional post-processing pass that smooths edges.
- **Frame rate and quick settings.** Choose a 30 or 60 FPS cap and enable the FPS and frame-time overlay. Press **L + R + −** during gameplay to adjust common settings. The app saves settings in `RecompSettings.ini` and caches compiled shaders on the microSD card.
- **Switch runtime.** Graphics commands run on a dedicated thread. OpenAL audio pauses when the console returns to HOME or goes to sleep and resumes when the game regains focus.

## Switch performance

- **Overclocked CPU (1963.5 MHz)** — 57.6–59.6 FPS near the cabins at Whispering Rock in a handheld test. Performance varies with the camera angle and activity in the scene.
- **Stock clocks** — around 40 FPS reported. A matching capture with recorded stock clocks is still pending.

![Gameplay at a cabin door with 62.7 FPS on the overlay](screenshots/switch-camp-cabins.jpg)

The cabin-door capture shows an instantaneous 62.7 FPS (15.9 ms) during a separate test with reduced graphics effects. Clock speeds were not recorded alongside this capture.

![Camp panorama with 39.7 FPS on the overlay](screenshots/switch-camp-panorama.jpg)

The heavier camp panorama shows 39.7 FPS (25.2 ms) in a separate 720p handheld run with the CPU at 1963.5 MHz.

## Release contents

The first release provides an SD preparation tool and these instructions. You need a compatible `Psychonauts.nro` from a local build and your own Steam installation. The public repository includes the ELF-to-C tool. A complete public build pipeline and a ready-to-play NRO are unavailable in this release. The lifter output alone lacks the runtime and integration work needed to build the Switch app.

## Prepare the microSD card

1. On Linux, install [Psychonauts on Steam](https://store.steampowered.com/app/3830/Psychonauts/) using the native Linux build. If you forced Proton in **Properties → Compatibility**, disable it and let Steam install the Linux files.
2. In Steam, open **Properties → Installed Files → Browse**. Find the directory with `Psychonauts` (the Linux ELF executable), `PsychonautsData2.pkg`, and `WorkResource`. Use a clean Steam installation. The expected SHA-256 of `Psychonauts` is `ec674eed6dac7045b4877dd22f36aa9088e43e58d81268c3a423273f1502d445`. On Linux, run `sha256sum "/path/to/Psychonauts/Psychonauts"` to check it.
3. Download and extract the release ZIP. Open a terminal in the extracted directory and run the following command with Python 3. Replace both input paths with your own. Choose an output directory that does not exist yet.

   ```bash
   python3 prepare_switch_sd.py --game "/path/to/Steam/steamapps/common/Psychonauts" --nro "/path/to/local/Psychonauts.nro" --output sd-card
   ```

4. Copy the generated `sd-card/switch/` folder to the root of the microSD card. If the card already has a `switch` directory, merge the folders and keep a backup of any existing Psychonauts saves before replacing files. The finished layout should look like this.

   ```text
   switch/
   ├── Psychonauts.nro
   └── psychonauts/
       ├── Psychonauts
       ├── PsychonautsData2.pkg
       └── WorkResource/
           ├── Sounds/
           └── SAVEDGAMES/
   ```

5. On a Switch already configured for homebrew, hold **R** while starting an installed title to open Homebrew Menu with full application memory. Select **Psychonauts**. The Album opens Homebrew Menu in applet mode with limited memory.
6. In the launcher select original English game text and voices. The launcher interface can use either English or Russian. Press **L + R + −** in-game for the quick settings menu. If the game fails to start, check `switch/psychonauts/recomp.log`. If sound is missing, verify that the original `.isb` files are present in `WorkResource/Sounds`.

## Local ELF-to-C translation

Install Python dependencies in a virtual environment with `python3 -m pip install capstone pyelftools`. Use a fresh output directory so you preserve any existing generated code and manual optimizations.

```bash
python3 tools/recomp/lift.py --elf "/path/to/Psychonauts/Psychonauts" --output /path/to/lifted-c
```

The tool targets the Linux executable revision above. The translated code is for local use. See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY.md) for the scope of the project's MIT license.

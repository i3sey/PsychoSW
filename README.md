# pshySW

[Русский](README.ru.md) · [GitHub Releases](https://github.com/i3sey/pshySW/releases)

An unofficial static recompilation project targeting the original Nintendo Switch with a Steam Linux copy of Psychonauts.

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

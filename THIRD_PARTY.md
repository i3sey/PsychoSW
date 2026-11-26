# Third-party notices / Сторонние компоненты

The MIT license applies to the original project runtime, tools, and documentation in this repository.
Psychonauts game code, assets, artwork and audio remain the property of their respective
rights holders. Psychonauts is a game by Double Fine Productions. This is an unofficial
homebrew project. Nintendo Switch is a Nintendo trademark.

Лицензия MIT распространяется на оригинальный код среды выполнения, инструменты и документацию проекта
в этом репозитории. Права на код, ресурсы, изображения и звук Psychonauts принадлежат
их правообладателям. Psychonauts разработана Double Fine Productions. Это неофициальный
homebrew-проект. Nintendo Switch — товарный знак Nintendo.

The lifter uses separately installed Capstone and pyelftools. Their licenses apply
to those packages. The SD preparation tool uses the Python standard library.
Утилита трансляции использует отдельно устанавливаемые Capstone и pyelftools
на условиях их лицензий. Утилита подготовки SD-карты использует стандартную библиотеку Python.

- [Capstone license](https://github.com/capstone-engine/capstone/tree/next/LICENSES)
- [pyelftools license](https://github.com/eliben/pyelftools/blob/main/LICENSE)

The gameplay screenshots depict Psychonauts and are included as performance
documentation. Rights to the depicted game content remain with their respective
holders.

Скриншоты Psychonauts приведены для документирования производительности. Права на
изображённые материалы игры принадлежат их правообладателям.

The local build generates the font atlas from a system font using Pillow and
compiles against libraries provided by the devkitPro Switch toolchain, including
libnx, SDL2, Mesa, OpenAL Soft and FFmpeg. The pinned toolchain links OpenAL Soft
and FFmpeg statically and builds FFmpeg with `--enable-gpl`. Its library licenses
apply in addition to the project MIT license.

Локальная сборка генерирует атлас из системного шрифта с помощью Pillow и
использует библиотеки devkitPro для Switch, включая libnx, SDL2, Mesa, OpenAL Soft
и FFmpeg. Зафиксированная версия тулчейна статически линкует OpenAL Soft и FFmpeg,
причём FFmpeg собран с `--enable-gpl`. К библиотекам применяются их собственные
лицензии.

- [devkitPro Switch portlibs](https://github.com/devkitPro/pacman-packages/tree/master/switch)
- [DejaVu fonts license](https://dejavu-fonts.github.io/License.html)
- [FFmpeg licensing](https://www.ffmpeg.org/legal.html)
- [OpenAL Soft license](https://github.com/kcat/openal-soft/blob/master/COPYING)

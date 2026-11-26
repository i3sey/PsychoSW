#!/usr/bin/env python3
"""Build a local Switch NRO from Steam game files and prepare an SD-card tree."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from prepare_switch_sd import prepare, validate_inputs


ROOT = Path(__file__).resolve().parents[1]
DEVKITA64_IMAGE = (
    "devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282"
)
DEFAULT_FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")


def build(game, output, font, jobs):
    executable, _, _ = validate_inputs(game, output)

    with TemporaryDirectory(prefix="psychosw-build-") as directory:
        stage = Path(directory)
        runtime = stage / "src/runtime"
        shutil.copytree(ROOT / "src/runtime", runtime,
                        ignore=shutil.ignore_patterns("launcher_font_atlas.h"))
        shutil.copy2(ROOT / "CMakeLists.txt", stage)

        subprocess.run([
            sys.executable, ROOT / "tools/gen_launcher_font.py",
            "--font", font, "--output", runtime / "launcher_font_atlas.h",
        ], check=True)
        subprocess.run([
            sys.executable, ROOT / "tools/recomp/lift.py",
            "--elf", executable, "--output", stage / "src/gen",
        ], check=True)

        # Mount only the temporary build tree. Steam files stay outside Docker.
        docker = [
            "docker", "run", "--rm", "--network", "none",
            "--user", f"{stage.stat().st_uid}:{stage.stat().st_gid}",
            "--mount", f"type=bind,source={stage},target=/work",
            "--workdir", "/work", DEVKITA64_IMAGE,
        ]
        subprocess.run([
            *docker, "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
            "-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake",
            "-DCMAKE_BUILD_TYPE=Release", "-DPSYCHOSW_USE_GAME_ICON=OFF",
        ], check=True)
        subprocess.run([*docker, "cmake", "--build", "build", "--parallel", str(jobs)],
                       check=True)
        prepare(game, stage / "build/Psychonauts.nro", output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True, help="Steam Linux installation folder")
    parser.add_argument("--output", type=Path, required=True, help="New folder for the SD-card tree")
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT, help="Cyrillic-capable TrueType font")
    parser.add_argument("--jobs", type=int, default=2, help="Parallel compile jobs (default: 2)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    try:
        build(args.game.resolve(), args.output.absolute(), args.font.resolve(), args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    print(f"SD-card files prepared at {args.output / 'switch'}")


if __name__ == "__main__":
    main()

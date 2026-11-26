#!/usr/bin/env python3
"""Prepare an SD-card folder from a local Steam installation and a local build."""

import argparse
import hashlib
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory


SUPPORTED_ELF = "ec674eed6dac7045b4877dd22f36aa9088e43e58d81268c3a423273f1502d445"


def prepare(game, nro, output):
    if game.resolve() in (output.resolve(), *output.resolve().parents):
        raise ValueError("Choose an output directory outside the Steam installation.")
    if output.exists() or output.is_symlink():
        raise ValueError(f"Choose a new output directory: {output}")

    executable = game / "Psychonauts"
    package = game / "PsychonautsData2.pkg"
    resources = game / "WorkResource"
    if not (executable.is_file() and package.is_file() and resources.is_dir()):
        raise ValueError("Select the Steam Linux installation folder containing "
                         "Psychonauts, PsychonautsData2.pkg and WorkResource.")
    if hashlib.sha256(executable.read_bytes()).hexdigest() != SUPPORTED_ELF:
        raise ValueError("Unsupported Psychonauts executable. See the README for the expected SHA-256.")
    if not any((resources / "Sounds").glob("*.isb")):
        raise ValueError("WorkResource/Sounds is missing the original .isb sound banks.")
    with nro.open("rb") as stream:
        stream.seek(16)
        if stream.read(4) != b"NRO0":
            raise ValueError("The supplied file does not have a Switch NRO header.")

    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=".pshysw-", dir=output.parent) as temp:
        staging = Path(temp) / "sd"
        destination = staging / "switch" / "psychonauts"
        destination.mkdir(parents=True)
        shutil.copy2(nro, destination.parent / "Psychonauts.nro")
        shutil.copy2(executable, destination)
        shutil.copy2(package, destination)
        original_resources = (
            "Fonts", "Localization", "PCLevelPackFiles", "PixelShaders",
            "Scripts", "Sounds", "Textures", "VertexShaders", "cutscenes",
        )
        for name in original_resources:
            source = resources / name
            if source.is_dir():
                shutil.copytree(source, destination / "WorkResource" / name)
        saves = destination / "WorkResource" / "SAVEDGAMES"
        saves.mkdir(parents=True)
        (saves / "savegameprefs.ini").write_text(
            "-- Savegame directory ignore list\nplayer.stats\nsaved\n", encoding="ascii"
        )
        staging.rename(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True, help="Steam Linux installation folder")
    parser.add_argument("--nro", type=Path, required=True, help="Locally built Psychonauts.nro")
    parser.add_argument("--output", type=Path, required=True, help="New folder for the SD-card tree")
    args = parser.parse_args()
    try:
        prepare(args.game, args.nro, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"SD-card files prepared at {args.output / 'switch'}")


if __name__ == "__main__":
    main()

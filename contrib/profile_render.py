#! python3

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


WORLD_NAME = "TwentySix_Three"
PROGRESS_RE = re.compile(r"Rendered\s+(\d+)\s+of\s+(\d+)")


def next_output_directory(output_root):
    pattern = re.compile(r"^%s(?:-(\d+))?$" % re.escape(WORLD_NAME))
    highest = 0
    for path in output_root.iterdir() if output_root.exists() else ():
        if not path.is_dir():
            continue
        match = pattern.match(path.name)
        if match:
            highest = max(highest, int(match.group(1) or 0))
    return output_root / ("%s-%d" % (WORLD_NAME, highest + 1))


def find_texture_path(repository, requested):
    candidates = [
        requested,
        os.environ.get("TMO_TEXTURE_PATH"),
        repository / "tmp" / "minecraft-26.3-client.jar",
        Path.home() / ".minecraft" / "versions" / "26.3" / "26.3.jar",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate).resolve()
    raise SystemExit(
        "A Minecraft 26.3 client jar is required. Pass --texturepath PATH "
        "or set TMO_TEXTURE_PATH."
    )


def write_config(path, world, output, texture):
    path.write_text(
        "worlds[%r] = %r\n"
        "outputdir = %r\n"
        "texturepath = %r\n"
        "renders['overworld'] = {\n"
        "    'world': %r,\n"
        "    'title': %r,\n"
        "    'dimension': 'overworld',\n"
        "    'rendermode': 'normal',\n"
        "}\n"
        % (
            WORLD_NAME,
            str(world),
            str(output),
            str(texture),
            WORLD_NAME,
            WORLD_NAME + " Overworld",
        ),
        encoding="utf-8",
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Profile a numbered TwentySix_Three overworld render.")
    parser.add_argument(
        "--python", type=Path, default=Path(".venv/bin/python3.15"),
        help="Python executable containing profiling.sampling.")
    parser.add_argument(
        "--texturepath", type=Path,
        help="Minecraft 26.3 client jar or resource pack.")
    parser.add_argument(
        "--processes", type=int,
        help="Worker process count; defaults to Overviewer's CPU count.")
    parser.add_argument(
        "--sampling-rate", default="100hz",
        help="Sampling rate passed to profiling.sampling (default: 100hz).")
    return parser.parse_args()


def main():
    args = parse_args()
    repository = Path(__file__).resolve().parents[1]
    world = repository / "tmp" / WORLD_NAME
    output_root = repository / "tmp" / "out"
    output = next_output_directory(output_root)
    texture = find_texture_path(repository, args.texturepath)
    #python = (repository / args.python).resolve() if not args.python.is_absolute() else args.python
    python = "python"

    #if not python.is_file():
    #    raise SystemExit("Python executable not found: %s" % python)
    if not (world / "level.dat").is_file():
        raise SystemExit("World not found: %s" % world)

    output_root.mkdir(parents=True, exist_ok=True)
    profile_root = repository / "tmp" / "profiles"
    profile_root.mkdir(parents=True, exist_ok=True)
    flamegraph = profile_root / (output.name + ".html")

    with tempfile.TemporaryDirectory(prefix="tmo-profile-") as temp_directory:
        config = Path(temp_directory) / "overviewer_config.py"
        write_config(config, world.resolve(), output.resolve(), texture)

        command = [
            str(python),
            "-m", "profiling.sampling", "run",
            "--subprocesses",
            "--native",
            "--mode", "cpu",
            "-r", args.sampling_rate,
            "--flamegraph",
            "-o", str(flamegraph),
            str(repository / "overviewer.py"),
            "--simple-output",
            "--config=" + str(config),
        ]
        if args.processes is not None:
            command.extend(["--processes", str(args.processes)])

        print("Output directory: %s" % output)
        print("Flamegraph: %s" % flamegraph)
        print("Command: %s" % " ".join(command))
        print()

        started = time.perf_counter()
        process = subprocess.Popen(
            command,
            cwd=repository,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        completed_operations = total_operations = 0
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="")
            match = PROGRESS_RE.search(line)
            if match:
                completed_operations = int(match.group(1))
                total_operations = int(match.group(2))
        return_code = process.wait()
        elapsed = time.perf_counter() - started

    if return_code != 0:
        raise SystemExit("Render command failed with exit code %d" % return_code)
    if not (output / "index.html").is_file():
        raise SystemExit("Render did not produce %s" % (output / "index.html"))
    if completed_operations != total_operations or total_operations == 0:
        raise SystemExit("Could not confirm a completed render operation count")

    print()
    print("Performance summary")
    print("-------------------")
    print("Output:             %s" % output)
    print("Elapsed:            %.3f seconds" % elapsed)
    print("Render operations:  %d" % completed_operations)
    print("Operations/second:  %.3f" % (completed_operations / elapsed))
    print("Seconds/operation:  %.6f" % (elapsed / completed_operations))
    print("Flamegraph:         %s" % flamegraph)


if __name__ == "__main__":
    sys.exit(main())

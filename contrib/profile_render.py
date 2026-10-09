#! python3

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


DEFAULT_WORLD = "TwentySix_Three"
PROGRESS_RE = re.compile(r"Rendered\s+(\d+)\s+of\s+(\d+)")


def next_output_directory(output_root, name):
    pattern = re.compile(r"^%s(?:-(\d+))?$" % re.escape(name))
    highest = 0
    for path in output_root.iterdir() if output_root.exists() else ():
        if not path.is_dir():
            continue
        match = pattern.match(path.name)
        if match:
            highest = max(highest, int(match.group(1) or 0))
    return output_root / ("%s-%d" % (name, highest + 1))


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


def write_config(path, name, world, output, texture):
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
            name,
            str(world),
            str(output),
            str(texture),
            name,
            name + " Overworld",
        ),
        encoding="utf-8",
    )


def write_wrapped_config(path, config, world, output, texture):
    # Run the given config unchanged, then point its world(s), output and
    # textures at the profiling inputs.
    path.write_text(
        "exec(compile(open(%r, 'rb').read(), %r, 'exec'), globals(), locals())\n"
        "for _world_name in list(worlds):\n"
        "    worlds[_world_name] = %r\n"
        "outputdir = %r\n"
        "texturepath = %r\n"
        % (str(config), str(config), str(world), str(output), str(texture)),
        encoding="utf-8",
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Profile a numbered render of a test world.")
    parser.add_argument(
        "--world", type=Path,
        help="World directory (containing level.dat); defaults to tmp/%s." % DEFAULT_WORLD)
    parser.add_argument(
        "--name",
        help="Name for the output and profile files; defaults to the world "
             "directory's name.")
    parser.add_argument(
        "--config", type=Path,
        help="Overviewer config whose renders to profile (for example a "
             "production config); its worlds, outputdir and texturepath are "
             "replaced. Defaults to a single normal-mode overworld render.")
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
    parser.add_argument(
        "--profiler", choices=("sampling", "perf"), default="sampling",
        help="sampling: Python's profiling.sampling flamegraph (default). "
             "perf: Linux perf record of the whole process tree, for "
             "profiling the C extension.")
    parser.add_argument(
        "--perf-frequency", type=int, default=999,
        help="Samples per second per CPU for --profiler perf (default: 999).")
    return parser.parse_args()


def main():
    args = parse_args()
    repository = Path(__file__).resolve().parents[1]
    world = (args.world or repository / "tmp" / DEFAULT_WORLD).resolve()
    name = args.name or world.name
    output_root = repository / "tmp" / "out"
    output = next_output_directory(output_root, name)
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
    if args.profiler == "perf":
        profile = profile_root / (output.name + ".perf.data")
    else:
        profile = profile_root / (output.name + ".html")

    with tempfile.TemporaryDirectory(prefix="tmo-profile-") as temp_directory:
        config = Path(temp_directory) / "overviewer_config.py"
        if args.config:
            write_wrapped_config(config, args.config.resolve(), world,
                                 output.resolve(), texture)
        else:
            write_config(config, name, world, output.resolve(), texture)

        environment = None
        if args.profiler == "perf":
            # -X perf (inherited by workers through PYTHONPERFSUPPORT) makes
            # Python frames visible in perf's call graphs.
            environment = dict(os.environ, PYTHONPERFSUPPORT="1")
            command = [
                "perf", "record",
                "-F", str(args.perf_frequency),
                "--call-graph", "fp",
                "-o", str(profile),
                "--",
                str(python), "-X", "perf",
            ]
        else:
            command = [
                str(python),
                "-m", "profiling.sampling", "run",
                "--subprocesses",
                "--native",
                "--mode", "cpu",
                "-r", args.sampling_rate,
                "--flamegraph",
                "-o", str(profile),
            ]
        command += [
            str(repository / "overviewer.py"),
            "--simple-output",
            "--config=" + str(config),
        ]
        if args.processes is not None:
            command.extend(["--processes", str(args.processes)])

        print("Output directory: %s" % output)
        print("Profile: %s" % profile)
        print("Command: %s" % " ".join(command))
        print()

        started = time.perf_counter()
        process = subprocess.Popen(
            command,
            cwd=repository,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        completed_operations = total_operations = 0
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="", flush=True)
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
    print("Profile:            %s" % profile)


if __name__ == "__main__":
    sys.exit(main())

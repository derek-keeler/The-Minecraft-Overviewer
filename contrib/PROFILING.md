# Profiling renders

`contrib/profile_render.py` runs a complete Overviewer render of a test world,
optionally under a profiler, and prints how long it took. It is meant for
finding where render time goes and for checking that a change makes renders
faster without changing the output.

Each run writes to a new numbered directory, so earlier results are kept:

- the render: `tmp/out/<name>-<n>/`
- the profile, if any: `tmp/profiles/<name>-<n>.html` or `.perf.data`

At the end it prints a summary:

```
Performance summary
-------------------
Output:             tmp/out/TwentySix_Three-3
Elapsed:            23.871 seconds
Render operations:  4762
Operations/second:  199.489
Seconds/operation:  0.005013
```

## Requirements

- A Minecraft 26.3 client jar for textures. Pass `--texturepath PATH`, set
  `TMO_TEXTURE_PATH`, or put it at `tmp/minecraft-26.3-client.jar` or
  `~/.minecraft/versions/26.3/26.3.jar`.
- A world to render. By default the script looks for `tmp/TwentySix_Three`;
  pass `--world PATH` (the directory containing `level.dat`) for any other.
- The built C extension, as for any render.
- For `--profiler sampling` (the default): Python 3.15 or later, which has
  `profiling.sampling`. Pass `--python PATH` to run Overviewer with a
  different Python than the script's own.
- For `--profiler perf`: Linux `perf`. Profiling unprivileged processes
  normally needs `kernel.perf_event_paranoid` at 2 or lower.

## Usage

Render one normal-mode overworld render of the default test world, with the
sampling profiler:

```
python contrib/profile_render.py --texturepath ~/minecraft/26.3.jar
```

Time a render without a profiler:

```
python contrib/profile_render.py --profiler none --world ~/worlds/MyWorld
```

Render an existing config's renders instead of the single default render. The
config is run unchanged, except that its worlds, `outputdir` and
`texturepath` are pointed at the profiling inputs, so a production config can
be used as it is. `contrib/profile_render_example_config.py` has seven renders
of one world (five overworld renders, two of them lit and three overlays, plus
the nether and the end):

```
python contrib/profile_render.py --profiler none --world ~/worlds/MyWorld \
    --config contrib/profile_render_example_config.py --name Multi
```

Options:

| Option | Meaning |
|---|---|
| `--world PATH` | World directory to render (default `tmp/TwentySix_Three`) |
| `--name NAME` | Name for the output and profile files (default: the world directory's name) |
| `--config PATH` | Render this config's renders instead of a single normal-mode render |
| `--texturepath PATH` | Minecraft client jar or resource pack |
| `--processes N` | Worker processes (default: Overviewer's CPU count) |
| `--profiler sampling\|perf\|none` | Which profiler to run under (default `sampling`) |
| `--sampling-rate RATE` | Sampling rate for `profiling.sampling` (default `100hz`) |
| `--perf-frequency HZ` | Samples per second per CPU for `perf record` (default 999) |
| `--python PATH` | Python to run Overviewer with |

## Reading the profiles

**Sampling profiler.** `--profiler sampling` writes a flamegraph per process
(`<name>-<n>_<pid>.html`, one per worker) and a combined one
(`<name>-<n>.html`). It shows Python functions and, with `--native`, which
the script passes, time spent in C.

**perf.** `--profiler perf` records the whole process tree with frame-pointer
call graphs, running Python with `-X perf` (and `PYTHONPERFSUPPORT=1` for the
workers) so Python functions appear in the stacks as `py::function:file`. For
example:

```
perf report -i tmp/profiles/<name>-<n>.perf.data --no-children --sort sym
perf report -i tmp/profiles/<name>-<n>.perf.data --no-children --sort dso
perf annotate -i tmp/profiles/<name>-<n>.perf.data --stdio -l -s alpha_over_full
```

Call graphs are complete through code built with frame pointers. The C
extension is compiled with Python's own compiler flags, so it has them when
Python was built with `-fno-omit-frame-pointer`; add `-g` to the flags to see
source lines in `perf annotate`. System libraries often lack both: samples in
`libz` or numpy then lose their callers and appear under their own library,
with addresses rather than names.

## Benchmark results

Results of the performance work, one row per change, each measured on top of
the ones before it.

**Method.** Each row is one run of each benchmark below, without a profiler, on
an AMD Ryzen 7 5700G (8 cores, 16 threads, 16 worker processes) in Docker.
Expect a few percent of noise between runs.

- *Single render:* one normal-mode overworld render of a small test world:
  `python contrib/profile_render.py --profiler none --world <world>`.
  This is the second of two runs, so where generated textures are cached
  between runs, they are.
- *Seven renders:* `contrib/profile_render_example_config.py` on the same
  world with a generated nether and end:
  `python contrib/profile_render.py --profiler none --world <world> --config contrib/profile_render_example_config.py`.
- *Seven-render tile size:* the total size of the seven renders' tiles.
- *Tiles identical:* every tile of both renders compared pixel by pixel with
  the baseline's.
- *Full world render:* a full render of a large production world (4.5 GB,
  about 1.07 million render operations) with a six-render config like the
  example (the world has no end yet).

| Change | Single render | Seven renders | Seven renders vs. baseline | Seven-render tile size | Tiles identical | Full world render |
|---|---:|---:|---:|---:|---|---:|
| Baseline | *pending* | *pending* | — | *pending* | — | *pending* |
| Faster chunk parsing | *pending* | *pending* | *pending* | *pending* | *pending* | *pending* |
| Texture generation | *pending* | *pending* | *pending* | *pending* | *pending* | *pending* |
| Chunk reuse within a render | *pending* | *pending* | *pending* | *pending* | *pending* | *pending* |

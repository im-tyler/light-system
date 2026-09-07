# Benchmark Results: Meridian renderer vs stock Godot Forward+

First automated comparison captured with `run_bench.sh` (see
[README.md](./README.md) for method and caveats).

## Environment

- date: 2026-09-07
- machine: MacBook Air (Mac16,13), Apple M4, integrated GPU
- os: macOS 26.6.2
- renderer binary: `prototype/build/meridian_vk_bootstrap` (build of
  2026-09-06, hidden-window non-interactive mode, Vulkan via MoltenVK,
  `selected_device=Apple M4`)
- godot: 4.7.2.stable.official.ed1daf0bf (official GitHub release,
  mac universal), Forward+, `--rendering-driver vulkan`, reports
  Vulkan 1.2.334 on Apple M4 (MoltenVK)
- resolution: 1280x720, vsync disabled in Godot (MoltenVK still often
  floors windowed presents at 60 Hz; see caveats)
- frames: 117 measured per run (renderer: 120 presented minus 3 skipped;
  Godot: 30 warmup + 117 measured), static bounds-derived camera
- shadows on in both engines; city renderer run uses
  `--error-threshold 0.05`

## Per-run results (2026-09-07, REPEAT=2)

| engine | scene | run | median ms | avg ms | p99 ms | avg fps |
| --- | --- | --- | --- | --- | --- | --- |
| renderer | stanford_dragon | 1 | 39.22 | 39.84 | 48.35 | 25.1 |
| renderer | stanford_dragon | 2 | 37.54 | 37.49 | 45.37 | 26.7 |
| godot | stanford_dragon | 1 | 16.66 | 16.68 | 19.29 | 60.0 (capped) |
| godot | stanford_dragon | 2 | 16.47 | 16.67 | 19.39 | 60.0 (capped) |
| renderer | massive_city | 1 | 121.74 | 121.81 | 134.10 | 8.2 |
| renderer | massive_city | 2 | 191.40 | 191.20 | 208.57 | 5.2 |
| godot | massive_city | 1 | 5.10 | 5.87 | 18.87 | 170.5 (unthrottled) |
| godot | massive_city | 2 | 16.65 | 16.69 | 18.91 | 59.9 (capped) |

Supplementary single runs earlier the same day (idle machine): renderer
dragon avg 41.5-44.4 ms; renderer city avg 122.2-133.0 ms; godot dragon
16.66 ms (capped); godot city 16.66 ms (capped). A later verification run
taken while other builds were compiling on the machine measured renderer
dragon at 66.9 ms avg — CPU contention moves this renderer's numbers
substantially; always benchmark on an idle machine.

## Comparison (medians; capped Godot runs are upper bounds on frame time)

| scene | renderer | stock Godot Forward+ | verdict |
| --- | --- | --- | --- |
| stanford_dragon (871k tris) | ~37.5-39.2 ms (25-27 fps) | <=16.6 ms (>=60 fps) | Godot at least ~2.3x faster |
| massive_city (1,004k tris) | ~122-191 ms (5-8 fps) | ~5.9 ms (170 fps) when unthrottled; <=16.7 ms otherwise | Godot at least ~7x and likely ~20x faster |

## Honest caveats

- Shading paths differ: renderer debug/selection path (procedural
  shading, cluster LOD selection, 3-cascade CSM) vs Godot stock PBR
  (PSSM4). Same geometry, same camera framing, same resolution.
- Godot numbers at 16.6x ms sit on the 60 Hz present floor — true Godot
  frame times are lower; the unthrottled city run (5.87 ms avg) is the
  one direct measurement we got.
- Renderer city run 2 (191 ms) followed sustained GPU load (thermal
  drift / possible machine contention from concurrent builds); the
  122-133 ms range from idle runs is more representative.
- Renderer timings include its full CPU traversal + residency + draw
  submit path as it exists today; this is the baseline the mission
  (beat stock Forward+ on dense static geometry) must close against, not
  a final meshlet pipeline.

## Reading

Gate 2 target (beat stock Forward+ on at least one scene) is not met on
this suite today: stock Godot Forward+ comfortably outperforms the
current renderer path on both shared scenes at 720p on Apple M4. The
dragon gap (~2.3x+) and city gap (>=7x) are the concrete numbers for
milestone reporting under ADR-008.

## Re-running

```bash
cd benchmarks/godot
REPEAT=2 ./run_bench.sh
```

Requires stock Godot at `/tmp/godot-bench/Godot.app` (see README for the
download) and the renderer built at `prototype/build/meridian_vk_bootstrap`.
Raw logs and JSON land under `benchmarks/godot/results/<timestamp>/`.

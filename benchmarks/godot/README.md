# Godot Comparison Benchmark Harness

Automated benchmark comparing the standalone Meridian Vulkan renderer
(`prototype/build/meridian_vk_bootstrap`) against stock Godot Forward+
on shared scenes. This is the ADR-008 fixed-suite tooling: one command
runs both engines back to back on identical geometry and camera framing
and prints a comparison table.

## Layout

- `run_bench.sh` — orchestrates both engines, prints tables, keeps raw logs
- `project/` — minimal stock Godot project (no addons) whose runner script
  replicates the renderer's benchmark camera and metric format
- `RESULTS.md` — captured numbers with environment header and caveats
- `results/` — per-run raw logs and JSON (gitignored)

## Scenes

| id | shared asset | renderer manifest | godot load path |
| --- | --- | --- | --- |
| `stanford_dragon` | `benchmarks/assets/dragon/dragon.obj` (871,306 tris) | `benchmarks/scenes/external/dragon_manifest.txt` | staged clone copy in `project/assets/`, imported via `godot --headless --import` |
| `massive_city` | `benchmarks/scenes/generated/massive_city.glb` (1,004,604 tris) | `benchmarks/scenes/generated/city_manifest.txt` (`--error-threshold 0.05` per renderer usage note) | runtime `GLTFDocument` load of the same file (absolute path, no copy) |

`prototype/thirdparty/meshoptimizer/demo/pirate.glb` is a 24 KB stub, not
the real pirate asset, so it is not used.

## Method parity

- Same source geometry bytes in both engines.
- Same window/swapchain size: 1280x720.
- Same camera framing: the Godot script replicates
  `build_camera_frame_data()` in `prototype/src/vk_bootstrap.mm` —
  camera at `center + (0.4r, 0.5r, 1.5r)` looking at the scene-bounds
  center, 55 deg vertical fov, near `0.01r`, far `8r`, where `r` is the
  max bounds extent. Static camera (deterministic). An optional
  `--camera=orbit` mode exists for future use; it does not match the
  renderer's non-interactive camera and is not used for comparisons.
- Same metric shape: `GODOT_BENCHMARK:` mirrors `MERIDIAN_BENCHMARK:`
  (median/p99/avg ms per frame, avg fps, sample count). Default measured
  frames = 117 (renderer presents 120 and skips its first 3).
- Godot runs stock Forward+ with a single shadowed DirectionalLight3D
  (PSSM 4 splits, max distance `3r` to mirror the renderer's CSM far
  clamp; the renderer uses 3 cascades). Background color, no sky.
- vsync is disabled in the Godot project and at runtime; see caveats.

## How to run

```bash
# one-time: stock Godot (official GitHub release, mac universal)
mkdir -p /tmp/godot-bench && cd /tmp/godot-bench
curl -LO https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_macos.universal.zip
unzip Godot_v4.7.2-stable_macos.universal.zip
./Godot.app/Contents/MacOS/Godot --version

# renderer must be built at prototype/build/meridian_vk_bootstrap
cd <repo>/benchmarks/godot
./run_bench.sh                       # dragon + city, one run each
REPEAT=2 ./run_bench.sh              # two runs per engine per scene
SCENES=stanford_dragon ./run_bench.sh
```

Godot opens a brief window per run (there is no headless GPU rendering
in Godot 4.x). The renderer runs its hidden-window non-interactive mode.

Environment overrides: `GODOT_BIN`, `RENDERER_BIN`, `SCENES`, `FRAMES`,
`WARMUP`, `REPEAT`.

## Caveats (read before quoting numbers)

- Different shading paths. The renderer draws its debug/selection path
  (procedural shading, cluster LOD selection, 3-cascade CSM); Godot runs
  stock PBR (Forward+, PSSM4 shadows). Same geometry and camera, not the
  same material system — this measures each engine's current end-to-end
  dense-geometry path, which is the comparison the mission asks for.
- Present throttling. Both engines present through MoltenVK on macOS.
  vsync-off is requested but MoltenVK has no reliable unthrottled present
  here; windowed runs often floor at the 60 Hz display refresh
  (16.7 ms). Any Godot number near 16.6 ms reads as "at or above 60 FPS,
  true frame time unmeasured". Occasionally runs exceed 60 FPS (present
  throttle not engaged); such runs expose the true frame time.
- Warmup asymmetry. The renderer hardwires a 3-frame skip; Godot gets 30
  warmup frames (default, override with `WARMUP`) so first-frame mesh
  uploads do not pollute its samples.
- City renderer config uses `--error-threshold 0.05` (its documented
  scene-appropriate value; default 0.001 does not activate city LOD
  groups).
- Bounds source differs slightly: the renderer uses vgeo bounds (include
  `bounds_padding`: 0.1 dragon, 0.5 city); Godot computes aggregate mesh
  AABBs. Framing shift is negligible.
- Thermal and machine-load variance is real (sustained GPU load moved a
  city renderer run from ~122 ms to ~191 ms in one session). Run on an
  idle machine; use `REPEAT` and compare medians.
- The renderer rebuilds its scene resource from the source asset on every
  invocation (deterministic; ~10-25 s per run, not part of frame timing).
- Engine counters are not directly comparable: Godot reports
  `TOTAL_PRIMITIVES_IN_FRAME`; the renderer reports selected/drawn
  cluster counts on stdout.

# Benchmark Results: Meridian renderer vs stock Godot Forward+

First automated comparison captured with `run_bench.sh` (see
[README.md](./README.md) for method and caveats).

## 2026-09-07 17:34 local: shadow caster LOD — city gap 4.9x -> ~2.9x

Same machine/os/godot as below, but the machine was heavily loaded the
whole session (load average ~85-100: an iOS-simulator game at ~98% CPU
plus its SimMetalHost GPU companion, concurrent builds). Both engines ran
under the same conditions back-to-back; renderer numbers below are
nonetheless inflated versus an idle machine (the 2026-09-07 evening
section is the idle reference). One change, measured on this tree:

Shadow caster LOD: the shadow pass now selects casters from a second
`simulate_traversal` at the main error threshold x 8 (`--shadow-error-scale`,
default 8.0, <= 1 disables). Depth-only silhouettes far below the
2048px-cascade texel footprint cannot survive PCF filtering, so casters
select their own coarser LOD level; the merged layered shadow pass,
per-cascade overlap masks, and draw machinery are unchanged. Residency
merges the shadow selection's pages (matters only for
`--demand-streaming`). Paired A/B under identical load, minutes apart:

| scene | metric | before | after |
| --- | --- | --- | --- |
| massive_city | median ms | 21.18 | 12.14 |
| massive_city | shadow draws | 20746 | 3239 |
| massive_city | GPU shadow pass | 3.2-3.9 ms | 0.6-1.5 ms |
| massive_city | vkQueueSubmit | 9.46 ms | 5.87 ms |
| massive_city | fence (GPU) | 7.43 ms | 1.88 ms |
| stanford_dragon | median ms | 11.10 | 8.64 |
| stanford_dragon | shadow draws | 5532 | 800 |
| stanford_dragon | GPU shadow pass | ~4.5 ms | 0.32 ms |
| stanford_dragon | fence (GPU) | 5.50 ms | 2.57 ms |

Main-pass draws are untouched (city 20732, dragon 4002); the shadow
draw counts match `meridian_trace` selections at threshold x8 exactly
(city 1955 base + 1284 LOD; the LOD ladder cliffs to 2 clusters just
above x8, so 8 is the ceiling for the city ladder).

### Per-run results (2026-09-07 17:34, REPEAT=2, loaded machine)

| engine | scene | run | median ms | avg ms | p99 ms | avg fps |
| --- | --- | --- | --- | --- | --- | --- |
| renderer | stanford_dragon | 1 | 8.34 | 8.34 | 10.33 | 119.9 |
| renderer | stanford_dragon | 2 | 8.46 | 8.42 | 10.47 | 118.8 |
| godot | stanford_dragon | 1 | 16.59 | 16.68 | 19.70 | 59.9 (capped) |
| godot | stanford_dragon | 2 | 6.91 | 7.93 | 18.76 | 126.1 (unthrottled) |
| renderer | massive_city | 1 | 13.92 | 14.52 | 23.78 | 68.9 |
| renderer | massive_city | 2 | 13.05 | 13.27 | 19.02 | 75.4 |
| godot | massive_city | 1 | 16.75 | 16.65 | 20.29 | 60.1 (capped) |
| godot | massive_city | 2 | 16.64 | 16.53 | 24.89 | 60.5 (capped) |

Under this load Godot's city runs sat ON the 60 Hz cap with p99 ~25 ms
(it could not hold 60 fps), so no unthrottled Godot city number was
obtainable this session. Against the idle-machine unthrottled Godot city
reference (4.12 ms), the renderer's paired 12.14 ms closes the gap from
~4.9x to ~2.9x — while itself being load-inflated. Dragon now clears
100 fps under load (8.3-8.6 ms vs 9.17 ms idle pre-change).

Steady-state renderer profile after (city): submit 5.9 ms, fence 1.9 ms
(GPU main 1.7-3.4 + shadow 0.6-1.5), traverse 2.8 ms (main + shadow
traversals), build 1.0 ms, draws main 20732 / shadow 3239.

Verification: clean `-Wall -Wextra -Wpedantic` build of all meridian
targets; `--validate` clean on dragon + city (only the preexisting
MoltenVK blend-state warning); `visibility_selection_subset=true` on
dragon, city, terrace and uv_seam; terrace + uv_seam replays and the
builder smoke set pass; `--demand-streaming --budget 96` dragon passes
(page uploads include the shadow-LOD pages; residency held at 95/3617).
Screenshot pixel-compare was NOT usable this session: the screenshot
path re-acquires a presented swapchain image, and with a hidden window
under compositor load that content came back black/non-composited —
reproduced on the unmodified pre-change binary, so it is an
environmental flake of the capture path, not a rendering change
(cross-checked: binary-identical code at `--shadow-error-scale 1`
produced the same black captures as the changed build, and the scale
1/2/8 captures were 98.5-99.3% identical to each other with a gradual
coarsening gradient).

Remaining city gap (~2.9x to the idle unthrottled Godot), in order:
(a) main-pass draw count 20.7K — the shadow pass is now 3.2K of ~24K
total encodes, so the MoltenVK per-draw submit tax is dominated by the
main list; (b) CPU traverse+build ~3.8 ms — serial DFS x2 plus
per-cluster AABB/cone tests, parallelizable; (c) main-pass GPU 1.7-3.4
ms under load.

## 2026-09-07 (evening): submit-path fixes — dragon at parity+, city gap cut 4x

Same machine/os/godot/build recipe as the morning section below. Renderer
rebuilt from the same tree plus four changes (measured one at a time on this
machine, dragon / city at the old pinned `--error-threshold 0.05`):

1. Exact CPU draw counts on the `vkCmdDrawIndirect` fallback. MoltenVK 1.4.2
   advertises no `drawIndirectCount` (core feature false; the symbol is a
   no-op stub), so the renderer had been running the fallback with the count
   parameter set to the draw-list *capacity* — 4 draws x total-cluster-count
   Metal encodes per frame (dragon 115K, city 327K) at ~0.2 us each. Passing
   the CPU-known per-pass counts: dragon 38.9 -> 15.9 ms (submit 23.3 ->
   3.3), city 124.9 -> 65.0 ms (submit 71.5 -> 15.3). The no-count path now
   draws the exact CPU list; the HZB occlusion-refined list is only consumed
   where `vkCmdDrawIndirectCount` exists (its GPU-written count cannot feed
   `vkCmdDrawIndirect`).
2. Merged single-draw layered shadow pass: one draw list with a per-entry
   cascade overlap mask (geometry_kind bits 17..19), one instance per
   overlapping cascade, `gl_Layer` selection in the vertex shader
   (VK_EXT_shader_viewport_index_layer), one layered framebuffer over the
   existing 2D-array depth. Shadow submissions 3 -> 1, shadow draws
   sum(cascades) -> one per caster cluster: dragon 15.9 -> 14.9 ms (submit
   3.3 -> 1.9; shadow draws 11770 -> 5532), city 65.0 -> 56.9 ms (submit
   15.3 -> 8.8; 58861 -> 28930). Dragon screenshot pixel-compare vs the
   pre-change build: 1595 of 921600 pixels differ by >10 (PCF penumbra
   order effects, localized on the model).
3. Scene-scaled auto LOD threshold: default `--error-threshold` is now
   `max(0.001, 8.9 x median LOD-group geometric error)`. Dragon's median
   (1.12e-4) resolves to the 0.001 floor exactly — selection and draw counts
   unchanged. City's median (0.1007) resolves to 0.8965, selecting ~20.7K
   clusters instead of 28.9K (pinned 0.05) / 31.4K (full detail): city
   56.9 -> 47.0 ms. `run_bench.sh` no longer pins city to 0.05.
4. Per-frame visibility analysis removed from the frame loop (the identical
   analysis still runs once after the loop for the report; the property
   check is unchanged and still passes): city 47.0 -> 20.7 ms, dragon
   ~15.9 -> ~9.9 ms. The 921K-pixel scan + set inserts had cost ~26 ms/frame
   on city.

### Per-run results (2026-09-07 evening, REPEAT=2)

| engine | scene | run | median ms | avg ms | p99 ms | avg fps |
| --- | --- | --- | --- | --- | --- | --- |
| renderer | stanford_dragon | 1 | 9.51 | 9.85 | 15.73 | 101.5 |
| renderer | stanford_dragon | 2 | 9.17 | 9.37 | 11.85 | 106.8 |
| godot | stanford_dragon | 1 | 16.87 | 16.60 | 24.83 | 60.2 (capped) |
| godot | stanford_dragon | 2 | 16.65 | 16.65 | 24.01 | 60.1 (capped) |
| renderer | massive_city | 1 | 22.42 | 22.57 | 27.73 | 44.3 |
| renderer | massive_city | 2 | 20.35 | 20.75 | 26.57 | 48.2 |
| godot | massive_city | 1 | 4.39 | 5.16 | 18.34 | 193.7 (unthrottled) |
| godot | massive_city | 2 | 4.12 | 5.16 | 14.94 | 193.8 (unthrottled) |

### Comparison (medians)

| scene | renderer | stock Godot Forward+ | verdict |
| --- | --- | --- | --- |
| stanford_dragon (871k tris) | 9.17 ms (~107 fps) | 16.65 ms (60 Hz present floor) | renderer ~1.8x faster than the capped reading and clears 60 fps; Godot's true time is unmeasured below the cap |
| massive_city (1004k tris) | 20.35 ms (48 fps) | 4.12 ms unthrottled (193 fps) | Godot ~4.9x faster (was ~7-20x) |

Steady-state renderer profile: dragon submit 2.7-2.9 ms, fence (GPU) 4.8-5.0
ms, draws main 4101 / shadow 5532; city submit 8.4 ms, fence 8.1 ms (GPU:
shadow 3.9 + main 3.4), draws main 20733 / shadow 20746, CPU traverse+build
~3.3 ms.

Verification for this section: clean `-Wall -Wextra -Wpedantic` build;
`--validate` clean on both scenes (only the preexisting MoltenVK blend-state
warning on the uint visibility target); `visibility_selection_subset=true`
on both scenes (drawn set is now the full CPU selection — the occlusion
heuristic previously dropped some visible contributors, which the city
screenshot diff exposed); terrace replay smoke and `--demand-streaming
--budget 96` dragon run pass.

Remaining city gap (~5x), in order: (a) `vkQueueSubmit` 8.4 ms — ~41.5K
Metal draw encodes at ~0.2 us each, irreducible on MoltenVK without fewer
draws or a Metal backend; (b) GPU 8.1 ms — shadow pass renders every caster
into 2-3 cascades (cascade 1/2 frusta span the whole scene); needs caster
LOD/distance culling; (c) CPU traverse+build 3.3 ms — serial DFS plus
per-cluster AABB tests, parallelizable.

## 2026-09-07 (morning): first automated comparison

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

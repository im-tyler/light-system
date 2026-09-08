# Benchmark Results: Meridian renderer vs stock Godot Forward+

First automated comparison captured with `run_bench.sh` (see
[README.md](./README.md) for method and caveats).

## 2026-09-08 local: dead-pass pruning + visibility-store elision — submit mystery profiled to Metal encode; city GPU total -0.3 to -1.6ms

Same machine/os/godot as below; machine loaded the whole session (load
average ~20-31: the iOS-simulator game + companion plus concurrent agent
workloads). All renderer A/B pairs are minutes apart under that load;
draw/encode counts, wasted-vertex counters, and report fields are
deterministic (load-independent). The idle-machine rerun remains pending
for the whole 2026-09-07/08 series.

Two changes, measured together and individually probed:

1. **Dead per-frame passes removed (fallback path).** Profiling the
   post-fold command stream showed four GPU work items that no draw path
   consumes on MoltenVK, submitted every frame anyway: the instance-cull
   dispatch (its output only fed the retained-but-not-dispatched
   cluster_select shader), the occlusion-refine dispatch + its two
   buffer fills and barriers (only the drawIndirectCount path can
   consume the GPU-written survivor list), the full HZB build (12
   dispatches, ~14 barriers, 3 image transitions — only occlusion reads
   it), and the 7.3MB visibility image -> readback-buffer copy (read
   once per run by the post-loop analysis). All four now run in a
   one-shot "diagnostic epilogue" command buffer submitted after the
   present loop, so every report field survives (`compute_cull_visible`,
   `compute_occlusion_surviving` now computed against the final frame's
   own HZB — 14020 vs 14015 on city, a 0.04% diagnostic drift from the
   same-frame vs previous-frame HZB; static-camera benchmark runs are
   unaffected). The drawIndirectCount path keeps occ + HZB per frame
   (it consumes them).
2. **Visibility store elided on non-capture frames.** The visibility
   attachment's 7.3MB store-back was paid every frame for a readback
   that happens once. A second render pass identical except
   `storeOp = DONT_CARE` on the visibility attachment (render-pass
   compatible, shared framebuffers) is used for all frames except the
   capture frame — the final index of a fixed-count run, or every frame
   while interactive (the loop breaks on close before rendering the
   observing frame, so interactive has no last-frame signal).

| scene | metric | before | after |
| --- | --- | --- | --- |
| massive_city | GPU total (per-frame timers, clean pair) | 2.78-3.95 ms | 2.05-2.38 ms |
| massive_city | GPU total (medians, 3x paired round) | 3.47-3.80 ms | 2.96-3.30 ms |
| massive_city | fence (GPU execution wait) | 1.7-2.5 ms | 1.9-2.0 ms |
| massive_city | median ms (paired) | 8.52-9.16 | 8.48-8.53 |
| stanford_dragon | median ms (paired) | 8.33-8.37 | 8.29-8.47 |
| both | per-frame commands removed | cull fill+dispatch+2 barriers, occ fill+dispatch+2 barriers, HZB 2 transitions+12 dispatches+~12 barriers, visibility blit+2 barriers+transition | 0 (epilogue, once) |

**The submit-cost mystery is closed, honestly negative:** the item was
"MoltenVK fixed submit ~1.5-2ms beyond draws". Measured directly with
`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=1` (encode at record time):
city cmdrec 0.01 -> 2.51 ms while submit 2.21 -> 0.05 ms — the cost
moves exactly, total unchanged. The ~2ms IS Metal command-buffer
encoding of the frame's ~30-command stream, paid on the CPU no matter
which timer wraps it. After the pruning above the stream is minimal
(2 render passes, 2+2 draws, 1 barrier, timestamps), and the submit
timer still reads ~2ms under load — that residual is the encode floor
plus load noise (submit readings ranged 0.57-4.3 ms across the session
with identical command streams; a full-detail city run with 51K draws
folded to the same 2+2 encodes read submit=0.57 during a load spike).
Also tried, no reproducible wall-time change under load:
`MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1`, `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`
(argbuf read ~0.5ms worse), GPU timestamps off via new `--no-gpu-timers`
flag (fence -0.3-0.5ms from less GPU counter-sampling, wall flat). All
knobs were env-only experiments; nothing relies on them at runtime.

**Degenerate-corner cost counted (main-pass item, closed as
negligible):** the fold now reports wasted vertex-shader invocations
(`wastedvs` in MERIDIAN_CPU). City main 201597 (~3% of main VS work),
shadow 736692 (~15%, mostly unused cascade-stride slots); dragon main
56220 (~4.5%), shadow 389001 (~33%). Doubling kDrawBucketCount to 8
produced an identical fold on city (still 2 nonempty buckets, same
wasted counts) — the vertex-count distribution is too peaked for more
buckets to matter, and even zeroing the waste is <0.05ms of GPU.
Reverted to 4. The visibility-store elision measured -0.40/-0.07ms
GPU-main across two dedicated A/B rounds (control pass variance
±0.15ms) — expected ~0.1-0.15ms of store bandwidth, at the measurement
floor. Main-pass GPU remains: city 1.2-1.5ms, dragon 0.6-0.7ms — that
is now genuinely geometry + PCF shading, not structural waste.

Verification: clean `-Wall -Wextra -Wpedantic` rebuild (0 warnings);
dragon AND city screenshots pixel-identical to the pre-change build
(0/921600 pixels >10 on both — draw order untouched); report fields
identical (city 94790 valid pixels, cull counter 1, occ 14020);
`visibility_selection_subset=true` on dragon, city, terrace, uv_seam,
textured_uv_seam; `replay_runtime_parity=true` everywhere;
`--validate` clean on dragon + city (only the preexisting MoltenVK
blend-state warning); builder smoke set (4 manifests), meridian_dump,
terrace + uv_seam replays, `meridian_trace --parallel 8`
(parallel_match=true), and `--demand-streaming --budget 96` dragon
(2016 page uploads, residency pinned 95, subset holds) all pass.

Remaining city gap, honestly: (a) ~2ms Metal command-buffer encode
floor — not command-count driven anymore; further cuts need MoltenVK
or Metal-level work (or an idle-machine rerun to establish the true
floor; current readings are load-inflated), (b) main-pass GPU
1.2-1.5ms is real shading cost — further reduction means changing
pixels (PCF tap count, material model), which the pixel-comparison
bars currently forbid, (c) CPU traverse+build ~3.7ms under load
(idle rerun pending, owned by the 2026-09-07 23:15 parallelization).


## 2026-09-07 23:15 local: parallel CPU traversal + draw build — city traverse+build 5.8 -> 3.7 ms paired, output bit-identical

Same machine/os as below; the machine stayed loaded the whole session
(load average ~17-31: the iOS-simulator game + companion from the
17:34 section plus concurrent agent workloads). All A/B pairs are
minutes apart under that load; draw counts / encode counts /
parallel-vs-serial vector equality are trace-deterministic
(load-independent). The idle-machine rerun remains pending for the
whole 2026-09-07 evening series.

One change: the per-frame CPU work now runs on a small fork-join
thread pool (`--threads N`, default auto = min(hardware_concurrency,
8); 1 = the previous serial path). Two levels of parallelism: (a) the
main and shadow-caster `simulate_traversal` DFS run concurrently,
(b) each DFS forks subtree tasks at hierarchy nodes whose children all
span >= 512 base clusters (capped at 2x-threads tasks per traversal),
and (c) the selection -> GpuDrawEntry conversion is chunked (~1536
selection indices per job) across the same pool. Determinism argument:
the coverage marks the DFS mutates are scoped to ancestor descents
(set before the sibling loop, cleared after), so every sibling subtree
sees exactly the incoming coverage state — a forking child gets a
byte-copy plus fresh output marks, and an ordered merge in child order
reproduces the serial first-encounter order of every deduplicated
list (the marks only suppress duplicate output pushes, never traversal
decisions). Build chunks write chunk-local `first_instance` that the
ordered concat fixes up with global offsets. Nothing else (fold,
residency merge, uploads) changed. `meridian_trace --parallel <t>`
now runs both paths and asserts vector equality.

| scene | metric (CPU sections, ms/frame) | serial (`--threads 1`) | parallel (8 threads) |
| --- | --- | --- | --- |
| massive_city | traverse (2 DFS + fork) | 3.88 | 2.39 |
| massive_city | build (chunks + fold) | 1.94 | 1.30 |
| massive_city | traverse+build total | 5.82 | 3.69 (-37%) |
| stanford_dragon | traverse+build total | ~1.35 | ~1.28 |

Thread sweep on city under the same load: 1 -> 3.88/1.94, 4 ->
2.55/1.46, 6 -> 2.67/1.34, 8 -> 2.39/1.30. The first cut used 4x-thread
task budgets and a 256-cluster fork floor; under external load that
produced too many small wakeups and *regressed* 8-thread traverse to
4.17 ms — fewer, bigger tasks (2x budget, 512 floor) is what the table
above reports.

Frame-time medians under a load spike to ~31 during the interleaved
REPEAT=2 baseline-binary vs parallel-binary runs were a wash (city
8.58/8.52 vs 8.52/8.61; dragon 8.39/8.53 vs 8.38/8.35): at that load
the frame is fence/submit-bound and the CPU-section saving mostly
lands inside stolen cycles. p99 inflated on some parallel runs
(city r1 21.5 ms) — wakeup latency under load spikes; not observed
at load ~20 or below.

Verification: dragon AND city screenshots pixel-identical to the
pre-change build (0/921600 pixels differ >10 on both — the ordered
merge reproduces the exact entry order, so not even the previous
0.047% city depth-flip class reappears); `parallel_match=true` from
`meridian_trace --parallel 8` on terrace, uv_seam, textured_uv_seam,
dragon (t 0.001 and 0.008) and city (t 0.8965 and 7.172), repeated x3;
ThreadSanitizer clean on dragon + city traversals; draws unchanged
(city main:20732 shadow:3239, dragon main:4002 shadow:800; encodes
2+2 / 4+4); `replay_runtime_parity=true` and
`visibility_selection_subset=true` on both scenes including
`--validate` runs (only the preexisting MoltenVK blend-state warning);
builder smoke set (4 manifests), meridian_dump, terrace + uv_seam
replays, and `--demand-streaming --budget 96` dragon (residency
pinned 96/3617, subset holds) all pass; clean
`-Wall -Wextra -Wpedantic` rebuild (0 warnings).

Remaining city gap, in order: (a) MoltenVK fixed submit cost
~1.5-2 ms beyond draws (follow-up work), (b) main-pass GPU 1.3-1.8 ms,
(c) CPU traverse+build now ~3.7 ms under load — expected to shrink
further on an idle machine where the pool gets real cores (idle rerun
pending; dragon is already ~1.3 ms total and effectively done).

## 2026-09-07 20:35 local: instance-folded draw submission — city gap ~2.9x -> ~1.6-2.1x

Same machine/os/godot as below; the machine was loaded the whole session
(load average ~20-40: two concurrent agent workloads in the repo). All
renderer runs below are paired A/B minutes apart under that load; the
encode counts are trace-deterministic (load-independent). A final
idle-machine rerun happens after this session and should replace these
wall times.

One mechanism, two commits, measured one at a time. MoltenVK has no
multi-draw indirect — every `vkCmdDrawIndirect` entry becomes one Metal
draw encode at ~0.15 us, so both passes paid a per-cluster submit tax
(city: 20.7K main + 3.2K shadow encodes per frame). The CPU draw lists
are now instance-folded into vertex-count buckets (quartile edges via
`nth_element`, one `vkCmdDraw` per nonempty bucket): the vertex shader
already resolved its draw entry through `gl_InstanceIndex`, so the fold
only (a) reorders entries bucket-major, (b) sets `firstInstance` to the
entry's new global index (x4 stride for the layered shadow pass), and
(c) collapses corners past a cluster's own triangle count to zero-area
triangles (counts are triangle_count*3, so triangles never straddle a
bucket boundary; the shadow shader also degenerates stride slots past
each entry's cascade-overlap popcount). Caster selection, LOD policy,
and the drawIndirectCount path (devices that truly support it) are
unchanged.

| scene | metric | before | after main-fold | after +shadow-fold |
| --- | --- | --- | --- | --- |
| massive_city | main encodes | 20732 | 2 | 2 |
| massive_city | shadow encodes | 3239 | 3239 | 2 |
| massive_city | vkQueueSubmit | 6.6-6.9 ms | 2.2-2.4 ms | 1.9-2.2 ms |
| massive_city | median ms (paired) | 14.02 / 14.16 | 8.82 / 8.55 | 8.54 / 8.50 |
| massive_city | GPU main / shadow | 1.7-3.4 / 0.6-1.5 ms | 1.3 / 0.7-2.1 ms | 1.8 / 0.67 ms |
| stanford_dragon | main encodes | 4002 | 4 | 4 |
| stanford_dragon | shadow encodes | 800 | 800 | 4 |
| stanford_dragon | median ms (paired) | 8.39 / 8.55 | 8.37 / 8.29 | 8.36 / 8.34 |

Draw selection is unchanged (city draws=main:20732 shadow:3239 all
along; `meridian_trace` predicts the same 5417 base + 15329 LOD
clusters at the auto threshold 0.8965). Dragon renders pixel-identical
to the pre-change build (0/921600 pixels differ >10). City differs on
430/921600 pixels (0.047%), all inside one contiguous screen region —
the bucket-major reorder flips the depth-equal winner among coincident
surfaces (same class of diff as the layered-shadow merge's documented
PCF penumbra order effect, and smaller); visibility readback stats are
identical before/after (94790 valid pixels, same unique base/LOD
geometry counts, `visibility_selection_subset=true`).

### Per-run results (2026-09-07 20:35, REPEAT=2, loaded machine)

| engine | scene | run | median ms | avg ms | p99 ms | avg fps |
| --- | --- | --- | --- | --- | --- | --- |
| renderer | stanford_dragon | 1 | 8.34 | 8.34 | 10.55 | 119.9 |
| renderer | stanford_dragon | 2 | 8.30 | 8.35 | 10.21 | 119.7 |
| godot | stanford_dragon | 1 | 8.22 | 8.34 | 9.57 | 119.9 (unthrottled) |
| godot | stanford_dragon | 2 | 7.70 | 11.73 | 21.82 | 85.3 (unthrottled) |
| renderer | massive_city | 1 | 8.56 | 8.67 | 12.34 | 115.4 |
| renderer | massive_city | 2 | 8.57 | 8.74 | 11.86 | 114.4 |
| godot | massive_city | 1 | 5.26 | 5.93 | 17.24 | 168.5 (unthrottled) |
| godot | massive_city | 2 | 16.78 | 16.65 | 23.44 | 60.0 (capped) |

Under the same load, dragon is at parity with Godot (8.30 vs 7.70-8.22
unthrottled); city is 8.56 vs Godot's best same-session unthrottled
5.26 -> ~1.6x, or ~2.1x against the idle-machine Godot reference of
4.12 ms (the renderer numbers are themselves load-inflated — the
pre-change build measured 12.1 ms under a comparable load and 8.3-8.6
ms here).

Verification: clean `-Wall -Wextra -Wpedantic` rebuild (0 warnings);
`--validate` clean on dragon + city (only the preexisting MoltenVK
blend-state warning); `visibility_selection_subset=true` on dragon,
city, terrace, uv_seam and textured_uv_seam; terrace + uv_seam replays
and the builder smoke set (8 manifests) pass; `--demand-streaming
--budget 96` dragon passes (2016 page uploads, residency pinned at the
budget, subset holds).

Steady-state city profile after: traverse 2.7-3.1 ms, build 1.3-1.5 ms,
residency 0.2, upload 0.1-0.2, cmdrec 0.02, submit 1.9-2.2 ms (was
6.6-6.9; the remainder is the fixed MoltenVK encode cost of the
command stream — compute passes, barriers, HZB mips — no longer the
draw lists), fence (GPU) 1.5-2.2 ms.

Remaining city gap (~1.6-2.1x), in order: (a) CPU traverse+build
~4.0-4.6 ms (two serial DFS traversals + draw-build; parallelizable),
(b) fixed submit cost ~1.5-2 ms (MoltenVK command translation beyond
draws), (c) main-pass GPU 1.3-1.8 ms. Draw/submit tax is no longer a
top-2 item.

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

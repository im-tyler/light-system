# Implementation Status

Last updated: 2026-09-05

## Phase 1: Offline Builder (Complete)

- `meridian_builder` CLI: manifest -> meshlet generation -> hierarchy -> LOD -> page packing -> .vgeo serialization
- `.obj` and `.gltf`/`.glb` import (sparse accessors, meshopt compression, seam locking)
- meshlet generation, optimization, and bounds via meshoptimizer
- partition-based hierarchy tree with clusterlod simplification-backed LOD groups
- seam-safe cross-material simplification
- dual payload domains (base clusters + LOD clusters) with page table
- adjacent replacement-level page dependency hints for streaming prefetch
- `meridian_dump`, `meridian_trace`, `meridian_residency`, `meridian_replay` CLI tools
- validated on synthetic benchmarks + external pirate.glb + Stanford Dragon (871K triangles) + generated 1M-triangle city

## Phase 2: Standalone Vulkan Renderer (In Progress)

### GPU Pipeline (Working)

Per-frame pipeline order:
1. Compute instance culling (frustum 6-plane AABB test, atomic append) -- GPU
2. Cluster/LOD selection -- **CPU** (`simulate_traversal`) with normal-cone backface cull; output uploaded to the same buffers the GPU shader used to populate. The serial DFS compute shader is retained for reference but not dispatched.
3. Occlusion refinement (project cluster AABB against previous frame's HZB -- skipped on frame 0) -- GPU compute
4. Shadow pass (3 cascaded shadow maps, depth-only render from per-cascade orthographic projections that tight-fit the camera sub-frusta, 2048px per cascade in a 2D-array depth image, log/uniform split blend lambda=0.7, depth bias) -- GPU graphics

5. Main geometry pass (vertex pulling from payload SSBOs, smooth vertex normals + hemisphere ambient + directional lighting + 8-tap Poisson-disk PCF shadow with per-pixel rotation and slope-scaled bias) -- GPU graphics
6. HZB construction (depth-copy compute shader + per-mip max-downsample cascade) -- GPU compute

GPU timestamp profiler emits `MERIDIAN_GPU: cull=.. sel=.. occ=.. shadow=.. main=.. hzb=.. total=..ms` every frame.

### Data Flow (Connected)

- CPU `simulate_traversal` runs every frame, producing selected base + LOD clusters
- CPU converts selection to `GpuDrawEntry[]` (32 bytes each: VkDrawIndirectCommand header + per-draw metadata) with normal-cone backface cull and writes to HOST_COHERENT draw_list/draw_count buffers
- `vkCmdDrawIndirectCount` reads draw list directly from the same buffers for both shadow and main passes (with fallback to `vkCmdDrawIndirect` if the extension is unavailable)
- Shadow map rendered to depth texture, sampled in main fragment shader via `sampler2DShadow`
- Visibility buffer: two-word RG32_UINT encoding matching visibility_format.h spec (instance, kind, index, local_triangle)
- Normal-cone backface culling on both base and LOD clusters (schema v3 stores LOD cones; dragon steady-state draws 8366 -> 6467)
- `meridian_vk_bootstrap --error-threshold` CLI override; `meridian_trace` reports LOD group error distribution
- Occlusion refinement output available but readback deferred until indirect draws eliminate frame latency

### Interactive Mode

- `--interactive` CLI flag: continuous present loop until ESC
- WASD movement, mouse look, Q/E vertical, camera auto-oriented toward scene center
- FPS and draw count displayed in window title

### Known Issues

- **No texture support**: all shading is procedural (per-cluster color hash + hemisphere ambient). No UV interpolation or texture sampling.
- **Meshlet boundary seams (residual)**: smooth normals are now angle-weighted and position-welded in the builder (`compute_smooth_normals`), which matches normal values across index-split duplicates at the same position. Any remaining boundary seams come from LOD-level T-junctions at cluster borders of different detail, which are mitigated but not fully eliminated by seam-locked vertex simplification.
- **City LOD hierarchy is degenerate (clusterlod output)**: `massive_city` LOD group errors are min ~0.036 / median ~0.1 with root-level groups at FLT_MAX, and there is a selection cliff between threshold ~5 (31K base clusters, ~39 groups activate) and ~20 (collapses to 1 group / 2 clusters). Between those bounds there is no useful middle representation, so the renderer default (0.001) keeps the city at full detail (correct but ~31K draws). Fixing this requires builder-side changes (coarser district partitioning or simplification caps), not traversal changes. `meridian_trace` now prints the error distribution, and `meridian_vk_bootstrap` accepts `--error-threshold` to tune per scene.
- **Page residency initialized as all-resident by default**; pass `--demand-streaming` to run the StreamingScheduler + async-disk-load path. Under that flag pages start unloaded (seed pages autodetected via a coarse `simulate_traversal`), the scheduler throttles loads to `streaming_max_loads_per_frame` per frame, and a page's `loading -> resident` transition now waits on a real `pread()` from a worker thread (`AsyncReader`) against a serialized temp `.vgeo` written at startup. A latency-window simulation remains as the fallback path when the temp-file write or reader open fails. The GPU payload buffer is still populated in full at startup -- async reads currently validate the I/O path rather than replace the live buffer; the next step to remove startup memory cost is mmap-backed payload streaming plus per-page sub-buffer uploads.
- **GPU cluster_select.comp is retained but not dispatched**: CPU `simulate_traversal` produces the draw list each frame; the serial DFS compute shader predates the multi-run coverage model and is kept as reference only. Rebuilding it (BFS-per-level or workgroup-DFS) is worth doing only if profiling shows CPU selection as a bottleneck for some scene class.

### What's Validated

- CPU and GPU selection match exactly on scenes with well-connected LOD hierarchies (pre-cone-culling Dragon: both emit 8628 after normal-cone backface cull; with LOD-cluster cones the CPU draw list drops to 6467).
- Visibility buffer readback confirms `visibility_selection_subset=true` on all benchmark scenes (re-validated 2026-09-05 on the pinned meshoptimizer build)
- Tested assets: 5 synthetic benchmarks, pirate.glb (5K tris), fuzz.glb, Stanford Dragon (871K tris), generated city (1M tris)
- Platform: macOS Apple M4, MoltenVK, Vulkan 1.2; meshoptimizer vendored at c645e49 via `tools/vendor_thirdparty.sh`

### Performance (Apple M4, MoltenVK, 1280x720, meshoptimizer pinned at c645e49)

Stanford Dragon (871K tris): median 15.4ms / ~65 FPS, steady-state draw count 6467 after LOD-cluster normal-cone culling (8366 with base-only culling; 8628 on the pre-2026-09 meshoptimizer build).
Massive City (1M tris): median 30.6ms / ~33 FPS at the default full-detail threshold (was 65.5ms / 15.3 FPS in April; the improvement comes from the repinned meshoptimizer and LOD-cluster cone culling; the city LOD hierarchy remains threshold-degenerate, see Known Issues).

Per-cascade culling closed ~8ms of the CSM regression on Dragon (32 -> 24ms) and ~6ms on City (73 -> 67ms) by filtering the CPU draw list against each cascade's orthographic frustum before submitting, so most clusters land in only one or two cascades instead of all three.

Per-pass GPU (Dragon, steady state): cull 0.1ms, sel 0.0ms (CPU), occ 0.05ms, shadow 3-5ms, main 2-4ms, hzb 0.1-0.2ms.

Per-frame CPU (emitted every 60 frames as `MERIDIAN_CPU: ...`, measured post-CSM + per-cascade culling):
- Application-side work (traverse, residency, build, upload, cmdrec) is under 1ms on both scenes (dragon ~0.35ms, city ~1.0ms).
- `vkQueueSubmit` is 7.8ms on Dragon, 21.8ms on City. Roughly 2x the pre-CSM numbers (3.8ms / 10.6ms) because we now issue 4 indirect-draw calls per frame (main + 3 shadow cascades) instead of 1; per-cascade culling trimmed each cascade's draw count substantially but the per-submission Metal translation cost still scales with total draws across all passes. Only addressable further by reducing total draws (e.g. GPU-side draw packing, texture-array draws) or a native Metal backend.
- `vkWaitForFences` reflects GPU execution time, not CPU overhead.

## Not Yet Implemented

- Real streaming scheduler (CPU prototype exists but pages start all-resident; mmap-based payload streaming is the next step)
- Async disk I/O replacing the live startup buffer (validation path exists via `--demand-streaming`)
- Benchmark automation vs stock Godot
- Texture/UV support
- Broader glTF import coverage
- Compressed geometry payloads
- Deeper Godot runtime integration
- Parallel GPU traversal (BFS-per-level or workgroup-DFS) to replace the retained-but-not-dispatched serial compute_select.comp
- City-class scene LOD: coarser district partitioning or simplification caps so generated cities get a usable mid-error representation

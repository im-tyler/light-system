# Implementation Status

Last updated: 2026-04-16

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
- Occlusion refinement output available but readback deferred until indirect draws eliminate frame latency

### Interactive Mode

- `--interactive` CLI flag: continuous present loop until ESC
- WASD movement, mouse look, Q/E vertical, camera auto-oriented toward scene center
- FPS and draw count displayed in window title

### Known Issues

- **No texture support**: all shading is procedural (per-cluster color hash + hemisphere ambient). No UV interpolation or texture sampling.
- **Meshlet boundary seams (residual)**: smooth normals are now angle-weighted and position-welded in the builder (`compute_smooth_normals`), which matches normal values across index-split duplicates at the same position. Any remaining boundary seams come from LOD-level T-junctions at cluster borders of different detail, which are mitigated but not fully eliminated by seam-locked vertex simplification.
- **Page residency initialized as all-resident by default**; pass `--demand-streaming` to run the StreamingScheduler + async-disk-load path. Under that flag pages start unloaded (seed pages autodetected via a coarse `simulate_traversal`), the scheduler throttles loads to `streaming_max_loads_per_frame` per frame, and a page's `loading -> resident` transition now waits on a real `pread()` from a worker thread (`AsyncReader`) against a serialized temp `.vgeo` written at startup. A latency-window simulation remains as the fallback path when the temp-file write or reader open fails. The GPU payload buffer is still populated in full at startup -- async reads currently validate the I/O path rather than replace the live buffer; the next step to remove startup memory cost is mmap-backed payload streaming plus per-page sub-buffer uploads.
- **~~City has sparse node-LOD links~~ (fixed)**: previously `massive_city.vgeo` reported 6230 LOD groups but only 8 node-LOD links (vs Stanford Dragon's 2509 groups -> 946 links). Now: dragon 2509 groups -> 2509 links (100%), city 6230 groups -> 6230 links (100%). Fix (builder_cluster.cpp `build_node_lod_links` + builder_traversal.cpp + gpu_abi.h): each LOD group now stores its base cluster coverage as a multi-run table (`LodGroupBaseRun` list indexed by `LodGroupRecord::{first_base_run_index, base_run_count}`). Builder attaches a group to the deepest hierarchy node whose cluster span contains all of the group's runs. Traversal threads a per-cluster coverage bitmap through the recursive descent: when an LOD group is selected at a node, its base runs mark covered clusters; descendants skip fully-covered subtrees and base emits skip covered clusters. No child-alignment constraint on the attachment -- subsets and cross-child-boundary groups both work. Schema version bumped to 2.
- **CPU cluster selection divergence on sparse-LOD-link scenes (stale; refer to LOD link fix above)**: on `massive_city` the CPU `simulate_traversal` returns 0 LOD clusters + 31394 base clusters while the pre-hoist GPU `compute_select.comp` produced 15106 draws on the same data. CPU path is semantically identical to GPU shader (LOD-group-first, then base emit if leaf/acceptable error, else descend). Diagnostic pass showed 99.997% of city's base clusters have `normal_cone.w = 1.0` (meshoptimizer-marked degenerate), so backface culling is a no-op regardless of path. After the LOD-link fix city has a dense LOD hierarchy (6230 links), but the default `debug_error_threshold=0.001` is still far below city's smallest group error (~0.036), so CPU selects 0 LOD at that threshold. At looser thresholds CPU traversal now picks LOD groups correctly. GPU shader `cluster_select.comp` is still retained-but-not-dispatched and has not been rewritten against the multi-run coverage model.

### What's Validated

- CPU and GPU selection match exactly on scenes with well-connected LOD hierarchies (Dragon: both emit 8628 after normal-cone backface cull). Diverge on sparse-LOD scenes (see Known Issues).
- Visibility buffer readback confirms `visibility_selection_subset=true` on most benchmark scenes
- Tested assets: 5 synthetic benchmarks, pirate.glb (5K tris), Stanford Dragon (871K tris), generated city (1M tris)
- Platform: macOS Apple M4, MoltenVK, Vulkan 1.2

### Performance (Apple M4, MoltenVK, 1280x720)

Stanford Dragon (871K tris): median 19.2ms / 52 FPS with 3-cascade CSM + per-cascade culling + 8-tap Poisson PCF (improved from 24.0ms after LOD attachment coverage fix)
Massive City (1M tris): median 65.5ms / 15.3 FPS with 3-cascade CSM + per-cascade culling + 8-tap Poisson PCF (small improvement from 66.7ms; threshold too tight to activate the now-dense LOD hierarchy, see Known Issues)

Per-cascade culling closed ~8ms of the CSM regression on Dragon (32 -> 24ms) and ~6ms on City (73 -> 67ms) by filtering the CPU draw list against each cascade's orthographic frustum before submitting, so most clusters land in only one or two cascades instead of all three.

Per-pass GPU (Dragon, steady state): cull 0.1ms, sel 0.0ms (CPU), occ 0.05ms, shadow 3-4ms, main 3-4ms, hzb 0.1-0.2ms.

Per-frame CPU (emitted every 60 frames as `MERIDIAN_CPU: ...`, measured post-CSM + per-cascade culling):
- Application-side work (traverse, residency, build, upload, cmdrec) is under 1ms on both scenes (dragon ~0.35ms, city ~1.0ms).
- `vkQueueSubmit` is 7.8ms on Dragon, 21.8ms on City. Roughly 2x the pre-CSM numbers (3.8ms / 10.6ms) because we now issue 4 indirect-draw calls per frame (main + 3 shadow cascades) instead of 1; per-cascade culling trimmed each cascade's draw count substantially but the per-submission Metal translation cost still scales with total draws across all passes. Only addressable further by reducing total draws (e.g. GPU-side draw packing, texture-array draws) or a native Metal backend.
- `vkWaitForFences` reflects GPU execution time, not CPU overhead.

## Not Yet Implemented

- Per-cluster backface culling with normal cones on LOD clusters (currently base only; many LOD clusters have cones but shader `emit_lod` skips the test)
- Real streaming scheduler (CPU prototype exists but pages start all-resident)
- Async disk I/O for page loading
- Benchmark automation vs stock Godot
- Texture/UV support
- Broader glTF import coverage
- Compressed geometry payloads
- Deeper Godot runtime integration
- Parallel GPU traversal (BFS-per-level or workgroup-DFS) to replace the retained-but-not-dispatched serial compute_select.comp

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
- **Page residency initialized as all-resident by default**; pass `--demand-streaming` to run the StreamingScheduler + simulated-async-load path. Under that flag pages start unloaded (except the first 64 seeded so the hierarchy root has something to descend into), the scheduler throttles loads to `streaming_max_loads_per_frame` per frame, and a page's `loading -> resident` transition is delayed by `streaming_load_latency_frames` frames. Real disk I/O (mmap or GCD/AIO) is still not wired -- loads don't touch the filesystem yet, the .vgeo payload is already in memory from startup.
- **City has sparse node-LOD links (architectural, not a bug in a specific call site)**: `meridian_dump` on `massive_city.vgeo` reports 6230 LOD groups but only 8 node-LOD links, vs Stanford Dragon's 2509 groups -> 946 links (~10% of nodes). Root cause: `build_node_lod_links` (builder_cluster.cpp) requires an EXACT span match between an LOD group's runtime cluster indices and a hierarchy node's cluster span, because `try_select_lod_group` replaces the node's entire base-cluster subtree with the group's LOD clusters; relaxing to "contains" would leave uncovered base clusters unrendered. The builder generates hierarchy partitions via `meshopt_partitionClusters` and LOD groups via `clusterlod` -- both spatial, but using different algorithms that don't align for topologically diverse scenes. Dragon is a single connected mesh and aligns well; city (glTF with many building instances, reordered across material sections) does not. A proper fix is either (a) align partitioning with clusterlod's groupings, (b) teach traversal to stitch partial LOD coverage with complement base-span emits, or (c) rebuild the hierarchy from LOD group boundaries. All three are substantial refactors and not yet attempted.
- **CPU cluster selection divergence on sparse-LOD-link scenes**: on `massive_city` the CPU `simulate_traversal` returns 0 LOD clusters + 31394 base clusters while the pre-hoist GPU `compute_select.comp` produced 15106 draws on the same data. CPU path is semantically identical to GPU shader (LOD-group-first, then base emit if leaf/acceptable error, else descend). Diagnostic pass showed 99.997% of city's base clusters have `normal_cone.w = 1.0` (meshoptimizer-marked degenerate), so backface culling is a no-op regardless of path. Root cause of the GPU's 15106 figure is unexplained -- possibly a pre-existing bug, stale residency state, or compute_cull producing `cull_visible_instances=0` in some frames (which makes compute_select a no-op). Not worth further investigation until city has a usable LOD hierarchy.

### What's Validated

- CPU and GPU selection match exactly on scenes with well-connected LOD hierarchies (Dragon: both emit 8628 after normal-cone backface cull). Diverge on sparse-LOD scenes (see Known Issues).
- Visibility buffer readback confirms `visibility_selection_subset=true` on most benchmark scenes
- Tested assets: 5 synthetic benchmarks, pirate.glb (5K tris), Stanford Dragon (871K tris), generated city (1M tris)
- Platform: macOS Apple M4, MoltenVK, Vulkan 1.2

### Performance (Apple M4, MoltenVK, 1280x720)

Stanford Dragon (871K tris): median 24.0ms / 41 FPS with 3-cascade CSM + per-cascade culling + 8-tap Poisson PCF
Massive City (1M tris): median 66.7ms / 14.9 FPS with 3-cascade CSM + per-cascade culling + 8-tap Poisson PCF

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

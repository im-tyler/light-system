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
4. Shadow pass (depth-only render from light orthographic projection, 2048px, depth bias) -- GPU graphics
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
- **Meshlet boundary seams**: adjacent clusters don't share vertices, creating potential lighting discontinuities at cluster boundaries. Smooth vertex normals reduce the visual impact but don't fully eliminate seams.
- **Page residency initialized as all-resident**: no real demand-driven streaming. All geometry loaded at startup.
- **CPU cluster selection divergence on sparse-LOD-link scenes**: on `massive_city` the CPU `simulate_traversal` returns 0 LOD clusters + 31394 base clusters while the pre-hoist GPU `compute_select.comp` produced 15106 draws on the same data. CPU path is semantically identical to GPU shader (LOD-group-first, then base emit if leaf/acceptable error, else descend). Diagnostic pass showed 99.997% of city's base clusters have `normal_cone.w = 1.0` (meshoptimizer-marked degenerate), so backface culling is a no-op regardless of path. Root cause of the GPU's 15106 figure is unexplained -- possibly a pre-existing bug, stale residency state, or compute_cull producing `cull_visible_instances=0` in some frames (which makes compute_select a no-op). Not worth further investigation until city has a usable LOD hierarchy. City's builder produces only 8 node-LOD links for 6230 LOD groups, which is the actual root cause of the poor traversal outcomes.
- **No CPU cluster-level frustum culling**: only instance-level frustum cull runs (on GPU `compute_cull`). A cluster-level test would help scenes where the camera sees a small portion of the asset.

### What's Validated

- CPU and GPU selection match exactly on scenes with well-connected LOD hierarchies (Dragon: both emit 8628 after normal-cone backface cull). Diverge on sparse-LOD scenes (see Known Issues).
- Visibility buffer readback confirms `visibility_selection_subset=true` on most benchmark scenes
- Tested assets: 5 synthetic benchmarks, pirate.glb (5K tris), Stanford Dragon (871K tris), generated city (1M tris)
- Platform: macOS Apple M4, MoltenVK, Vulkan 1.2

### Performance (Apple M4, MoltenVK, 1280x720)

Stanford Dragon (871K tris): median 17.1ms / 53-72 FPS
Massive City (1M tris): median 53-56ms / 18-20 FPS (bottlenecked by sparse-LOD selection + degenerate normal cones + vkQueueSubmit translation cost scaling with draw count)

Per-pass GPU (Dragon, steady state): cull 0.1ms, sel 0.0ms (CPU), occ 0.05ms, shadow 3-4ms, main 3-4ms, hzb 0.1-0.2ms.

Per-frame CPU (emitted every 60 frames as `MERIDIAN_CPU: ...`):
- Application-side work (traverse, residency, build, upload, cmdrec) is under 1ms on both scenes.
- `vkQueueSubmit` is 3.8ms on Dragon, 10.6ms on city -- scales with draw count because MoltenVK translates vkCmdDrawIndirectCount into a Metal draw-call loop on the CPU thread. Fundamental MoltenVK overhead; only addressable by reducing draws or a native Metal backend.
- `vkWaitForFences` reflects GPU execution time, not CPU overhead.

## Not Yet Implemented

- Cascaded shadow maps (current shadow pass is single-map orthographic)
- Per-cluster backface culling with normal cones on LOD clusters (currently base only; many LOD clusters have cones but shader `emit_lod` skips the test)
- CPU cluster-level frustum culling
- Real streaming scheduler (CPU prototype exists but pages start all-resident)
- Async disk I/O for page loading
- Benchmark automation vs stock Godot
- Texture/UV support
- Broader glTF import coverage
- Compressed geometry payloads
- Deeper Godot runtime integration
- Parallel GPU traversal (BFS-per-level or workgroup-DFS) to replace the retained-but-not-dispatched serial compute_select.comp

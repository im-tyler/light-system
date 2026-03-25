# Implementation Status

Last updated: 2026-03-25

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

Per-frame GPU pipeline order:
1. Compute instance culling (frustum 6-plane AABB test, atomic append)
2. Compute cluster/LOD selection (iterative DFS hierarchy traversal, 2048-deep stack, residency-aware)
3. Occlusion refinement (project cluster AABB against previous frame's HZB -- skipped on frame 0)
4. Shadow pass (depth-only render from light orthographic projection, 2048px, depth bias)
5. Main geometry pass (vertex pulling from payload SSBOs, face normal + hemisphere ambient + directional lighting + shadow sampling)
6. HZB construction (depth-copy compute shader + per-mip max-downsample cascade)

### Data Flow (Connected)

- GPU compute selection outputs GpuDrawEntry array (32 bytes each: VkDrawIndirectCommand header + per-draw metadata)
- CPU reads back draw list after fence wait; drives per-draw vkCmdDraw calls for both shadow and main passes
- Frame 0 uses CPU TraversalSelection fallback (no prior GPU output)
- Frame 1+ driven entirely by GPU compute selection readback
- Shadow map rendered to depth texture, sampled in main fragment shader via sampler2DShadow
- Visibility buffer: two-word RG32_UINT encoding matching visibility_format.h spec (instance, kind, index, local_triangle)
- Occlusion refinement output available but readback deferred until indirect draws eliminate frame latency

### Interactive Mode

- `--interactive` CLI flag: continuous present loop until ESC
- WASD movement, mouse look, Q/E vertical, camera auto-oriented toward scene center
- FPS and draw count displayed in window title

### Known Issues

- **Per-draw vkCmdDraw overhead**: each cluster requires a separate vkCmdDraw call with push constants. On the 871K-triangle Stanford Dragon (8871 clusters), this produces ~17K draw calls per frame = ~1 FPS. Indirect draws (vkCmdDrawIndirect) would collapse this to 1-2 calls per pass.
- **Indirect draw migration blocked**: MoltenVK crashes when descriptor binding 3 (shadow sampler) is uninitialized at pipeline creation time. Requires either VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, restructured context creation order, or placeholder image lifecycle management.
- **Flat face normals only**: no smooth vertex normals from source mesh data. Creates faceted appearance on curved surfaces.
- **No texture support**: all shading is procedural (per-cluster color hash + hemisphere ambient). No UV interpolation or texture sampling.
- **Meshlet boundary seams**: adjacent clusters don't share vertices, creating potential lighting discontinuities at cluster boundaries.
- **Page residency initialized as all-resident**: no real demand-driven streaming. All geometry loaded at startup.

### What's Validated

- GPU selection matches CPU traversal exactly on all scenes (parity verified via readback counters)
- Visibility buffer readback confirms `visibility_selection_subset=true` on all benchmark scenes
- Tested assets: 5 synthetic benchmarks, pirate.glb (5K tris), Stanford Dragon (871K tris), generated city (1M tris)
- Platform: macOS Apple M4, MoltenVK, Vulkan 1.2

## Not Yet Implemented

- vkCmdDrawIndirect (blocked on MoltenVK descriptor lifecycle -- see known issues)
- Real streaming scheduler (CPU prototype exists but pages start all-resident)
- Async disk I/O for page loading
- Frame timing / GPU profiling (Vulkan timestamp queries)
- Benchmark automation vs stock Godot
- Smooth vertex normals from source mesh
- Texture/UV support
- Broader glTF import coverage
- Compressed geometry payloads
- Deeper Godot runtime integration

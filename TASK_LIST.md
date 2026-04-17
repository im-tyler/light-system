# Task List

Last updated: 2026-04-02

## Completed

### Offline Builder
- [x] vendored meshoptimizer
- [x] manifest parser, .obj loader, .gltf/.glb loader
- [x] meshlet generation, bounds, optimization
- [x] hierarchy construction, LOD groups via clusterlod
- [x] page packing, .vgeo serialization, validation
- [x] seam-safe simplification, page dependencies
- [x] CPU traversal/residency prototypes, replay harness

### GPU Pipeline Foundation
- [x] Vulkan bootstrap (MoltenVK + GLFW, device, swapchain)
- [x] scene buffer upload (all GPU ABI structs)
- [x] GPU vertex pulling from payload SSBOs (replaced CPU debug mesh expansion)
- [x] compute instance culling (frustum AABB, atomic append)
- [x] compute cluster/LOD selection (iterative DFS, 2048-deep stack, residency-aware)
- [x] HZB construction (depth-copy compute + mip chain downsample)
- [x] occlusion refinement (cluster AABB projection vs HZB mip levels)
- [x] directional shadow pass (depth-only, orthographic, depth bias)

### Pipeline Integration
- [x] per-frame UBO (view_projection, light_vp, light_dir -- push constants reduced to 16 bytes)
- [x] vkCmdDrawIndirect for main and shadow passes (placeholder depth image resolves MoltenVK descriptor binding)
- [x] smooth vertex normals stored in payload (builder_cluster.cpp) and interpolated in vertex shader
- [x] shadow map sampling in main fragment shader (sampler2DShadow, world-to-light transform)
- [x] visibility buffer encoding matches spec (RG32_UINT, two-word format per visibility_format.h)
- [x] interactive camera (WASD/mouse, --interactive CLI flag, FPS in title)
- [x] occlusion feedback loop closed (prev-frame HZB used for current-frame refinement)
- [x] page residency initialized as all-resident (GPU traversal was emitting 0 draws when pages were unloaded)
- [x] gl_FrontFacing normal correction for inconsistent mesh winding
- [x] hemisphere ambient lighting + per-cluster color variation

### Test Assets
- [x] Stanford Dragon downloaded and built (871K tris, 8871 clusters, 56MB .vgeo)
- [x] 1M-triangle procedural city generated (31K clusters, 104MB .vgeo)

## Next Priority: Rendering Quality

These directly affect visual output and must be fixed before the renderer looks credible:

- [x] smooth vertex normals from source mesh (normals stored in payload, interpolated in vertex shader)
- [-] fix meshlet boundary seams (partially addressed by smooth normals; shared-vertex merging at cluster boundaries still needed)
- [ ] per-cluster backface culling using meshlet normal cones
- [ ] better shadow map quality (PCF filtering, cascaded shadow maps for large scenes)

## Next Priority: Performance

- [ ] vkCmdDrawIndirectCount optimization (vkCmdDrawIndirect works but uses max_draws, causing no-op draws for zero-instance entries; needs actual count buffer)
- [ ] frame timing and GPU profiling (Vulkan timestamp queries per pass)

## Next Priority: Streaming

- [ ] CPU streaming scheduler with real page loading/eviction
- [ ] async disk I/O for non-blocking page loads
- [ ] staging buffer for device-local memory

## Later

- [ ] benchmark automation vs stock Godot
- [ ] broader glTF import coverage
- [ ] compressed geometry payloads
- [ ] deeper Godot runtime integration

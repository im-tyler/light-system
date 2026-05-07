# Task List

Last updated: 2026-04-16

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

## Performance (done / open)

- [x] GPU profiler -- Vulkan timestamp queries per pass, `MERIDIAN_GPU:` line every frame
- [x] vkCmdDrawIndirectCount with runtime extension probe + fallback to vkCmdDrawIndirect
- [x] hoist cluster/LOD selection to CPU (serial GPU DFS was 10-18ms on M4; replaced with CPU simulate_traversal + HOST_COHERENT upload)
- [x] per-cluster backface culling via meshlet normal cones (base clusters only; LOD clusters still unculled)
- [ ] reconcile CPU/GPU selection divergence on sparse-LOD-link scenes (post-LOD-fix, the remaining gap is that `cluster_select.comp` hasn't been rewritten against the multi-run coverage model -- it still uses the pre-fix single-group-per-node logic; the GPU shader isn't dispatched in the current pipeline)
- [x] fix city builder producing only 8 node-LOD links for 6230 LOD groups (dragon 946->2509, city 8->6230; builder now attaches groups to deepest containing node with multi-run base cluster coverage, traversal filters via a coverage bitmap)
- [x] CPU cluster-level frustum culling (landed in 6e19a34; each cluster's AABB is tested against the camera frustum in the CPU draw-build loop before emit)
- [x] profile the ~9ms CPU-side overhead still remaining after the sel hoist (attributed to vkQueueSubmit MoltenVK translation: Dragon 7.8ms, City 21.8ms submit; application work <1ms on both)

## Rendering Quality

- [x] smooth vertex normals from source mesh
- [x] meshlet boundary seams (angle-weighted smooth normals + position welding of vertex normals across index-split duplicates in the source mesh)
- [x] PCF shadow filtering (8-tap Poisson disk + per-pixel rotation, slope-scaled bias)
- [x] cascaded shadow maps for outdoor / large scenes (3-cascade, log/uniform split blend, manual depth compare to work around MoltenVK sampler2DArrayShadow limitation)
- [x] per-cascade frustum culling to reduce 3x draw-submit cost (each cascade now pulls a CPU-filtered subset of the main draw list against its own orthographic frustum; shadow.cascade_draw_lists[3] + shadow.cascade_descriptor_sets[3])

## Streaming

- [x] connect streaming_scheduler.cpp to frame loop (request/load/evict state machine, gated behind `--demand-streaming`)
- [x] real async disk I/O for non-blocking page loads (AsyncReader: worker thread + pread(); serialises the resource to a temp .vgeo on demand-streaming startup, submits real reads for page byte ranges, drains completions on the main thread to transition pages from loading -> resident)
- [x] staging buffer for device-local memory (`create_device_local_buffer_staged` routes payload uploads through a HOST_VISIBLE staging buffer + vkCmdCopyBuffer; transparently falls back to the existing HOST_COHERENT path on unified-memory platforms like Apple Silicon where DEVICE_LOCAL and HOST_VISIBLE share the same heap)
- [x] root-page autodetect so the demand-streaming seed set doesn't rely on index-0 ordering (seed = pages picked by a coarse `simulate_traversal` with all-resident mask + infinite error threshold, capped at `streaming_seed_pages`)

## Traversal (deferred)

- [ ] parallel GPU traversal (BFS-per-level or workgroup-DFS) to replace the retained-but-not-dispatched serial compute_select.comp; worth building only after profiling proves CPU selection is actually the bottleneck for some class of scene

## Later

- [ ] benchmark automation vs stock Godot
- [ ] broader glTF import coverage
- [ ] texture / UV support
- [ ] compressed geometry payloads
- [ ] deeper Godot runtime integration

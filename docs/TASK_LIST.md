# Task List

Last updated: 2026-09-08
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

## Robustness (2026-09-05)

- [x] Vulkan validation layers + debug-utils messenger behind `--validate` (repairs: render-pass depth format ordering, indirect buffer usages, per-image present semaphores, HZB layout tracking, sampled usage on the depth image)
- [x] window resize + swapchain recreation (dynamic viewport/scissor, out-of-date/suboptimal recovery, surface-resource recreation)
- [x] temp .vgeo leak on error paths (cleanup owns the file)
- [x] indirect fallback ghost draws (zeroed draw-list tails via high-water upload)
- [x] CI (GitHub Actions: build + builder/dump/trace/replay smoke)
- [x] dragon + city replay scripts

## Performance (done / open)

- [x] GPU profiler -- Vulkan timestamp queries per pass, `MERIDIAN_GPU:` line every frame
- [x] vkCmdDrawIndirectCount with runtime extension probe + fallback to vkCmdDrawIndirect
- [x] hoist cluster/LOD selection to CPU (serial GPU DFS was 10-18ms on M4; replaced with CPU simulate_traversal + HOST_COHERENT upload)
- [x] per-cluster backface culling via meshlet normal cones (base clusters since April; LOD clusters since 2026-09-05 via schema v3 -- dragon draws 8366 -> 6467)
- [x] fix city builder producing only 8 node-LOD links for 6230 LOD groups (dragon 946->2509, city 8->6230; builder now attaches groups to deepest containing node with multi-run base cluster coverage, traversal filters via a coverage bitmap)
- [x] CPU cluster-level frustum culling (landed in 6e19a34; each cluster's AABB is tested against the camera frustum in the CPU draw-build loop before emit)
- [x] profile the ~9ms CPU-side overhead still remaining after the sel hoist (attributed to vkQueueSubmit MoltenVK translation: Dragon 7.8ms, City 21.8ms submit; application work <1ms on both)
- [x] repin vendored meshoptimizer (c645e49) with tools/vendor_thirdparty.sh; builder provenance fixed against the upstream optimize_clusters_level change (2026-09-05)
- [ ] rebuild cluster_select.comp against the multi-run coverage model (or drop it) -- only worth doing if profiling shows CPU selection as a bottleneck
- [x] city-class LOD usefulness: fixed 2026-09-06 — position-remapped partitioning + district hierarchy levels (see IMPLEMENTATION_STATUS); smooth threshold ladder, city t=1.0 at 90.6ms/21K draws vs 143.5ms/31K full detail

## Rendering Quality

- [x] smooth vertex normals from source mesh
- [x] meshlet boundary seams (angle-weighted smooth normals + position welding of vertex normals across index-split duplicates in the source mesh)
- [x] PCF shadow filtering (8-tap Poisson disk + per-pixel rotation, slope-scaled bias)
- [x] cascaded shadow maps for outdoor / large scenes (3-cascade, log/uniform split blend, manual depth compare to work around MoltenVK sampler2DArrayShadow limitation)
- [x] per-cascade frustum culling to reduce 3x draw-submit cost (each cascade now pulls a CPU-filtered subset of the main draw list against its own orthographic frustum; shadow.cascade_draw_lists[3] + shadow.cascade_descriptor_sets[3])
- [x] shadow caster LOD (2026-09-07): second simulate_traversal at 8x the main error threshold selects casters for the shadow pass (`--shadow-error-scale`, <= 1 disables); residency merges shadow-selection pages under --demand-streaming. City shadow draws 20746 -> 3239 / GPU shadow 3.2-3.9 -> 0.6-1.5ms, dragon 5532 -> 800 / ~4.5 -> 0.32ms; city 21.2 -> 12.1ms, dragon 11.1 -> 8.6ms paired under load (see benchmarks/godot/RESULTS.md)
- [x] instance-folded draw submission (2026-09-07): MoltenVK encodes one Metal draw per indirect-draw entry, so both CPU draw lists now stable-partition into vertex-count buckets (quartile edges) and submit one instanced vkCmdDraw per nonempty bucket; vertex shaders collapse corners past a cluster's own triangle count (and unused shadow stride slots) to zero-area triangles. City main encodes 20732 -> 2, shadow 3239 -> 2; submit 6.6-6.9 -> 1.9-2.2ms; paired city 14.0 -> 8.5ms, dragon unchanged ~8.3ms; dragon pixel-identical, city 0.047% depth-equal order flips; draw selection unchanged, true-drawIndirectCount path unchanged (see benchmarks/godot/RESULTS.md)
- [x] parallel CPU traversal + draw build (2026-09-07): fork-join pool (parallel_exec.h, --threads, auto = min(hw, 8)); main/shadow DFS run concurrently, each forks subtree tasks (>=512-cluster children, 2x-thread task budget), draw build chunked ~1536 indices/job; ordered child-order merges keep selection + draw lists bit-identical to serial (meridian_trace --parallel asserts it; TSan clean; dragon AND city pixel-identical to the serial build). City traverse+build 5.82 -> 3.69ms paired under load (see benchmarks/godot/RESULTS.md); idle rerun pending
- [x] submit-path + main-pass GPU items (2026-09-08): dead per-frame passes moved to a post-loop diagnostic epilogue (cull, occ, HZB, visibility readback copy — none feed draws on the MoltenVK fallback), visibility attachment store elided on non-capture frames (transient render pass), `--no-gpu-timers` flag, fold reports wasted VS invocations. City GPU total 2.78-3.95 -> 2.05-2.38ms clean pair, wall -0.1-0.2ms; dragon AND city pixel-identical. Residual ~2ms submit profiled to Metal command-buffer encoding (prefill experiment moves it exactly to cmdrec) — closed as a Metal-level floor pending idle rerun; degenerate-corner waste counted at ~3-4.5% of main VS work (non-item, more buckets don't change the fold); main-pass GPU 1.2-1.5ms city is real shading cost (see benchmarks/godot/RESULTS.md)

## Streaming

- [x] connect streaming_scheduler.cpp to frame loop (request/load/evict state machine, gated behind `--demand-streaming`)
- [x] real async disk I/O for non-blocking page loads (AsyncReader: worker thread + pread(); serialises the resource to a temp .vgeo on demand-streaming startup, submits real reads for page byte ranges, drains completions on the main thread to transition pages from loading -> resident)
- [x] staging buffer for device-local memory (`create_device_local_buffer_staged` routes payload uploads through a HOST_VISIBLE staging buffer + vkCmdCopyBuffer; transparently falls back to the existing HOST_COHERENT path on unified-memory platforms like Apple Silicon where DEVICE_LOCAL and HOST_VISIBLE share the same heap)
- [x] root-page autodetect so the demand-streaming seed set doesn't rely on index-0 ordering (seed = pages picked by a coarse `simulate_traversal` with all-resident mask + infinite error threshold, capped at `streaming_seed_pages`)

## Traversal (deferred)

- [ ] parallel GPU traversal (BFS-per-level or workgroup-DFS) to replace the retained-but-not-dispatched serial compute_select.comp; worth building only after profiling proves CPU selection is actually the bottleneck for some class of scene

## Ontos Viewer Bridge

- [x] ontos_view accepts spec-20 RegionMultipole records (tag 9, 2026-09-08)
- [x] spec 21-22 contact + modal audio (2026-09-08): dump tool re-simulates the contact pass and resynthesizes the audio bit-exactly (`--wav`, FNV hash on the OK line); ontos_view parses tag 10 and renders the WAV offline (`--wav`); CI cross-verifies both against the committed simval corpora on ubuntu + macos
- [x] realtime audio device output in ontos_view (2026-09-08): CoreAudio AudioQueue at 65536 Hz stereo — each contact spawns a voice at its tick crossing running the spec-22 recurrence unmodified, spatialized with camera-relative constant-power pan + distance attenuation (viewer-side gains only; the offline --wav mono reference is unchanged and still cmp-identical with the simval goldens)
- [x] linux realtime audio path in ontos_view (2026-09-08): ALSA write thread pulling the same shared VoiceBank as CoreAudio (identical voice pool/mix semantics), spec content at 65536 Hz with deterministic linear resampling when the device lacks the native rate, float32 device format with s16 fallback, RAII drop-on-stop; new ubuntu CI job compile/link-checks the viewer with libasound (runtime device testing still manual); offline --wav renders cmp-identical from both macOS and linux builds
- [x] contact-event visualization in ontos_view (2026-09-08): expanding flash rings at Contact midpoints — ring shape added to the ontos billboard shader, size from the colliding bodies, fade over 4 playback ticks

## Later

- [ ] benchmark automation vs stock Godot
- [ ] broader glTF import coverage
- [ ] texture / UV support
- [ ] compressed geometry payloads
- [ ] deeper Godot runtime integration

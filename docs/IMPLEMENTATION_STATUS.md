# Implementation Status

Last updated: 2026-09-08

## Phase 1: Offline Builder (Complete)

- `meridian_builder` CLI: manifest -> meshlet generation -> hierarchy -> LOD -> page packing -> .vgeo serialization
- `.obj` and `.gltf`/`.glb` import (sparse accessors, meshopt compression, seam locking)
- meshlet generation, optimization, and bounds via meshoptimizer
- partition-based hierarchy tree with clusterlod simplification-backed LOD groups
- seam-safe cross-material simplification
- dual payload domains (base clusters + LOD clusters) with page table
- adjacent replacement-level page dependency hints for streaming prefetch
- `meridian_dump`, `meridian_trace`, `meridian_residency`, `meridian_replay` CLI tools
- `ontos_stream_dump` (tools/ontos, standalone C++17 no-deps): third independent implementation of the ontos stream spec (after ontos's Rust and simval's Python); verifies streams bit-for-bit, including spec-21 contact dynamics (re-simulated impulse pass) and spec-22 modal audio (bit-exact WAV + FNV hash, `--wav`). Spike toward the ontos Phase 3 viewer bridge (JoltViewer thin-consumer pattern, zero coupling to the renderer core). CI cross-verifies it against the committed simval corpora on every push (ubuntu + macos).
- validated on synthetic benchmarks + external pirate.glb + Stanford Dragon (871K triangles) + generated 1M-triangle city

## Phase 2: Standalone Vulkan Renderer (In Progress)

### GPU Pipeline (Working)

Per-frame pipeline order:
1. Cluster/LOD selection -- **CPU** (`simulate_traversal`) with normal-cone backface cull; output uploaded to the HOST_COHERENT draw-list buffers. The serial DFS compute shader is retained but not dispatched. (The GPU instance-cull dispatch also runs only in the post-loop diagnostic epilogue since 2026-09-08 — its output fed only the not-dispatched selection shader.)
2. Shadow pass (3 cascaded shadow maps, depth-only render from per-cascade orthographic projections that tight-fit the camera sub-frusta, 2048px per cascade in a 2D-array depth image, log/uniform split blend lambda=0.7, depth bias) -- GPU graphics. Caster selection uses a second CPU traversal at a coarser error threshold (shadow caster LOD, default 8x `--shadow-error-scale`), so the shadow pass draws far fewer, coarser clusters than the main pass.
3. Main geometry pass (vertex pulling from payload SSBOs, smooth vertex normals + hemisphere ambient + directional lighting + 8-tap Poisson-disk PCF shadow with per-pixel rotation and slope-scaled bias) -- GPU graphics. The visibility attachment stores only on the capture frame (post-loop readback; transient render pass otherwise).
4. Post-loop diagnostic epilogue (2026-09-08, fallback path): one-shot submit that runs the instance-cull dispatch, rebuilds the HZB from the final frame's depth, runs the occlusion-refine dispatch for the survivor-count report, and copies the visibility image to the readback buffer. On the drawIndirectCount path, occ + HZB still run per frame (the refined list feeds draws there) and only cull + visibility-copy live in the epilogue.

GPU timestamp profiler emits `MERIDIAN_GPU: cull=.. sel=.. occ=.. shadow=.. main=.. hzb=.. total=..ms` every frame (skippable via `--no-gpu-timers`); on the fallback path cull/occ/hzb read ~0 because those passes run in the epilogue instead.

### Data Flow (Connected)

- CPU `simulate_traversal` runs every frame, producing selected base + LOD clusters
- CPU converts selection to `GpuDrawEntry[]` (32 bytes each: VkDrawIndirectCommand header + per-draw metadata) with normal-cone backface cull and writes to HOST_COHERENT draw_list/draw_count buffers
- Draw submission: on devices with real `drawIndirectCount`, both passes consume the draw list via `vkCmdDrawIndirectCount`. The fallback (MoltenVK — the extension is advertised but the symbol is a no-op stub) instance-folds each list into vertex-count buckets and issues one instanced `vkCmdDraw` per nonempty bucket; the vertex shaders degenerate corners past each cluster's own triangle count (and unused shadow stride slots), so draw selection is byte-identical between paths.
- Shadow map rendered to depth texture, sampled in main fragment shader via `sampler2DShadow`
- Visibility buffer: two-word RG32_UINT encoding matching visibility_format.h spec (instance, kind, index, local_triangle)
- Normal-cone backface culling on both base and LOD clusters (schema v3 stores LOD cones; dragon steady-state draws 8366 -> 6467)
- `meridian_vk_bootstrap --error-threshold` CLI override; `meridian_trace` reports LOD group error distribution
- Occlusion refinement output available but readback deferred until indirect draws eliminate frame latency

### Interactive Mode

- `--interactive` CLI flag: continuous present loop until ESC
- WASD movement, mouse look, Q/E vertical, camera auto-oriented toward scene center
- FPS and draw count displayed in window title
- Resizable window: framebuffer-size changes and swapchain out-of-date/suboptimal recreate the surface resources instead of exiting
- `--validate` enables the Khronos validation layer + debug-utils messenger (clean on the benchmark scenes; one known MoltenVK warning about blend state on the uint visibility target with blending disabled)

### Known Issues

- **Texture/UV support landed 2026-09-07** (schema v4): per-vertex UVs through base + LOD clusters (attribute-aware simplification), deterministic embedded checker texture, opt-in via manifest `emit_texture = true`; old v3 files still load; texel sampling verified via screenshot pixel counts; dragon unregressed. Remaining: per-material textures, image decode, mipmaps, OBJ `vt` import.
- **Meshlet boundary seams (residual)**: smooth normals are now angle-weighted and position-welded in the builder (`compute_smooth_normals`), which matches normal values across index-split duplicates at the same position. Any remaining boundary seams come from LOD-level T-junctions at cluster borders of different detail, which are mitigated but not fully eliminated by seam-locked vertex simplification.
- **City LOD hierarchy fixed (2026-09-06)**: the degeneracy was attachment collapse, not the error ladder. `partition_cluster_ids` fed raw vertex indices to `meshopt_partitionClusters` while the clusterlod DAG partitions position-remapped indices, so on index-split geometry (thousands of disconnected boxes) the two partitioners disagreed on adjacency; combined with a flat one-cut hierarchy (no intermediate levels), all LOD-group attachments piled onto ~43 near-root nodes and selection collapsed to a cliff. Fix (builder-side): partitions now use position-remapped indices, and `build_temp_hierarchy` grows district targets (`max(partition_size, count/8)`) so the tree gains fanout-8 intermediate levels. City trace now has a smooth ladder (t=0.05: 1894 groups / 14K+15K clusters -> t=8: 60+57; no cliff). Interleaved A/B on the renderer: city at t=1.0 runs 90.6ms / 21223 draws vs 143.5ms / 31394 at full detail (-37% frame time); dragon improves too (5532 vs 8543 draws at default threshold, no regression; `visibility_selection_subset=true` holds). Remaining (lower severity): the clusterlod DAG still smears provenance (~1.9x overlap at depth 1), so ~5K base clusters stay uncovered at mid thresholds and draw counts are slightly non-monotonic around t=4; depth-0 groups are no-op replacements.
- **Page residency / payload streaming (2026-09-06: mmap path landed)**: default stays all-resident; `--demand-streaming` now mmaps the serialized `.vgeo` (madvise WILLNEED readahead), worker completions carry page bytes, payload buffers are allocated empty at full size (uncommitted on unified memory) and populated per-page via direct sub-range `vkMapMemory` memcpys (staging + `vkCmdCopyBuffer` fallback for discrete DEVICE_LOCAL). Seed pages upload synchronously before frame 0; CPU-side payload copies are freed after seeding (the mmap is the byte source). Verified: dragon streams 1447 pages/30.4MB with parity + visibility-subset holding; `--budget 96` pins residency through evict/reload cycles; city at threshold 0.05 streams 59MB of 155MB, 103.8ms vs 114.2ms on the pread+staging path; temp-file/mmap failure falls back to full upload + latency sim; default path unregressed. Remaining: mmap persisted `.vgeo` directly (skip the startup temp write); MADV_DONTNEED after eviction.
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

Shadow caster LOD (2026-09-07 17:34): the shadow pass selects casters from a second `simulate_traversal` at 8x the main error threshold (`--shadow-error-scale`, <= 1 disables and shares the main selection); residency merges the shadow selection's pages for `--demand-streaming`. City shadow draws 20746 -> 3239, dragon 5532 -> 800; GPU shadow pass city 3.2-3.9 -> 0.6-1.5ms, dragon ~4.5 -> 0.32ms. Under-load paired A/B: city 21.2 -> 12.1ms, dragon 11.1 -> 8.6ms (idle-machine Godot city reference 4.12ms unthrottled: gap ~4.9x -> ~2.9x).

Instance-folded draw submission (2026-09-07 20:35): MoltenVK has no multi-draw indirect, so each per-cluster indirect-draw entry cost one Metal draw encode (~0.15us). Both draw lists are now stable-partitioned into vertex-count buckets (quartile edges) and submitted as one instanced `vkCmdDraw` per nonempty bucket — the vertex shaders already resolved entries via `gl_InstanceIndex` (firstInstance = folded global entry index; shadow stride x4), and corners past a cluster's own count collapse to zero-area triangles. City main encodes 20732 -> 2, shadow 3239 -> 2; submit 6.6-6.9 -> 1.9-2.2ms; paired under-load city 14.0 -> 8.5ms (gap to Godot ~2.9x -> ~1.6-2.1x), dragon unchanged at ~8.3ms (submit there is fixed-cost dominated). Dragon pixel-identical; city 0.047% pixel diff from depth-equal order flips on coincident surfaces. Draw selection, caster LOD, and the true-drawIndirectCount path are unchanged.

Parallel CPU traversal + draw build (2026-09-07 23:15): the two per-frame `simulate_traversal` DFS (main + shadow-caster) and the selection->GpuDrawEntry conversion now run on a small fork-join pool (`prototype/src/parallel_exec.h`; `--threads N`, default auto = min(hardware_concurrency, 8); 1 = serial). The DFS forks subtree tasks where all children span >= 512 base clusters (2x-threads task budget); the build chunks ~1536 selection indices per job. Output is bit-identical to the serial path at any thread count — coverage marks are scoped to ancestor descents so forking children run on byte-copies and an ordered child-order merge reproduces the serial first-encounter order of every deduplicated list; `meridian_trace --parallel <t>` asserts the vector equality (true on all benchmark scenes, TSan-clean). Paired under load ~20: city traverse+build 5.82 -> 3.69ms (-37%); dragon ~1.3ms (unchanged, already cheap). Dragon AND city render pixel-identical to the serial build (0/921600 pixels >10). Idle-machine numbers pending.

Dead-pass pruning + diagnostic epilogue (2026-09-08): on the MoltenVK fallback the per-frame command stream no longer carries work no draw path consumes — the instance-cull dispatch (output only fed the not-dispatched cluster_select), the occlusion-refine dispatch (only drawIndirectCount consumes its GPU-written list), the HZB build, and the 7.3MB visibility readback copy all moved to a one-shot post-loop epilogue submit that refreshes the same report fields (occ survivors now computed against the final frame's own HZB: 14020 vs 14015 on city, 0.04% diagnostic drift). The visibility attachment also skips its store-back on non-capture frames (second render pass, storeOp DONT_CARE, compatible/shared framebuffers; capture frame = final index of a fixed-count run, every frame while interactive). City GPU total 2.78-3.95 -> 2.05-2.38ms clean pair; wall -0.1-0.2ms city, flat dragon; dragon AND city pixel-identical (draw order untouched). The remaining ~2ms "submit" cost was profiled to the Metal command-buffer encode itself (MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS moves it exactly from submit to cmdrec; total unchanged) — the stream is now minimal and further cuts need Metal-level work or an idle-machine rerun. New `--no-gpu-timers` flag skips the per-frame timestamp queries (fence -0.3-0.5ms, wall flat under load); the fold reports wasted vertex-shader invocations (`wastedvs` in MERIDIAN_CPU: city main ~3%, dragon ~4.5% of VS work — bucket-count increases don't change the fold, the distribution is too peaked).

Per-pass GPU (Dragon, steady state): cull 0.1ms, sel 0.0ms (CPU), occ 0.05ms, shadow 3-5ms, main 2-4ms, hzb 0.1-0.2ms.

Per-frame CPU (emitted every 60 frames as `MERIDIAN_CPU: ...`, measured post-CSM + per-cascade culling):
- City steady state (2026-09-08, post dead-pass pruning, 8 threads, machine loaded ~20): traverse 2.1-3.0ms, residency 0.15-0.2ms, build 1.2-1.7ms, upload 0.1-0.2ms, cmdrec 0.01-0.02ms, submit 1.6-2.4ms (load-noisy, range 0.57-4.3 across the session; the underlying cost is Metal command-buffer encoding, see 2026-09-08 entry above), fence 1.4-2.0ms (GPU total 2.0-3.3ms). Idle-machine rerun pending.
- Application-side work is under 1ms on dragon (~1.5ms with the fold bookkeeping folded into build).
- `vkWaitForFences` reflects GPU execution time, not CPU overhead.

## Not Yet Implemented

- (done 2026-09-08) persisted `.vgeo` inputs mmap directly (header-validated); startup temp write only for non-.vgeo inputs. Also fixed silent streamed-payload corruption (payload offsets omitted the base-run table — 229KB shift on dragon; streamed vs resident is now bit-identical on terrace) and added MADV_DONTNEED on eviction.
- Per-material textures, real image decode, compression, mipmaps (v1 = one embedded checker), OBJ `vt` import
- Per-material textures, real image decode, compression, mipmaps (v1 = one embedded checker), OBJ `vt` import
- Godot gap after instance-folded draws (2026-09-07 20:35, loaded machine — see benchmarks/godot/RESULTS.md): dragon ~8.3ms at parity with same-load Godot (clears 100fps); city 8.5-8.6ms vs 4.12ms idle unthrottled Godot (~1.6x vs best same-session unthrottled 5.26ms, ~2.1x vs idle reference; was ~2.9x). Main/shadow encodes are now 2+2 per frame; the CPU traverse+build is parallelized (2026-09-07 23:15, city 5.8 -> 3.7ms paired under load, bit-identical output; idle numbers pending). Submit-path item closed 2026-09-08: dead per-frame passes pruned (city GPU total -0.3 to -1.6ms, wall -0.1-0.2ms) and the residual ~2ms submit profiled to Metal command-buffer encoding itself (prefill moves it exactly; not command-count driven anymore) — remaining cuts need Metal-level work. Main-pass GPU 1.2-1.5ms city is real shading cost (degenerate-corner waste counted at ~3-4.5% of VS work; visibility store elided); further reduction means changing pixels. Idle-machine rerun pending.
- Broader glTF import coverage
- **Normal-cone cull FIXED 2026-09-08 (schema v5)**: culls per meshopt's canonical test (dot >= cutoff*len + radius) with bounding-sphere center+radius threaded through cluster records, CPU selection, and compute shaders. The old test was inverted — it culled front-facing tight cones and kept back-facing ones; the documented past 'culling gains' were the wrong half. uv_seam now renders (0 -> 69K visible px); dragon 4101->4002 draws, city 20.9->19.3ms; subset property holds on all scenes.
- (fixed 2026-09-08) screenshot acquire path validated clean.
- Godot comparison harness exists (benchmarks/godot/); results honestly unfavorable — stock Forward+ is faster today (benchmarks/godot/RESULTS.md).
- ontos_view (2026-09-08): plays back ontos v2 gravity streams including spec-19 collapse (tag 8, level 2), spec-20 multipole (tag 9) and spec-21 contacts (tag 10) — billboard bodies colored by region/level, region grid, playback controls, `--frames` headless smoke; depth attachment, MSAA, double-buffered instances, mmap'd stream parsing. Spec coverage matches the golden set. `--wav FILE` renders the spec-22 modal audio offline (deterministic WAV + FNV hash, bit-identical with ontos and simval — the mono spec-22 reference). Interactive playback also plays the contact rings through the audio device live (65536 Hz stereo float; macOS CoreAudio AudioQueue and Linux ALSA write thread, both pulling a shared VoiceBank with identical pool/mix semantics): each contact spawns a voice when playback crosses its tick, advancing the identical spec-22 recurrence; stereo placement is constant-power pan from the contact's screen-x plus distance attenuation referenced to the visible half-height, snapshotted from the camera at spawn (viewer render choice — sample content stays a pure function of stream + camera; wall-clock only schedules when rings start; contact-free streams skip the device). Linux devices that cannot run 65536 Hz natively get simple deterministic linear resampling in the viewer path only — the offline `--wav` reference never resamples; device format is float32 with an s16 fallback (same rounding as the offline render). The Linux backend is compile/link-checked by CI on ubuntu (viewer built with libasound); runtime device testing there remains manual. Contact events additionally draw expanding flash rings at the contact midpoint (ring shape in the ontos billboard shader, fades over 4 playback ticks). No trails.
- Compressed geometry payloads
- Deeper Godot runtime integration
- Parallel GPU traversal (BFS-per-level or workgroup-DFS) to replace the retained-but-not-dispatched serial compute_select.comp
- Ontos viewer bridge beyond the stream-dump spike (region grid as instanced geometry via the existing instance path; waits on ontos Phase 2 fields)

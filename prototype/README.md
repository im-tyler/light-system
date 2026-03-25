# Standalone Prototype

Purpose:

- prove the dense-geometry runtime outside Godot
- profile culling, visibility, streaming, and shadow costs
- provide the first real performance baseline for Meridian itself

Planned contents:

- `src/`: Vulkan runtime code
- `shaders/`: compute and graphics shaders
- `thirdparty/`: local dependencies if needed

Initial priority:

1. Vulkan bootstrap
2. cluster metadata upload
3. instance culling
4. hierarchy traversal
5. visibility buffer
6. HZB
7. material resolve
8. shadow pass

## Current scaffold

The initial executable in this directory is:

- `meridian_builder`: a first-pass offline builder CLI driven by `--manifest`
- `meridian_dump`: a tiny inspector for `.vgeo` summary data
- `meridian_trace`: a CPU-side traversal prototype for node/LOD/page selection behavior
- `meridian_residency`: a CPU-side residency prototype for request/load/evict flow
- `meridian_vk_bootstrap`: a headless Vulkan bootstrap shell around the current upload contract
- `meridian_replay`: a deterministic multi-frame runtime-contract replay harness

Runtime foundation modules now include:

- `src/runtime_contract.h`: shared node/LOD/page runtime enums and constants
- `src/gpu_abi.h`: persistent GPU buffer layout spec in code
- `src/visibility_format.h`: visibility-buffer encoding spec in code
- `src/resource_upload.*`: CPU-to-GPU upload translation for `VGeoResource`
- `src/vk_bootstrap.mm`: standalone Vulkan bootstrap with macOS MoltenVK + GLFW window/device/swapchain bring-up
- `src/replay_script.*`: deterministic replay script loading for runtime validation

Builder modules now include:

- `src/builder_import.cpp`: manifest and source-asset ingestion
- `src/builder_cluster.cpp`: meshlet, hierarchy, LOD, and paging construction
- `src/builder_serialize.cpp`: `.vgeo` writing and summary IO
- `src/builder_validate.cpp`: manifest and resource validation
- `src/builder_traversal.cpp`: deterministic selection and prefetch logic
- `src/vgeo_builder.cpp`: public orchestration layer

Current live runtime milestone:

- uploads the current scene ABI into Vulkan buffers
- creates a swapchain on this Mac through MoltenVK
- renders a first debug view by drawing CPU-selected base and LOD cluster geometry
- uses a real 3D camera/view/projection path for the current debug geometry rendering
- reports replay-vs-runtime selection parity for the current debug rendering path
- performs selection and residency updates inside the live runtime frame loop
- uses persistent debug mesh buffers with per-frame selected draw submission
- includes a minimal visibility attachment scaffold in the Vulkan render pass
- reads back visibility IDs on the CPU and reports visible geometry counts
- compares visible geometry IDs against the submitted selection to validate visibility subset behavior

Parallel validation path now exists in stock Godot through the root `project.godot` benchmark runner and `benchmarks/scripts/run_godot_baseline.py`.

Current role:

- define the typed resource model in code
- serialize a first-pass `.vgeo` container
- load simple `.obj`, `.gltf`, and `.glb` geometry inputs with material-slot-driven section assignment
- support baseline glTF sparse POSITION/index accessors and unnamed-material order fallback
- support baseline `EXT_meshopt_compression` decode in glTF buffer views
- lock shared-position seams for baseline glTF material, normal, and UV discontinuities
- build first-pass meshlets and pages using `meshoptimizer`
- reorder clusters in hierarchy traversal order so node cluster ranges stay truthful
- build first-pass LOD groups and simplified clusters using `clusterlod`
- link exact-match hierarchy node spans to LOD group chains through provenance tracking
- page both base-cluster and LOD-cluster payload domains for runtime residency work
- prototype CPU-side traversal selection against node-linked LOD groups and page residency
- emit adjacent replacement-level page dependency hints for streaming prefetch
- preserve cross-material seam vertices during simplification in the current OBJ/material path
- serialize base-cluster payloads and LOD payloads as separate sections
- provide a stable place to begin the real mesh-processing pipeline

Example direct build command in the current environment:

```bash
c++ -std=c++20 -Wall -Wextra -pedantic \
  renderer/prototype/src/main.cpp \
  renderer/prototype/src/vgeo_builder.cpp \
  renderer/prototype/thirdparty/meshoptimizer/src/allocator.cpp \
  renderer/prototype/thirdparty/meshoptimizer/src/clusterizer.cpp \
  renderer/prototype/thirdparty/meshoptimizer/src/indexgenerator.cpp \
  renderer/prototype/thirdparty/meshoptimizer/src/meshletutils.cpp \
  renderer/prototype/thirdparty/meshoptimizer/src/partition.cpp \
  renderer/prototype/thirdparty/meshoptimizer/src/simplifier.cpp \
  renderer/prototype/thirdparty/meshoptimizer/src/spatialorder.cpp \
  -o renderer/prototype/build/meridian_builder
```

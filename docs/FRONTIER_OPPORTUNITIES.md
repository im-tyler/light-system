# Frontier Opportunities Beyond a Straight Nanite Clone

Last updated: 2026-03-23

## Summary

The current plan is not the maximum ambition available in 2026.

There are several newer developments that could let a Godot dense-geometry renderer do something **better than Nanite in specific areas**, but most of them are either:

- vendor-specific,
- experimental,
- not yet portable enough for a Godot core strategy, or
- valuable only after the baseline clustered renderer already exists.

So the correct answer is:

- **No, the current plan is not the absolute frontier.**
- **Yes, there are ways to surpass Nanite in some workloads.**
- **But the portable baseline should still be compute-first clustered rendering with streaming.**

## The Most Important Missing Idea

The real competition target should not be:

"Godot has a Nanite clone."

It should be:

"Godot has a dense-geometry platform with multiple geometry representations."

That means:

1. **Clustered static geometry** for photogrammetry, architecture, rocks, props.
2. **Procedural/generated geometry** for foliage, trees, shells, mesostructures, resurfaced materials.
3. **Compressed geometry formats** that work for both rasterization and ray tracing.

Nanite is strongest on #1.
Modern research opens strong opportunities in #2 and #3.

## Opportunity 1: Beat Nanite on Nature / Aggregate Geometry

### Why this matters

Epic's own Nanite docs still call out **aggregate geometry** such as:

- leaves
- grass
- hair
- hole-heavy layered geometry

as a case that breaks Nanite's ideal scaling due to overdraw and weak simplification behavior.

This is a real opening.

### Better strategy

Instead of forcing all foliage through one Nanite-like path:

- use clustered geometry for trunks, rocks, large solid assets
- use a specialized foliage path for leaves/needles/cards/alpha-heavy assets
- prefer geometry-aware or procedural foliage generation where possible

### Relevant new tech

- **Opacity Micromaps (OMM)** improve ray tracing of alpha-tested geometry substantially.
- **Procedural GPU tree generation** can replace large stored geometry with compact generation logic.
- **Procedural resurfacing** can generate geometric detail at runtime instead of storing it.

### Why this could beat Nanite

Nanite's weakness is trying to represent aggregate detail as ordinary hierarchical triangle clusters.

A Godot renderer could do better by being explicitly **hybrid**:

- solid surfaces -> virtualized clusters
- foliage / shell-like detail -> specialized procedural or opacity-aware path

## Opportunity 2: Geometry Compression Better Aligned With Ray Tracing

Nanite is excellent at rasterized dense geometry, but the broader industry in 2025-2026 is moving toward geometry representations that are friendlier to **both rasterization and ray tracing**.

### Key developments

#### AMD Dense Geometry Format (DGF)

AMD describes DGF as a block-based geometry compression technology designed for future hardware support and released:

- an open SDK
- Vulkan support
- an animation-aware encoding update in September 2025
- a provisional Vulkan extension for using compressed DGF directly in acceleration-structure builds

#### Parallel DGF topology decompression

Eurographics 2025 work shows a parallel decompression method that:

- reduces triangle access from sequential `O(T)` to `O(log T)`
- and for DGF-sized blocks allows effectively `O(1)` access

#### Real-time meshlet decompression

2025 work shows:

- practical meshlet compression
- mesh-shader decompression each frame
- index compression ratios up to `16:1` versus the vertex pipeline

### Why this matters

If Godot wants to be more future-proof than a straight Nanite clone, it should consider a geometry format roadmap where:

- raster and RT share the same compact source geometry
- geometry is not expanded permanently into large intermediate forms
- compression is a first-class part of the runtime architecture

### Why this could beat Nanite

Not necessarily on today's baseline raster pass, but on:

- memory footprint
- streaming behavior
- RT integration
- future hardware alignment

This is one of the strongest "beyond Nanite" directions.

## Opportunity 3: Runtime Procedural Resurfacing

Eurographics 2025 introduced **real-time procedural resurfacing using GPU mesh shaders**.

Core idea:

- store a simpler base control mesh
- generate detailed explicit geometry at render time
- use dynamic LOD and procedural control maps

The paper reports:

- reduced VRAM usage
- lower power consumption
- competitive performance
- and the ability to render significantly more primitives without being limited by GPU memory

### Best use cases

- rocks
- bark
- cliffs
- masonry
- chainmail / repeated mesostructure
- terrain-like surface enrichment

### Why this matters

For some content classes, storing all triangles explicitly may stop being the best answer.

Nanite is still fundamentally about efficient storage and selection of explicit triangles.
Procedural resurfacing changes the game by storing a lower-order surface plus generation logic.

### Why it is not the baseline

- depends on mesh shaders
- better for some asset classes than for general arbitrary scanned geometry
- raises authoring and tooling questions

### Recommendation

Treat this as a **Phase 5+ asset-class-specific accelerator**, not the core renderer.

## Opportunity 4: GPU Work Graphs / Mesh Nodes

Work graphs are one of the most important execution-model changes since Nanite shipped.

### What changed

DirectX work graphs are now official, and Microsoft highlighted further progress in 2025.
AMD also exposes work graphs and mesh nodes via experimental Vulkan extensions.

This matters because GPU work graphs allow:

- dynamic work expansion on GPU
- less CPU orchestration
- more natural fully GPU-driven pipelines
- recursive / procedural generation patterns that are awkward in classic dispatch chains

### Why this could surpass Nanite

Nanite is already highly GPU-driven, but work graphs make some classes of dynamic expansion and GPU-side scheduling cleaner and potentially cheaper.

Relevant examples:

- GPU-driven procedural generation
- dynamic subdivision / resurfacing
- adaptive culling trees
- multi-stage visibility scheduling without CPU round trips

### Why it is not the portable base

- DirectX work graphs are ahead of Vulkan standardization
- Vulkan support is still experimental and vendor-specific
- not exposed in stock Godot

### Recommendation

Design the renderer around a **logical GPU task graph**, but implement it first with conventional compute/indirect dispatch.
Then add work-graph backends later where available.

## Opportunity 5: A Better Hybrid Strategy for Ray Tracing

If the renderer is meant to matter long-term, it should not think only about rasterization.

### New signals from 2025-2026

- DGF is explicitly aimed at compressed geometry for RT hardware.
- NVIDIA's RTX Mega Geometry is targeting cluster-based geometry for path tracing.
- OMMs materially improve alpha-heavy RT scenes.
- DXR 1.2 adds OMM and SER, and Microsoft reports substantial gains in complex scenes.

### Important implication

The winning architecture may be:

- one dense geometry representation for raster culling and shadowing
- a related compressed representation for RT acceleration structures
- specialized handling for alpha-heavy geometry

### Why this is strategically important

Unreal's long-term rendering advantage is not just Nanite alone.
It is Nanite + modern shadowing + ray tracing + content workflows.

If Godot builds a dense-geometry system that is RT-aware from day one, that could age better than a raster-only Nanite imitation.

## Opportunity 6: Neural / ML-Assisted Compression

This area is real, but dangerous to over-rotate on.

### Current signal

In 2025-2026:

- NVIDIA pushed neural shaders and cooperative vectors
- neural texture compression and texture streaming matured further
- DirectX added cooperative vectors support for in-shader ML workflows

### Possible geometry relevance

- learned decompression
- learned material approximation
- learned visibility heuristics
- learned texture/material compression attached to dense geometry assets

### Why this is not the main geometry plan

- very vendor- and API-specific right now
- weak portability story for Godot core
- likely to complicate toolchains and testing

### Recommendation

Use this only for:

- optional texture/material compression experiments
- future research branch

Do not hinge the core geometry renderer on it.

## Opportunity 7: Treat Streaming as a Compression-and-I/O Stack, Not Just Paging

This was underemphasized in the earlier plan.

### New 2026 signal

Microsoft shipped a public preview of:

- **DirectStorage 1.4**
- **Zstandard support**
- **Game Asset Conditioning Library (GACL)**

The stated goal is smoother asset streaming with better compression ratios and lower runtime cost.

### Why this matters for dense geometry

Nanite-class rendering depends on:

- chunk sizing
- decompression cost
- paging policy
- GPU upload scheduling
- storage format design

If Godot's dense-geometry path uses a stronger content-conditioning pipeline, it could improve:

- install size
- patch size
- streaming latency
- CPU overhead during streaming

### Best interpretation

The renderer should be designed as:

- geometry representation
- streaming scheduler
- compression strategy
- asset-conditioning pipeline

not just "clusters plus culling."

### Recommendation

Plan for:

- page-sized geometry chunks
- async decode path
- pluggable compression backend
- per-platform packing strategy

from the beginning.

## What This Means for the Real Plan

## Portable core

Build this no matter what:

1. offline cluster hierarchy
2. GPU-driven compute culling
3. visibility buffer or equivalent
4. page streaming
5. shadow path
6. constrained opaque material support

## Frontier accelerators

Add later as optional paths:

1. mesh shader raster path
2. runtime procedural resurfacing for select assets
3. meshlet compression / decompression
4. DGF or similar compressed geometry format
5. RT-focused compressed-geometry path
6. work graphs / mesh nodes backend
7. stronger asset conditioning and streaming compression pipeline

## Where Godot Could Realistically Beat Nanite

### 1. Hybrid foliage and aggregate geometry

Because Nanite's own docs call this out as a weak area, a renderer that uses:

- procedural trees
- explicit solid-geometry virtualization
- better alpha-aware RT support

could outperform a one-representation-fits-all Nanite-style approach in vegetation-heavy scenes.

### 2. Storage efficiency for some asset classes

Procedural resurfacing and aggressive meshlet compression could beat Nanite's explicit-geometry approach on:

- VRAM use
- disk footprint
- streaming bandwidth

for the right assets.

### 3. RT integration of compressed dense geometry

DGF-like paths and RT-aware compressed geometry could become a cleaner long-term story than "Nanite for raster, something else for RT."

### 4. GPU-native procedural worlds

Work-graph-based generation plus rendering could outperform traditional imported-asset pipelines in some content classes such as:

- forests
- rocks
- repeated natural detail
- decorative mesostructure

## Where Nanite Is Still the Hard Benchmark

Nanite still sets the bar on:

- production maturity
- engine integration
- editor workflow
- general-purpose static opaque geometry
- shipping robustness across a wide content set

That is why the correct strategy is not to chase every frontier feature at once.

## Final Recommendation

The best overall strategy in 2026 is:

### Step 1

Build the portable clustered renderer.

### Step 2

Make it strong on the exact places Nanite is strongest:

- static opaque dense geometry
- streaming
- shadows
- import workflow

### Step 3

Then try to beat Nanite where it is weaker:

- aggregate geometry / foliage
- compressed geometry for RT
- runtime procedural detail generation

### Step 4

Use work graphs, mesh nodes, DGF, and neural techniques as **optional accelerators**, not as the foundation.

## Sources

- Nanite overview and supported/limited-content discussion:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- Nanite landscapes:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/using-nanite-with-landscapes-in-unreal-engine?application_version=5.7
- DirectX State of the Union 2025:
  https://developer.microsoft.com/en-us/games/articles/2025/03/gdc-2025-directx-state-of-the-union/
- DirectX ML era update (2026):
  https://devblogs.microsoft.com/directx/evolving-directx-for-the-ml-era-on-windows
- D3D12 work graphs:
  https://devblogs.microsoft.com/directx/d3d12-work-graphs/
- D3D12 mesh nodes in work graphs:
  https://devblogs.microsoft.com/directx/d3d12-mesh-nodes-in-work-graphs/
- AMD Vulkan work graphs / mesh nodes:
  https://gpuopen.com/learn/gpu-workgraphs-mesh-nodes-vulkan/
- AMD Dense Geometry Format SDK:
  https://gpuopen.com/dgf/
- AMD Dense Geometry Format Vulkan extension:
  https://gpuopen.com/learn/dense-geometry-format-amd-vulkan-extension/
- AMD dense geometry overview:
  https://gpuopen.com/learn/problem_increasing_triangle_density/
- AMD meshlet compression:
  https://gpuopen.com/learn/mesh_shaders/mesh_shaders-meshlet_compression/
- Parallel DGF topology decompression:
  https://diglib.eg.org/handle/10.2312/egs20251050
- Real-time meshlet decompression:
  https://doi.org/10.1016/j.cag.2025.104292
- Real-time procedural resurfacing using GPU mesh shader:
  https://doi.org/10.1111/cgf.70075
- NVIDIA RTX Mega Geometry:
  https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples/
- NVIDIA neural rendering / RTX Kit:
  https://developer.nvidia.com/blog/get-started-with-neural-rendering-using-nvidia-rtx-kit
- NVIDIA GDC 2025 neural rendering update:
  https://developer.nvidia.com/blog/nvidia-rtx-advances-with-neural-rendering-and-digital-human-technologies-at-gdc-2025/
- NVIDIA micro-mesh:
  https://developer.nvidia.com/rtx/ray-tracing/micro-mesh
- NVIDIA Opacity Micro-Maps:
  https://developer.nvidia.com/rtx/ray-tracing/opacity-micro-map/get-started
- Indiana Jones OMM performance note:
  https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass/
- SIGGRAPH 2024 Advances in Real-Time Rendering:
  https://advances.realtimerendering.com/s2024/
- SIGGRAPH 2025 Advances in Real-Time Rendering:
  https://advances.realtimerendering.com/s2025/

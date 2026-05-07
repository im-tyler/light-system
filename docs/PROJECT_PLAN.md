# Project Meridian

Last updated: 2026-03-23

## Mission

Build a dense-geometry platform for Godot that makes the engine highly competitive with Nanite for the content classes that matter most in production:

- static opaque world geometry
- photogrammetry
- scanned props
- dense architecture
- heavy instancing
- shadowed high-detail scenes

This is not a "Nanite clone" project. It is a **compute-first, streaming-first, hybrid dense-geometry renderer** with room for multiple geometry representations over time.

## Product Goal

### Tier 1: Competitive Enough

Godot can import dense static assets and render them with:

- automatic clustered LOD
- GPU-driven culling
- crack-free transitions
- bounded memory through streaming
- good shadow performance
- minimal artist-authored LOD work

### Tier 2: Broad Production Use

Add:

- strong foliage workflows
- terrain and landscape integration
- broader material coverage
- more mature streaming and residency
- better tooling and editor iteration

### Tier 3: Near-Nanite Parity

Add:

- deforming and skeletal geometry
- broader platform reach
- stronger RT integration
- production-grade workflows across more content classes

## Non-Goals for v1

Do not promise these in the first production target:

- full material parity with all Godot `ShaderMaterial` usage
- transparency-heavy geometry
- skeletal meshes
- VR
- split screen
- web
- mobile
- broad renderer parity across Vulkan, D3D12, and Metal

## Strategic Position

### What Godot must beat

The real baseline is not "plain meshes."
The project must beat or strongly justify itself against:

- stock Godot Forward+
- stock auto mesh LOD
- stock visibility ranges / HLOD
- stock occlusion culling

### Where Godot can realistically win

- high-detail static opaque content
- large instance counts
- dense shadowed scenes
- foliage and aggregate geometry via a hybrid strategy
- future compressed-geometry and procedural-detail workflows

## Core Decisions

### 1. Compute-first

The baseline renderer path will use:

- compute culling
- indirect execution
- visibility buffer or equivalent deferred geometry path

Mesh shaders are an optional acceleration path, not the foundation.

### 2. Streaming-first

Streaming is not polish.
The renderer must be designed around:

- page-sized geometry chunks
- async loading / decode
- bounded GPU memory
- residency scheduling

### 3. Hybrid dense-geometry platform

The system should ultimately support more than one representation:

- clustered explicit geometry for solid assets
- specialized foliage / aggregate geometry paths
- optional procedural resurfacing for selected asset classes
- compressed geometry paths aligned with future RT workflows

### 4. Delivery vehicle: likely hybrid

Use:

- GDExtension for importer, resources, editor tooling, debug UX, and experiments
- standalone Vulkan for renderer proof and profiling
- engine module or fork for the real integrated runtime renderer unless stock Godot exposes sufficient renderer ownership later

## Phase Plan

## Phase 0: Feasibility and Baseline

Duration:

- 4 to 6 weeks

Deliverables:

- stock Godot benchmark scenes and numbers
- renderer integration feasibility memo
- architecture decision on `GDExtension-only` vs `hybrid` vs `engine module`
- asset format sketch
- early importer spike

Exit criteria:

- architecture is frozen
- benchmark methodology is frozen

## Phase 1: Offline Asset Pipeline

Duration:

- 6 to 10 weeks

Build:

- meshlet generation
- hierarchical simplification
- crack-safe cluster boundaries
- cluster/page packing format
- custom resource format
- fallback mesh generation
- command-line builder and Godot importer integration

Exit criteria:

- dense assets import deterministically
- generated resources pass validation and can be inspected

## Phase 2: Standalone Dense-Geometry Renderer

Duration:

- 10 to 14 weeks

Build:

- Vulkan prototype
- compute-driven instance and cluster culling
- HZB occlusion
- visibility buffer
- constrained PBR material resolve
- shadow pass for virtualized geometry
- residency and page streaming

Exit criteria:

- large static scenes render interactively
- memory stays bounded under camera traversal

## Phase 3: Godot Runtime Integration

Duration:

- 8 to 16 weeks

Build:

- `VGeoMesh` resource
- runtime scene integration
- editor and debug visualization
- benchmark harness inside Godot
- shadow and material bridge for the supported v1 subset

Exit criteria:

- a Godot scene uses the runtime path end to end
- benchmark wins are measurable on target scenes

## Phase 4: Competitive Performance Pass

Duration:

- 8 to 12 weeks

Build:

- page scheduler tuning
- import speed improvements
- material and shadow optimization
- vendor-specific profiling and fixes
- content authoring guidance

Exit criteria:

- repeated wins against stock Forward+ on the benchmark suite

## Phase 5: Frontier Extensions

Build selectively:

- mesh shader acceleration path
- hybrid foliage / aggregate geometry path
- compressed geometry runtime formats
- procedural resurfacing for specific asset classes
- RT-aware dense geometry path

## Success Metrics

The project is successful when it can demonstrate all of the following on a public benchmark set:

1. Dense static scenes that are materially faster than stock Godot at similar quality.
2. No visible cracks or unacceptable LOD popping in supported content.
3. Bounded memory use under prolonged camera movement.
4. Practical import workflow for dense source assets.
5. Shadow behavior that remains usable under heavy geometry density.

## Primary Risks

1. Delivery vehicle mismatch

The largest strategic risk is spending too long on a pure GDExtension runtime path that cannot truly own the render pipeline.

2. Material scope explosion

The renderer can fail by trying to support too much material behavior too early.

3. Streaming deferred too late

A renderer that culls well but cannot stream well will not be competitive with Nanite in real scenes.

4. Shadow work under-scoped

Dense geometry without dense-geometry-aware shadow behavior will feel incomplete immediately.

5. Chasing frontier features before the portable core works

Work graphs, mesh nodes, compressed RT paths, and neural techniques are promising, but they must not replace the core roadmap.

## Immediate Work Order

1. Finalize architecture decisions.
2. Build the benchmark suite.
3. Build the offline cluster/page pipeline.
4. Build the standalone Vulkan renderer.
5. Integrate into Godot only after the runtime path is proven.

## Related Docs

- [COMPETITIVE_PLAN.md](/Users/tyler/Documents/renderer/COMPETITIVE_PLAN.md)
- [FRONTIER_OPPORTUNITIES.md](/Users/tyler/Documents/renderer/FRONTIER_OPPORTUNITIES.md)
- [TECHNICAL_SPEC.md](/Users/tyler/Documents/renderer/TECHNICAL_SPEC.md)
- [ARCHITECTURE_DECISIONS.md](/Users/tyler/Documents/renderer/ARCHITECTURE_DECISIONS.md)
- [BENCHMARK_PLAN.md](/Users/tyler/Documents/renderer/BENCHMARK_PLAN.md)
- [IMPLEMENTATION_BACKLOG.md](/Users/tyler/Documents/renderer/IMPLEMENTATION_BACKLOG.md)

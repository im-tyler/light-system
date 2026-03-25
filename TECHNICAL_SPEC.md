# Technical Specification

Last updated: 2026-03-23

## Scope

This specification defines the **portable core** of Project Meridian.

The core target is:

- static opaque geometry
- desktop Forward+ workflows
- Vulkan-first implementation
- compute-first execution model
- streamed clustered geometry

Optional frontier paths such as mesh shaders, work graphs, procedural resurfacing, and compressed RT-oriented geometry are explicitly out of the portable core unless noted otherwise.

## Supported v1 Content

- static meshes
- dense props and scanned assets
- dense architectural meshes
- large numbers of rigid instances
- opaque PBR materials in a constrained subset
- directional-light shadow casting

## Explicitly Unsupported in v1

- skeletal meshes
- morph targets
- general translucent materials
- broad arbitrary `ShaderMaterial` parity
- VR
- split screen
- mobile
- web

## System Overview

The system is split into four major parts:

1. **Offline builder**
   - converts source meshes into streamed clustered resources
2. **Runtime dense-geometry renderer**
   - culls, selects, streams, and renders clustered geometry
3. **Godot integration layer**
   - importer, resources, editor UX, scene bindings
4. **Benchmark and validation layer**
   - correctness checks and performance measurement

## Offline Pipeline

## 1. Input qualification

At import time, classify meshes into one of these buckets:

- standard mesh path
- dense rigid geometry path
- excluded path

Initial default rule:

- if triangle count is greater than a configurable threshold, consider dense-geometry conversion

The importer must also expose overrides:

- force dense-geometry conversion
- force standard rendering
- mark as foliage / aggregate candidate for future hybrid workflows

## 2. Cluster construction

Use `meshoptimizer` for the baseline cluster construction pipeline.

Initial target cluster sizes:

- 64 vertices
- 124 triangles

Store per cluster:

- local vertex/index payload
- bounding sphere
- normal cone or equivalent directional culling data
- local error metric
- parent/child relationships
- page residency metadata

## 3. Hierarchical simplification

Build a crack-safe hierarchy using grouped simplification and locked borders.

Each hierarchy node must have:

- geometric error
- bounds
- parent reference
- children range
- page membership

The hierarchy must support:

- top-down traversal
- crack-safe adjacent cluster transitions within an object
- screen-space error selection

## 4. Page packing

The resource format is page-based.

Pages should group:

- cluster metadata
- vertex/index payloads
- optional compressed blocks

Page sizing must balance:

- streaming granularity
- decode/upload overhead
- locality for neighboring visible clusters

## 5. Serialization

The custom resource format should include:

- file header and version
- object bounds
- hierarchy root range
- page table
- cluster metadata buffer
- geometry payload section
- material section or external material mapping
- fallback mesh metadata

The format must support future extension for:

- compressed geometry payloads
- alternative geometry representations
- RT-specific metadata

## Runtime Data Model

## 1. CPU-side persistent state

Per resource:

- hierarchy metadata
- page table
- import settings
- fallback mesh
- material mapping

Per instance:

- transform
- material overrides
- visibility state
- streaming priority hints

## 2. GPU-side persistent state

- resource metadata buffer
- cluster metadata buffer
- page residency table
- streamed geometry payload buffers
- material parameter buffers
- visibility and indirect execution buffers

Current prototype code now freezes:

- a persistent GPU ABI for instances, nodes, clusters, lod groups, lod clusters, pages, and page dependencies
- a two-word visibility pixel format using `instance_index` plus `{valid, geometry_kind, geometry_index, local_triangle}`

## 3. Transient per-frame state

- visible instance lists
- visible cluster lists
- HZB or occlusion structures
- visibility buffer
- shadow visibility lists
- resolve outputs

## Runtime Pipeline

## 1. CPU frame orchestration

CPU responsibilities should be limited to:

- camera and scene update upload
- streaming scheduler decisions
- fallback handling
- pass dispatch orchestration

The CPU must not rebuild draw work at per-triangle granularity.

## 2. Pass sequence

### Pass A: instance culling

Compute:

- frustum cull instances
- coarse occlusion if available
- append visible instances

### Pass B: hierarchy traversal and cluster LOD selection

Compute:

- traverse cluster hierarchy
- prefer exact-match node-linked LOD groups before descending to finer base clusters
- test screen-space error
- frustum cull clusters
- mark missing pages
- emit renderable clusters

Current builder contract:

- hierarchy nodes remain the primary traversal unit
- node-linked LOD groups are optional substitutions for exact base-cluster spans
- base clusters and LOD clusters live in separate payload domains
- both payload domains are page-addressable

### Pass C: occlusion refinement

Compute against prior-frame or in-frame HZB:

- reject occluded clusters
- compact visible cluster list

### Pass D: geometry pass

Portable baseline:

- compute-assisted indirect path that writes a visibility buffer or equivalent deferred geometry representation

Optional later path:

- task/mesh shader acceleration

### Pass E: HZB build

Construct or update the hierarchical Z representation for subsequent culling.

### Pass F: material resolve

For each visible pixel:

- decode instance and cluster identity
- fetch triangle and vertex data
- reconstruct interpolants
- evaluate constrained opaque PBR material inputs
- write lighting-ready outputs

### Pass G: shadow pass

Directional shadow rendering for dense geometry using the same hierarchy and culling logic from the light's point of view.

## Visibility Representation

The baseline implementation should use a visibility buffer or equivalent minimal geometry ID representation.

Requirements:

- compact encoding for instance and cluster identity
- enough local primitive information to reconstruct attributes
- compatibility with a separate depth buffer

The exact bit layout can evolve, but the representation must be stable enough for:

- debug visualization
- material resolve
- future shadow and RT interoperability

## Streaming and Residency

## 1. Design principles

Streaming is a first-class system.

The scheduler must:

- prioritize visible and near-visible pages
- keep memory within a configurable budget
- avoid frame-stalling synchronous loads
- tolerate rapid camera movement

## 2. Residency states

Each page must have a state such as:

- unloaded
- requested
- loading
- resident
- eviction candidate

## 3. Scheduler inputs

- current visible hierarchy cuts
- near-future camera prediction
- shadow visibility demands
- memory pressure
- page fault history

Current page model:

- base pages cover contiguous base-cluster payload ranges
- LOD pages cover contiguous LOD-cluster payload ranges
- page kind is explicit so residency can reason about the correct payload domain
- page dependency links are soft adjacent-replacement prefetch hints, not hard prerequisites

Current prototype scheduler contract:

- traversal can fall back to resident base spans when a preferred linked LOD representation is unavailable
- missing pages identify immediate faults for the chosen representation
- prefetch pages identify adjacent replacement levels worth requesting next
- deterministic replay scripts can drive repeatable multi-frame validation of this contract before GPU execution exists

Current runtime milestone:

- macOS MoltenVK bootstrap can now create the Vulkan instance, GLFW window, surface, logical device, queues, and swapchain
- scene metadata and payload buffers now upload into the live Vulkan runtime
- first visible rendering now exists as a debug geometry path driven by CPU-selected base and LOD clusters
- the debug renderer now uses a real 3D camera/view/projection transform instead of flattened 2D projection
- the debug renderer now reports whether rendered base/LOD cluster selection matches replay selection
- the runtime loop now performs live selection and residency updates before building the debug draw for each frame
- the runtime now draws from persistent debug mesh buffers and includes a minimal visibility attachment scaffold in the render pass
- the runtime now reads back visibility IDs and reports visible geometry counts to the CPU for validation
- the runtime now compares visible IDs against submitted selected geometry to validate visibility subset behavior

Current build-system milestone:

- builder responsibilities are now split across import, cluster build, serialization, validation, and traversal modules
- public build orchestration remains behind the existing `vgeo_builder` API surface

## 4. Compression

The baseline format should be compatible with optional compressed payloads.

Do not hard-code one codec too early, but the format and page abstraction should allow:

- pluggable CPU-side decompression
- future GPU-side decompression
- per-platform packing choices

## Material Strategy

## v1 supported material subset

- base color
- normal
- roughness
- metallic
- ambient occlusion
- emissive

Alpha test is optional and should be treated carefully due to overdraw and future aggregate-geometry handling.

## v1 material exclusions

- general translucent workflows
- arbitrary custom shading graphs
- material features that require unsupported derivative behavior without a dedicated solution

## Shadow Strategy

Shadows are part of the core renderer, not an afterthought.

v1 requirements:

- dense geometry casts directional shadows
- shadow path shares hierarchy logic and streaming model
- shadow quality remains stable under camera movement and dense scenes

Future considerations:

- denser shadow residency models
- virtual-shadow-map-like strategies
- RT-aware shadow integration

## Godot Integration Architecture

## 1. GDExtension responsibilities

Use GDExtension for:

- importer hooks
- resource types
- editor tools
- benchmark controls
- debug visualization and developer UX

## 2. Runtime ownership assumption

The project should assume the final performant runtime path likely requires:

- engine module integration, or
- new upstream renderer extension points

Do not assume current public compositor hooks alone are sufficient for full production integration.

## 3. Scene objects

Planned resource and node concepts:

- `VGeoMesh`
- `VGeoMeshInstance3D`
- benchmark scene helpers
- debug visualization controls

## Validation and Debugging

## 1. Correctness validation

- cluster hierarchy validation at build time
- crack checks at adjacent LOD boundaries
- CPU/GPU visibility comparison on test scenes
- residency correctness under camera sweeps

## 2. Debug views

- cluster IDs
- hierarchy level
- page residency
- occlusion rejection
- screen-space error heat map
- raw visibility buffer
- shadow coverage

## Benchmarking Requirements

All major milestones must be evaluated against:

- stock Godot Forward+
- stock mesh LOD
- stock HLOD / visibility ranges
- stock occlusion culling

Use the benchmark plan in [BENCHMARK_PLAN.md](/Users/tyler/Documents/renderer/BENCHMARK_PLAN.md).

## Frontier Extensions

These are explicitly outside the portable core, but the architecture should leave room for them:

- mesh shader acceleration
- hybrid foliage representation
- procedural resurfacing
- compressed geometry runtime formats
- RT-aware compressed geometry
- work graph backends

## Related Docs

- [PROJECT_PLAN.md](/Users/tyler/Documents/renderer/PROJECT_PLAN.md)
- [COMPETITIVE_PLAN.md](/Users/tyler/Documents/renderer/COMPETITIVE_PLAN.md)
- [FRONTIER_OPPORTUNITIES.md](/Users/tyler/Documents/renderer/FRONTIER_OPPORTUNITIES.md)
- [ARCHITECTURE_DECISIONS.md](/Users/tyler/Documents/renderer/ARCHITECTURE_DECISIONS.md)

# Godot vs. Nanite: Research-Backed Competitive Plan

Last updated: 2026-03-23

## Executive Summary

If the goal is for Godot to **compete with Nanite in real game production**, the target should not be "copy Nanite feature-for-feature inside a pure GDExtension."

The realistic target is:

1. Make Godot highly competitive for **static opaque high-detail geometry** on desktop.
2. Deliver this first through a **compute-first GPU-driven renderer path**.
3. Treat **mesh shaders as an acceleration path**, not the foundation.
4. Treat **GDExtension as suitable for tooling, import, editor UX, and early experiments**, but expect the final renderer to require either:
   - a Godot engine module / fork, or
   - new upstream renderer extension points beyond what stable Godot exposes today.

This is still worth doing. The gap is real, but it is narrower and more specific than "Unreal has Nanite, Godot does not."

## What "Compete With Nanite" Should Mean

For planning, define three tiers:

### Tier 1: Competitive Enough

Godot can import photogrammetry, ZBrush-like props, dense architecture, and rock sets with:

- automatic clustered LOD
- high instance counts
- crack-free transitions
- good shadow performance
- little or no artist-authored LOD work

This is the first real target. It is hard, but plausible.

### Tier 2: Broad Production Use

Add:

- dense foliage workflows
- landscapes / terrain integration
- better shadowing for dense geometry
- streaming and residency management for large worlds
- solid material coverage for common opaque workflows

This is where the renderer becomes strategically important for Godot.

### Tier 3: Near-Nanite Parity

Add:

- deformed / skeletal support
- broader material edge cases
- editor rebuild workflows as smooth as Unreal
- wide platform coverage with strong fallbacks

This is a long-term goal, not a v1 target.

## Current Reality on 2026-03-23

### 1. Godot already has some of the "old world" mitigations

Godot stable already supports:

- automatic mesh LOD generation
- visibility ranges / HLOD
- CPU occlusion culling
- Forward+ on RenderingDevice backends

This matters because the new system must beat a better baseline than "plain meshes with no LOD."

### 2. Godot's current extension surface is not the same as full renderer ownership

Stable Godot exposes:

- `Compositor` / `CompositorEffect`
- `RenderDataRD`
- `RenderSceneBuffersRD`
- `RenderingDevice` access in Forward+ and Mobile

But the stable docs still describe the compositor as experimental, and the public hooks are framed as a way to insert **additional passes**, not replace the engine's opaque geometry pipeline end to end.

### 3. Mesh shaders are not yet a stock Godot RenderingDevice capability

As of the stable docs checked on 2026-03-23, `RDShaderSource` exposes:

- `source_vertex`
- `source_fragment`
- `source_tesselation_control`
- `source_tesselation_evaluation`
- `source_compute`

There is still no documented mesh/task shader stage in stable `RenderingDevice`.

Godot PR `#88934` for mesh shader support is still open.

### 4. Godot's own proposal history points to the same missing pieces

Open proposals still exist for:

- mesh shading in RenderingDevice
- virtualized geometry
- mesh streaming
- custom rendering backends via GDExtension

Taken together, this strongly suggests the same conclusion: the feature is desirable, but the required renderer surface is not fully there yet in stock Godot.

### 5. Nanite itself is not "everything, everywhere"

Current Unreal docs still frame Nanite around:

- virtualized geometry
- automatic clustered LOD
- fine-grained streaming
- its own rendering path

Nanite now supports much more than it did at UE5 launch, including landscapes and skeletal mesh paths, but it still has explicit supported/unsupported feature boundaries. That is useful because it means Godot does not need total parity on day one to be competitive.

## Strategic Conclusion

The project should be split into **two tracks**:

### Track A: Practical Product Goal

Build a renderer that makes Godot competitive for:

- static opaque meshes
- photogrammetry
- scanned props
- dense architecture
- large instance counts
- desktop Forward+ workflows

This is the product that matters.

### Track B: Long-Term Engine Goal

Work toward:

- upstream renderer hooks
- mesh shader support in RenderingDevice
- proper engine integration

This is the route to a durable community feature, but it should not block Track A.

## Architecture Decision

## Recommended final architecture

### Use GDExtension for:

- importer pipeline
- asset preprocessing
- resource types
- editor tooling
- debug tools
- benchmark harness

### Use standalone Vulkan for:

- proving the core renderer
- profiling culling / HZB / resolve / streaming
- vendor testing

### Expect an engine module or fork for the real renderer

Reason:

- full opaque-pass ownership
- shadow integration
- lighting integration
- material bridge
- long-term performance work

are much more credible in engine space than via today's public compositor hooks alone.

If stock Godot gains the missing renderer surfaces later, the module can be reduced or upstreamed.

## Recommended Technical Scope

## v1 scope

Ship only:

- static meshes
- opaque materials only
- desktop only
- Forward+ only
- Vulkan first
- clustered LOD
- GPU-driven culling
- crack-free transitions
- shadow casting
- streaming for geometry pages
- importer that is largely automatic

Do not promise in v1:

- transparency
- general `ShaderMaterial` compatibility
- skinned meshes
- VR
- split screen
- mobile
- web
- perfect parity across Vulkan, D3D12, and Metal

## Why this scope is correct

Nanite's value is mostly won on:

- opaque world geometry
- shadows for dense geometry
- instance-heavy environments
- artist workflow reduction

That is the part worth chasing first.

## What the Renderer Must Actually Have

To compete in practice, the system needs more than meshlets.

### Required pillars

1. **Offline cluster hierarchy**
   - meshlet generation
   - grouped simplification
   - crack prevention
   - per-cluster bounds and error

2. **GPU-driven visibility**
   - instance culling
   - cluster culling
   - frustum culling
   - occlusion culling
   - LOD cut selection on GPU

3. **Visibility-buffer or equivalent deferred geometry path**
   - one pass for visibility
   - deferred resolve for material data
   - efficient shadow path for the same geometry representation

4. **Streaming / residency**
   - page-based geometry storage
   - demand loading
   - bounded GPU memory

5. **Shadow integration**
   - dense geometry must cast usable shadows
   - this is not optional if the goal is to feel competitive with Nanite in real scenes

6. **Importer workflow**
   - artists import high-poly assets
   - clustered representation is built automatically
   - fallback mesh path exists for unsupported platforms

### Optional later accelerators

- task/mesh shader path
- meshlet compression improvements
- procedural resurfacing for select asset classes
- per-vendor tuning

## Compute-First vs Mesh-Shader-First

Choose **compute-first**.

### Why

- stable Godot does not yet document mesh/task shaders in RenderingDevice
- compute fallback is mandatory anyway
- open-source references like `lighthugger` prove the approach
- compute-first is easier to validate and integrate incrementally

### Mesh shaders should still be in the roadmap

But as:

- a Vulkan acceleration path
- a later optimization layer
- not the critical-path dependency for the project existing at all

## Material Strategy

Do not try to support all Godot materials in v1.

### v1 material target

Support a constrained opaque PBR subset:

- base color
- normal
- roughness
- metallic
- AO
- emissive
- alpha cutout only if it proves acceptable

### Avoid in v1

- arbitrary `ShaderMaterial`
- translucency
- refraction
- parallax-heavy custom code
- exotic per-material render features

Reason:

The material bridge can become bigger than the geometry project if left unconstrained.

## Shadow Strategy

Dense geometry without workable shadows will not feel competitive.

The plan must explicitly include a shadow path from the start.

### v1 shadow target

- support directional light shadows for virtualized geometry
- use the same cluster hierarchy and culling logic from the light's point of view
- keep compatibility with Godot's existing lighting model where feasible

### Longer-term shadow target

- investigate virtual-shadow-map-like behavior or equivalent sparse shadow residency
- separate static and dynamic shadow work where possible

## Streaming Strategy

Do not leave streaming for the polish phase.

If the goal is "Nanite-class usefulness," geometry streaming is foundational.

### v1 streaming target

- page the cluster hierarchy and vertex/index payloads
- keep a bounded GPU memory budget
- prioritize visible and near-visible pages
- support async CPU decode / upload

### Why early

Without streaming, the project risks becoming:

"GPU-driven clustered LOD for medium scenes"

which is valuable, but not the same category as Nanite.

## Recommended Revised Phases

## Phase 0: Feasibility Gates

Duration:

- 4 to 6 weeks

Deliverables:

- written benchmark baseline for stock Godot
- verified renderer hook map
- decision memo: `GDExtension-only`, `hybrid`, or `engine-module`
- validated import pipeline prototype

Questions to answer:

1. Can stock GDExtension own enough of the opaque and shadow passes to matter?
2. Can material resolve be integrated without reimplementing half the renderer?
3. Is the final delivery target a plugin, engine module, or fork?

Exit criterion:

- architecture decision is frozen

## Phase 1: Offline Asset Pipeline

Duration:

- 6 to 10 weeks

Build:

- cluster builder using meshoptimizer
- hierarchy construction
- error metric generation
- page packing format
- importer that emits a custom resource plus fallback mesh

Exit criterion:

- dense assets import automatically and produce stable clustered resources

## Phase 2: Standalone Renderer Prototype

Duration:

- 10 to 14 weeks

Build:

- Vulkan prototype
- compute-driven instance + cluster culling
- visibility buffer
- HZB
- material resolve for constrained PBR set
- shadow path
- page streaming

Exit criterion:

- large static scenes render interactively with bounded memory

## Phase 3: Godot Runtime Integration

Duration:

- 8 to 16 weeks

Build:

- `VGeoMesh` resource
- `VGeoMeshInstance3D` or equivalent runtime binding
- editor/debug views
- scene integration
- benchmarks against stock Forward+

Exit criterion:

- Godot scene can use the new renderer path on real content

## Phase 4: Competitive Performance Pass

Duration:

- 8 to 12 weeks

Build:

- vendor tuning
- page scheduler improvements
- shadow optimization
- instancing optimization
- import speed improvements
- profiler integration

Exit criterion:

- repeated benchmark wins on target scenes

## Phase 5: Expansion

Targets:

- mesh shader acceleration path
- landscapes
- foliage-specific handling
- broader material coverage
- eventual deformable support

## Benchmark Plan

The project needs benchmark scenes from day one.

### Compare against:

- stock Godot Forward+ with auto LOD
- stock Godot with visibility ranges / HLOD
- stock Godot occlusion culling
- Unreal Nanite on equivalent scenes when legally and practically testable

### Benchmark scenes

1. Photogrammetry canyon / ruins
2. Dense architecture block / street
3. Rock field with heavy instancing
4. Indoor occlusion-heavy scene
5. Large terrain-adjacent scene with props

### Metrics

- frame time
- GPU time by pass
- triangle/cluster counts submitted and visible
- residency / memory use
- import time
- build time
- shadow cost
- image quality regressions

## Staffing Estimate

### Solo engineer

Possible, but expect:

- 18 to 30 months for something seriously competitive

### Two strong rendering engineers

More realistic for:

- 9 to 18 months to reach a convincing Tier 1 / early Tier 2 result

### Needed skill mix

- rendering architecture
- Vulkan / GPU profiling
- Godot engine internals
- asset pipeline / import tooling

## Biggest Risks

1. **Wrong delivery vehicle**
   - Spending months on a GDExtension path that cannot truly own the renderer.

2. **Material scope explosion**
   - Trying to support every Godot material path too early.

3. **Streaming deferred too long**
   - Producing a cool demo that does not scale to real content.

4. **Shadow path under-scoped**
   - High-detail geometry without competitive shadow behavior will disappoint immediately.

5. **Over-indexing on mesh shaders**
   - Useful, but not the first blocker.

## Recommended Immediate Next Steps

1. Rewrite the current project docs around a **compute-first, engine-module-likely** strategy.
2. Add a formal **architecture decision record** for `GDExtension vs module`.
3. Start a **baseline benchmark suite** in Godot before writing renderer code.
4. Build the **offline cluster + page packer first** so it survives any runtime-path change.
5. Prototype the **standalone Vulkan renderer** before committing to Godot runtime integration details.

## Opinion

This is a real project, not a fantasy project. But it only stays real if the target is narrowed:

- beat stock Godot badly on dense static geometry
- become competitive with Nanite in the scenes where Nanite matters most
- accept that full parity is a multi-year engine effort

If the team stays disciplined on scope, this could become one of the most important rendering projects in the Godot ecosystem.

## Sources

- Godot mesh LOD docs: https://docs.godotengine.org/en/stable/tutorials/3d/mesh_lod.html
- Godot visibility ranges docs: https://docs.godotengine.org/en/stable/tutorials/3d/visibility_ranges.html
- Godot internal rendering architecture: https://docs.godotengine.org/en/stable/contributing/development/core_and_modules/internal_rendering_architecture.html
- Godot Compositor docs: https://docs.godotengine.org/en/stable/classes/class_compositor.html
- Godot Compositor tutorial: https://docs.godotengine.org/en/stable/tutorials/rendering/compositor.html
- Godot CompositorEffect docs: https://docs.godotengine.org/en/stable/classes/class_compositoreffect.html
- Godot RenderDataRD docs: https://docs.godotengine.org/en/stable/classes/class_renderdatard.html
- Godot RenderSceneBuffersRD docs: https://docs.godotengine.org/en/4.4/classes/class_renderscenebuffersrd.html
- Godot RDShaderSource docs: https://docs.godotengine.org/en/stable/classes/class_rdshadersource.html
- Godot proposal: virtualized geometry #2793: https://github.com/godotengine/godot-proposals/issues/2793
- Godot proposal: mesh shading #6822: https://github.com/godotengine/godot-proposals/issues/6822
- Godot proposal: mesh streaming #6109: https://github.com/godotengine/godot-proposals/issues/6109
- Godot proposal: custom rendering backends via GDExtension #4287: https://github.com/godotengine/godot-proposals/issues/4287
- Godot PR: mesh shader support #88934: https://github.com/godotengine/godot/pull/88934
- Unreal Engine Nanite overview: https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- Unreal Engine Nanite landscapes: https://dev.epicgames.com/documentation/en-us/unreal-engine/using-nanite-with-landscapes-in-unreal-engine?application_version=5.7
- meshoptimizer repository: https://github.com/zeux/meshoptimizer
- meshoptimizer site: https://meshoptimizer.org/
- meshoptimizer Nanite-like overview: https://deepwiki.com/zeux/meshoptimizer/3.2-nanite-like-lod-system
- Khronos mesh shading for Vulkan: https://www.khronos.org/blog/mesh-shading-for-vulkan
- VK_EXT_mesh_shader proposal: https://github.khronos.org/Vulkan-Site/features/latest/features/proposals/VK_EXT_mesh_shader.html
- AMD mesh shader best practices: https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
- AMD mesh shader overview: https://gpuopen.com/learn/mesh_shaders/mesh_shaders-index/
- Wicked Engine repository: https://github.com/turanszkij/WickedEngine

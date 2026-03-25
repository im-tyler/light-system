# Phase 0 Checklist

Last updated: 2026-03-23

Phase 0 exists to stop the project from committing to the wrong architecture.

## Deliverables

- benchmark scene list
- benchmark result template
- stock Godot baseline numbers
- renderer integration feasibility memo
- delivery vehicle decision
- initial asset format sketch

## Checklist

### Benchmarks

- [ ] define benchmark hardware profiles
- [ ] choose at least three benchmark scenes
- [ ] record stock Forward+ numbers
- [ ] record stock auto mesh LOD numbers
- [ ] record stock HLOD / visibility-range numbers where relevant
- [ ] record stock occlusion-culling numbers where relevant

### Renderer feasibility

- [ ] map current Godot renderer extension points (CompositorEffect, RenderingDevice, RenderDataRD, RenderSceneBuffersRD)
- [ ] identify what can live in GDExtension safely
- [ ] identify what likely requires engine-module ownership
- [ ] document blockers for pure extension-only runtime integration

### Critical: CompositorEffect depth integration test

- [ ] create a CompositorEffect that runs BEFORE the opaque pass
- [ ] have it write geometry to the depth buffer via RenderingDevice compute or graphics
- [ ] verify that Godot's Forward+ opaque pass respects this depth (skips occluded pixels)
- [ ] if depth integration works: dual-render approach is validated — no fork needed
- [ ] if depth integration fails: document the specific failure and define the minimal engine patch needed
- [ ] test with both simple (single mesh) and complex (1000+ instances) scenarios

### Lighthugger study

- [ ] clone and build Lighthugger
- [ ] study compute culling pipeline (instance cull → meshlet cull → visibility buffer write)
- [ ] study visibility buffer encoding (meshlet ID + triangle ID)
- [ ] study single-pass material resolve
- [ ] study cascaded shadow map integration
- [ ] document architectural patterns to adopt for Meridian

### Offline pipeline

- [ ] inspect `meshoptimizer` `demo/nanite.cpp`
- [ ] inspect `clusterlod.h`
- [ ] define first-pass cluster metadata schema
- [ ] define first-pass page schema

### Build and workflow

- [ ] choose initial project structure
- [ ] define benchmark artifact storage layout
- [ ] define import/build output naming and versioning

## Phase 0 Exit Gate

Phase 0 is complete only when:

1. the benchmark methodology is frozen
2. the CompositorEffect depth integration test has a clear result (dual-render works or minimal patch needed)
3. Lighthugger architecture is studied and patterns documented
4. the offline data model is sketched well enough to begin implementation
5. the shared buffer registry design is agreed with Aurora and Cascade

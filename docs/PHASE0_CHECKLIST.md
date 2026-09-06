# Phase 0 Checklist

Last updated: 2026-09-05

Phase 0 exists to stop the project from committing to the wrong architecture.

Historical note (2026-09-05): this checklist sat unchecked while the project
moved ahead. The statuses below reflect reality as of this date. Phase 0's
shape was superseded by events: the standalone Vulkan prototype (Phase 2)
became the primary body of work and the Godot runtime path was never started.
The umbrella peers referenced in the exit gate (Aurora, Cascade) were retired
with the godot-parity archive.

## Deliverables

- benchmark scene list
- benchmark result template
- stock Godot baseline numbers
- renderer integration feasibility memo
- delivery vehicle decision
- initial asset format sketch

## Checklist

### Benchmarks

- [x] choose at least three benchmark scenes (terrace, arch block, glTF block + uv-seam; frozen baselines 2026-03-25)
- [x] record stock Forward+ numbers (3 scenes, FROZEN_SYNTHETIC_GODOT_BASELINE.md; dragon/city baselines still missing)
- [ ] define benchmark hardware profiles (HARDWARE_PROFILES.md remains a template; all numbers to date are one Apple M4)
- [ ] record stock auto mesh LOD numbers
- [ ] record stock HLOD / visibility-range numbers where relevant
- [ ] record stock occlusion-culling numbers where relevant

### Renderer feasibility

- [x] map current Godot renderer extension points (source-reading pass, see PHASE0-DEPTH-GATE.md)
- [x] identify what can live in GDExtension safely (memo: notes/RUNTIME_DELIVERY_FEASIBILITY.md)
- [x] identify what likely requires engine-module ownership (same memo, Option B)
- [x] document blockers for pure extension-only runtime integration (ADR-009 fallback clause)

### Critical: CompositorEffect depth integration test

- [x] resolve the depth-write question on paper (2026-09-04: Forward+ pre-opaque depth survives; PHASE0-DEPTH-GATE.md, source-verified against Godot)
- [ ] create a CompositorEffect that runs BEFORE the opaque pass
- [ ] have it write geometry to the depth buffer via RenderingDevice compute or graphics
- [ ] verify that Godot's Forward+ opaque pass respects this depth (skips occluded pixels)
- [ ] test with both simple (single mesh) and complex (1000+ instances) scenarios

### Lighthugger study

- [x] study compute culling pipeline (instance cull → meshlet cull → visibility buffer write) — reflected in the prototype's pass structure
- [x] study visibility buffer encoding (instance + kind + index + triangle; visibility_format.h)
- [x] study cascaded shadow map integration (3-cascade CSM with per-cascade culling shipped)
- [ ] study single-pass material resolve (unbuilt — no resolve pass exists yet)
- [x] clone and build Lighthugger / document architectural patterns — absorbed directly into the prototype rather than as a written study report

### Offline pipeline

- [x] inspect `meshoptimizer` `demo/nanite.cpp`
- [x] inspect `clusterlod.h` (now vendored at a pinned commit; provenance contract documented)
- [x] define first-pass cluster metadata schema (schemas/VGEO_RESOURCE_SCHEMA.md, v3)
- [x] define first-pass page schema (schemas/PAGE_LAYOUT_SCHEMA.md)

### Build and workflow

- [x] choose initial project structure
- [x] define benchmark artifact storage layout
- [x] define import/build output naming and versioning
- [x] CI (2026-09-05: GitHub Actions builds all targets and smoke-runs builder/dump/trace/replay)

## Phase 0 Exit Gate

Status: **closed as superseded, not passed.**

1. benchmark methodology — frozen for the synthetic set; dragon/city comparison automation still open
2. CompositorEffect depth gate — resolved on paper; smoke test project still open and is the entry point for any future Godot runtime work
3. Lighthugger study — absorbed into implementation
4. offline data model — implemented and validated (builder complete, schema v3)
5. shared buffer registry with Aurora and Cascade — obsolete (umbrella retired 2026-09-04)

The project's live plan now lives in docs/TASK_LIST.md and
docs/IMPLEMENTATION_STATUS.md. The Godot runtime path remains parked behind
the PHASE0-DEPTH-GATE.md smoke test.

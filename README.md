# Renderer Workspace

Last updated: 2026-03-23

This directory contains the planning and implementation workspace for Project Meridian.

## Layout

- [PROJECT_PLAN.md](/Users/tyler/Documents/renderer/PROJECT_PLAN.md): project roadmap
- [TECHNICAL_SPEC.md](/Users/tyler/Documents/renderer/TECHNICAL_SPEC.md): portable-core renderer specification
- [ARCHITECTURE_DECISIONS.md](/Users/tyler/Documents/renderer/ARCHITECTURE_DECISIONS.md): ADRs
- [BENCHMARK_PLAN.md](/Users/tyler/Documents/renderer/BENCHMARK_PLAN.md): benchmark methodology
- [IMPLEMENTATION_BACKLOG.md](/Users/tyler/Documents/renderer/IMPLEMENTATION_BACKLOG.md): prioritized work queue
- [IMPLEMENTATION_STATUS.md](/Users/tyler/Documents/renderer/IMPLEMENTATION_STATUS.md): what is actually built right now
- [TASK_LIST.md](/Users/tyler/Documents/renderer/TASK_LIST.md): current execution checklist
- [PHASE0_CHECKLIST.md](/Users/tyler/Documents/renderer/PHASE0_CHECKLIST.md): feasibility gate checklist

## Working Directories

- [prototype](/Users/tyler/Documents/renderer/prototype): standalone Vulkan renderer
- [godot-vgeo](/Users/tyler/Documents/renderer/godot-vgeo): Godot-side importer and runtime integration
- [benchmarks](/Users/tyler/Documents/renderer/benchmarks): scenes, scripts, captures, and results
- [schemas](/Users/tyler/Documents/renderer/schemas): first-pass data and file-format schemas
- [tools](/Users/tyler/Documents/renderer/tools): builder and utility scripts
- [notes](/Users/tyler/Documents/renderer/notes): status notes and research findings

## First Execution Order

1. freeze Phase 0 benchmark scenes and result format
2. validate the delivery vehicle decision
3. finalize the first-pass `VGeo` resource and page schema
4. implement the offline builder
5. implement the standalone runtime

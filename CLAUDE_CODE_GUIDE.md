# Session Guide

Last updated: 2026-03-23

This file describes how to work on Project Meridian in focused implementation sessions.

## Start Every Session With

State:

1. current phase
2. exact deliverable for the session
3. which docs are authoritative
4. which benchmark or validation target will prove success

## Authoritative Docs

Read these first:

- [PROJECT_PLAN.md](/Users/tyler/Documents/renderer/PROJECT_PLAN.md)
- [TECHNICAL_SPEC.md](/Users/tyler/Documents/renderer/TECHNICAL_SPEC.md)
- [ARCHITECTURE_DECISIONS.md](/Users/tyler/Documents/renderer/ARCHITECTURE_DECISIONS.md)
- [BENCHMARK_PLAN.md](/Users/tyler/Documents/renderer/BENCHMARK_PLAN.md)
- [IMPLEMENTATION_BACKLOG.md](/Users/tyler/Documents/renderer/IMPLEMENTATION_BACKLOG.md)

Use these as supporting research:

- [COMPETITIVE_PLAN.md](/Users/tyler/Documents/renderer/COMPETITIVE_PLAN.md)
- [FRONTIER_OPPORTUNITIES.md](/Users/tyler/Documents/renderer/FRONTIER_OPPORTUNITIES.md)
- [READING_LIST.md](/Users/tyler/Documents/renderer/READING_LIST.md)

## Working Rules

### 1. Do not assume pure GDExtension runtime integration

Tooling and importer work can live there.
The runtime renderer may need an engine module or fork.

### 2. Build the portable core before frontier features

Priority order:

1. benchmark suite
2. offline cluster/page pipeline
3. standalone Vulkan runtime
4. Godot integration
5. frontier accelerators

### 3. Do not widen scope casually

v1 is:

- static
- opaque
- desktop
- compute-first
- streamed

### 4. Every rendering feature needs validation

Always connect work to:

- a correctness check
- a benchmark scene
- a debug view

## Recommended Session Sequence

## Session group A: Feasibility and baseline

Tasks:

- benchmark stock Godot scenes
- map current renderer extension points
- freeze delivery vehicle decision

## Session group B: Offline pipeline

Tasks:

- inspect meshoptimizer pipeline
- implement cluster metadata builder
- implement hierarchy and page packing
- validate cluster cracks and error metrics

## Session group C: Standalone renderer

Tasks:

- Vulkan setup
- GPU buffers and residency model
- compute culling
- visibility buffer
- material resolve
- shadow pass

## Session group D: Godot integration

Tasks:

- importer and resources
- editor/debug UX
- runtime scene integration
- benchmark execution inside Godot

## Session group E: Frontier branches

Tasks:

- mesh shader acceleration
- foliage path
- compressed geometry
- procedural resurfacing

## Example Session Prompt: Benchmark Baseline

```
We are in Phase 0 of Project Meridian.

Authoritative docs:
- PROJECT_PLAN.md
- TECHNICAL_SPEC.md
- BENCHMARK_PLAN.md

Today the goal is to build the stock Godot benchmark harness and capture baseline numbers.
Do not work on the runtime renderer yet.

Deliverables:
1. benchmark scene list
2. scripts or commands to run benchmarks
3. results template
4. notes on which current Godot features are active in each baseline run
```

## Example Session Prompt: Offline Builder

```
We are in Phase 1 of Project Meridian.

Authoritative docs:
- PROJECT_PLAN.md
- TECHNICAL_SPEC.md
- ARCHITECTURE_DECISIONS.md
- IMPLEMENTATION_BACKLOG.md

Today the goal is to implement the first version of the offline cluster builder.
Focus only on:
1. meshlet generation
2. hierarchy metadata
3. page packing format
4. builder-side validation output

Do not start Godot runtime integration in this session.
```

## Example Session Prompt: Standalone Runtime

```
We are in Phase 2 of Project Meridian.

Authoritative docs:
- PROJECT_PLAN.md
- TECHNICAL_SPEC.md
- BENCHMARK_PLAN.md

Today the goal is to implement the compute-driven visible-cluster path in the standalone Vulkan prototype.

Deliverables:
1. instance culling
2. hierarchy traversal
3. visible cluster compaction
4. debug visualization proving the correct active hierarchy cut

Do not add mesh shaders yet.
```

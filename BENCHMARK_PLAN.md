# Benchmark Plan

Last updated: 2026-03-23

## Purpose

This benchmark plan exists to prevent the project from optimizing against the wrong baseline.

The renderer must prove value against real Godot features and, where possible, against comparable Unreal Nanite scenes.

## Benchmark Principles

1. Compare against stock Godot, not a weak strawman.
2. Separate correctness, performance, and memory results.
3. Use repeatable scenes and camera paths.
4. Capture both cold and warm streaming behavior.

## Baselines

Each benchmark scene should be run in these modes where applicable:

1. stock Godot Forward+
2. stock Godot with auto mesh LOD
3. stock Godot with visibility ranges / HLOD
4. stock Godot with occlusion culling
5. Meridian runtime path

Optional comparison:

6. Unreal Nanite equivalent scene

## Benchmark Scene Set

## Scene A: photogrammetry ruins

Goals:

- evaluate scanned static geometry
- evaluate shadow cost
- evaluate memory residency under traversal

## Scene B: dense architecture block

Goals:

- evaluate visible cluster count under urban occlusion
- evaluate material resolve stability

## Scene C: rock field with heavy instancing

Goals:

- evaluate instance scaling
- evaluate shadow and HZB behavior

## Scene D: indoor occlusion stress scene

Goals:

- evaluate fine-grained occlusion culling
- evaluate streaming churn under rapid camera changes

## Scene E: vegetation-heavy hybrid test

Goals:

- evaluate the limits of the core clustered path
- provide a future benchmark for hybrid foliage work

## Metrics

Record at minimum:

- average frame time
- 1% low frame time
- GPU time by major pass
- CPU frame time
- visible instance count
- visible cluster count
- submitted vs rejected clusters
- shadow pass time
- residency misses
- VRAM use
- disk read volume during traversal

## Image and Quality Checks

Each benchmark run should also capture:

- reference screenshots
- LOD heat map
- page residency heat map
- shadow artifact notes
- visible crack / popping notes

## Test Conditions

For each scene define:

- hardware
- driver version
- renderer backend
- resolution
- shadow settings
- texture settings
- whether benchmarks are cold or warm cache

Use fixed scripted camera paths where possible.

## Reporting Format

For each benchmark run, store:

- scene ID
- build ID
- git commit
- date
- hardware profile
- config profile
- numeric metrics
- image captures
- notes

## Milestone Gates

## Gate 0: feasibility

Must produce stock Godot baseline numbers for at least three scenes.

## Gate 1: offline pipeline complete

Must validate import/build correctness on the benchmark assets.

## Gate 2: standalone renderer viable

Must beat stock Forward+ on at least one target scene without unacceptable artifacts.

## Gate 3: Godot integration viable

Must reproduce the standalone renderer advantage inside Godot on at least one benchmark scene.

## Gate 4: competitive renderer

Must beat stock Forward+ on multiple scenes and hold memory steady under traversal.

## Tools

Use:

- Godot profiler
- Vulkan validation layers
- RenderDoc
- vendor profilers as needed
- custom in-engine counters

## Benchmark Assets

Maintain a dedicated benchmark asset set that is not mixed with demo content.

Every benchmark asset should have:

- source mesh reference
- import settings
- fallback mesh data
- licensing note

## Output Location

Store benchmark artifacts in a dedicated structure such as:

```text
benchmarks/
  scenes/
  captures/
  results/
  scripts/
```

# Implementation Backlog

Last updated: 2026-03-23

This backlog is prioritized for the portable core first.

## P0: Architecture and Baseline

- freeze the document set and architecture decisions
- choose the likely runtime delivery vehicle after feasibility work
- define benchmark hardware profiles
- build the benchmark scene list
- create result templates and capture scripts

## P1: Offline Builder

- implement meshlet generation wrapper around `meshoptimizer`
- define cluster metadata schema
- implement hierarchy construction
- compute geometric error per cluster
- implement page packing
- define serialization format
- build a validator that checks bounds, parent links, and page references
- emit debug inspection output from the builder

## P2: Importer and Resources

- define `VGeoMesh` resource schema
- write importer settings surface
- generate fallback meshes
- connect the builder to Godot import flow
- add asset metadata and versioning

## P3: Standalone Runtime Core

- Vulkan bootstrap
- resource upload path
- persistent GPU buffer layout
- instance culling pass
- hierarchy traversal pass
- visible cluster compaction
- page residency table
- visibility buffer pass
- HZB build
- material resolve for constrained PBR
- directional shadow pass

## P4: Streaming

- async page request system
- page state machine
- residency budget controls
- priority heuristic for visible and near-visible pages
- eviction policy
- cold-start and traversal stress testing

## P5: Godot Runtime Integration

- runtime scene binding strategy
- `VGeoMeshInstance3D`
- debug view controls
- benchmark controls in-editor
- material mapping bridge for the supported subset
- shadow integration validation inside Godot

## P6: Validation and Tooling

- CPU/GPU visibility comparison tools
- crack detection scenes
- residency heat map
- LOD heat map
- shadow artifact visualization
- perf counter overlay

## P7: Performance Pass

- cluster traversal optimization
- page scheduler tuning
- material resolve optimization
- shadow optimization
- import/build speed optimization
- vendor-specific profiling on AMD and NVIDIA

## P8: Frontier Branches

- mesh shader acceleration path
- hybrid foliage path
- compressed geometry experiments
- procedural resurfacing experiments
- RT-aware geometry experiments
- work graph backend research

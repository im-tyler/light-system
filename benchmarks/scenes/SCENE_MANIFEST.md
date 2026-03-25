# Benchmark Scene Manifest

Last updated: 2026-03-23

Use this file to freeze the first benchmark scene set.

## Selection Rules

- each scene must stress a different dense-geometry failure mode
- scenes must be legally usable for repeated benchmarking
- at least one scene must be occlusion-heavy
- at least one scene must be instance-heavy
- at least one scene must stress dense static source geometry

## Frozen Phase 0 Synthetic Set

These are the frozen pre-real-asset benchmark scenes for current stock Godot and Meridian prototype baseline work:

1. `benchmark_terrace_manifest.txt`
2. `benchmark_arch_block_manifest.txt`
3. `benchmark_gltf_block_manifest.txt`

Reasoning:

- terrace grid stresses large contiguous dense geometry and LOD/page behavior
- architectural block stresses seam locking, repeated materials, and occlusion-like churn
- glTF block stresses the richer import path and instanced-node style layout

The remaining synthetic scenes stay as focused importer/edge-case validation assets, not baseline anchors.

## Initial Candidate Set

## Builder preflight scene

### Synthetic terrace grid

Purpose:

- deterministic large-scene builder verification
- interior-node exact-match LOD linkage checks
- base and LOD page coverage checks

Files:

- `benchmark_terrace_manifest.txt`
- generated source at `generated/terrace_grid.obj`

### Synthetic architectural block

Purpose:

- deterministic architectural seam-lock validation
- multi-page dependency validation across wall and roof materials
- runtime residency and prefetch simulation without external assets

Files:

- `benchmark_arch_block_manifest.txt`
- generated source at `generated/arch_block.obj`

### Synthetic glTF block scene

Purpose:

- deterministic glTF import validation
- node-transform flattening checks
- seam-lock and dependency validation through the richer asset path

Files:

- `benchmark_gltf_block_manifest.txt`
- generated sources at `generated/gltf_block_scene.gltf` and `generated/building_block.bin`

### Synthetic sparse glTF block scene

Purpose:

- deterministic sparse accessor import validation
- unnamed-material fallback validation through glTF material order
- replay validation of richer imported-edge cases without external assets

Files:

- `benchmark_sparse_gltf_block_manifest.txt`
- generated sources at `generated/sparse_gltf_block_scene.gltf` and `generated/sparse_building_block.bin`

### Synthetic UV seam glTF scene

Purpose:

- deterministic UV seam locking validation
- shared-position attribute discontinuity coverage
- builder summary validation through `seam_locked_vertices`

Files:

- `benchmark_uv_seam_gltf_manifest.txt`
- generated sources at `generated/uv_seam_plane.gltf` and `generated/uv_seam_plane.bin`

### Scene A: photogrammetry ruins

Purpose:

- dense static scan data
- shadow stress
- residency stress during traversal

Needed:

- source asset note
- camera path
- target quality settings

### Scene B: dense architecture block

Purpose:

- urban occlusion
- repeated materials
- high visible-cluster churn during movement

Needed:

- source asset note
- camera path
- target quality settings

### Scene C: rock field with heavy instancing

Purpose:

- instance scaling
- repeated geometry
- shadow and culling stress

Needed:

- source asset note
- camera path
- target quality settings

### Scene D: indoor occlusion stress scene

Purpose:

- strong occlusion
- rapid visibility changes
- streaming churn

Needed:

- source asset note
- camera path
- target quality settings

### Scene E: vegetation-heavy hybrid future scene

Purpose:

- future benchmark for foliage and aggregate-geometry work

Status:

- do not block portable-core work on this scene

## Freeze Rule

Do not change the initial three Phase 0 benchmark scenes once baseline capture begins unless there is a clear documented reason.

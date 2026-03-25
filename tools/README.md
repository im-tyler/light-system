# Tools

This directory holds offline builder inputs and utility scripts.

## Current files

- [sample_asset_manifest.txt](/Users/tyler/Documents/renderer/tools/sample_asset_manifest.txt): first-pass builder input format

## Manifest format

Current builder manifests use simple `key = value` lines.

Supported keys:

- `asset_id`
- `source_asset`
- `output_path`
- `bounds_min`
- `bounds_max`
- `emit_fallback`
- `cluster_vertex_limit`
- `cluster_triangle_limit`
- `page_cluster_limit`
- `hierarchy_partition_size`
- `material_slots`
- `bounds_padding`

The current builder is intentionally minimal. It writes a first-pass `.vgeo` binary with metadata tables and a readable summary file.

Current source-asset support:

- `.obj`
- `.gltf`
- `.glb`

Baseline glTF coverage currently includes:

- node world-transform flattening
- indexed and non-indexed triangle primitives
- sparse POSITION and index accessor unpacking
- material mapping by matching material name, or by material order when names are absent and slot counts match
- seam locking across material boundaries and baseline normal/UV discontinuities at shared positions

Current clustering behavior:

- `meshopt_buildMeshlets` with `cluster_vertex_limit` and `cluster_triangle_limit`
- `meshopt_optimizeMeshlet` per generated meshlet
- page grouping up to `page_cluster_limit` for both base and LOD payload domains
- hierarchy grouping up to `hierarchy_partition_size`
- `clodBuild` generation of simplified LOD groups and clusters
- adjacent page dependency hints derived from exact node/LOD replacement chains
- cross-material seam locking during simplification for current OBJ and glTF material-section paths

This is now a real meshoptimizer-backed first-pass builder, but it still lacks:

- broader crack-safe simplification validation beyond the current OBJ/material path
- deeper runtime integration of the generated LOD metadata
- broader glTF feature coverage beyond the baseline subset
- compression and streaming-oriented packing refinements

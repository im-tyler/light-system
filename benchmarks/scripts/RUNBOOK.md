# Benchmark Runbook

Last updated: 2026-03-23

## Phase 0 Tasks

1. choose three benchmark scenes
2. define deterministic camera paths
3. record stock Godot baseline numbers
4. store results in `results/`

## Preflight asset generation

Before builder-side verification of the synthetic terrace scene, run:

```bash
python3 benchmarks/scripts/generate_terrace_grid.py \
  --output benchmarks/scenes/generated/terrace_grid.obj

python3 benchmarks/scripts/generate_arch_block.py \
  --output benchmarks/scenes/generated/arch_block.obj

python3 benchmarks/scripts/generate_gltf_block.py \
  --output benchmarks/scenes/generated/gltf_block_scene.gltf

python3 benchmarks/scripts/generate_sparse_gltf_block.py \
  --output benchmarks/scenes/generated/sparse_gltf_block_scene.gltf

python3 benchmarks/scripts/generate_uv_seam_gltf.py \
  --output benchmarks/scenes/generated/uv_seam_plane.gltf
```

Useful prototype checks:

- `prototype/build/meridian_trace --manifest benchmarks/scenes/benchmark_terrace_manifest.txt --error-threshold 0.03 --resident-pages all --detail verbose`
- `prototype/build/meridian_trace --manifest benchmarks/scenes/benchmark_arch_block_manifest.txt --error-threshold 3.0 --resident-pages all --detail verbose`
- `prototype/build/meridian_residency --manifest benchmarks/scenes/benchmark_terrace_manifest.txt --error-threshold 0.03 --frames 4 --resident-budget 20 --bootstrap-resident none --eviction-grace 1`
- `prototype/build/meridian_residency --manifest benchmarks/scenes/benchmark_arch_block_manifest.txt --error-threshold 3.0 --frames 4 --resident-budget 12 --bootstrap-resident none --eviction-grace 1`
- `prototype/build/meridian_replay --manifest benchmarks/scenes/benchmark_terrace_manifest.txt --script benchmarks/replays/terrace_streaming_replay.txt`
- `prototype/build/meridian_replay --manifest benchmarks/scenes/benchmark_arch_block_manifest.txt --script benchmarks/replays/arch_streaming_replay.txt --detail verbose`
- `prototype/build/meridian_replay --manifest benchmarks/scenes/benchmark_gltf_block_manifest.txt --script benchmarks/replays/gltf_streaming_replay.txt --detail verbose`
- `prototype/build/meridian_replay --manifest benchmarks/scenes/benchmark_sparse_gltf_block_manifest.txt --script benchmarks/replays/sparse_gltf_streaming_replay.txt --detail verbose`
- `prototype/build/meridian_builder --manifest benchmarks/scenes/benchmark_uv_seam_gltf_manifest.txt` and inspect `build/benchmark_uv_seam_gltf.vgeo.summary.txt` for `seam_locked_vertices`

Real imported-asset workflow:

- `python3 benchmarks/scripts/run_import_validation.py --asset /absolute/path/to/asset.gltf --asset-id real_arch_asset --material-slots stone_wall,roof_cap`

Stock Godot synthetic baseline workflow:

- `python3 benchmarks/scripts/run_godot_baseline.py --scene-id benchmark_terrace --asset res://benchmarks/scenes/generated/terrace_grid.obj`
- `python3 benchmarks/scripts/run_godot_baseline.py --scene-id benchmark_arch_block --asset res://benchmarks/scenes/generated/arch_block.obj`
- `python3 benchmarks/scripts/run_godot_baseline.py --scene-id benchmark_gltf_block --asset res://benchmarks/scenes/generated/gltf_block_scene.gltf`

## Required Output Per Run

- one CSV row in [RESULT_TEMPLATE.csv](/Users/tyler/Documents/renderer/benchmarks/results/RESULT_TEMPLATE.csv)
- one markdown result note
- screenshots
- config notes

## Baseline Modes

Capture where relevant:

1. stock Forward+
2. stock auto mesh LOD
3. stock HLOD / visibility ranges
4. stock occlusion culling

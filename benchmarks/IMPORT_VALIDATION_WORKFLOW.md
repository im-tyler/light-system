# Import Validation Workflow

Use this workflow to validate a real external asset through the current Meridian contract:

1. build `prototype/build/meridian_builder` and `prototype/build/meridian_replay`
2. run `benchmarks/scripts/run_import_validation.py`
3. inspect the generated manifest, replay script, summary file, and markdown report

Example:

```bash
python3 benchmarks/scripts/run_import_validation.py \
  --asset /absolute/path/to/asset.gltf \
  --asset-id real_arch_asset \
  --material-slots stone_wall,roof_cap
```

Generated outputs:

- `benchmarks/scenes/external/<asset_id>_manifest.txt`
- `benchmarks/replays/external/<asset_id>_replay.txt`
- `build/<asset_id>.vgeo`
- `build/<asset_id>.vgeo.summary.txt`
- `benchmarks/results/<asset_id>_import_validation.md`

Current intended use:

- real architectural glTF assets
- real scanned glTF assets that fit the current baseline importer subset

Current baseline importer limits still apply:

- triangle primitives only
- no Draco
- no skins
- no morph targets
- no `EXT_mesh_gpu_instancing`
- material mapping must resolve by name or by material order fallback

Current real external asset results in this workspace:

- `prototype/thirdparty/meshoptimizer/gltf/fuzz.glb` passes the full builder + replay + stock Godot + Vulkan debug chain
- `prototype/thirdparty/meshoptimizer/demo/pirate.glb` now passes builder + replay + Vulkan debug runtime + thin Godot `.vgeo` importer spike after `EXT_meshopt_compression` decode support
- stock Godot does not currently load `pirate.glb` directly in this workspace, so stock-Godot comparison still needs a directly importable source asset or conversion step

# Benchmarks

Use this directory for repeatable benchmark assets and results only.

## Layout

- [scenes](/Users/tyler/Documents/renderer/benchmarks/scenes): benchmark scene manifests and scene assets
- [scripts](/Users/tyler/Documents/renderer/benchmarks/scripts): runbooks and automation
- [results](/Users/tyler/Documents/renderer/benchmarks/results): structured benchmark outputs
- [captures](/Users/tyler/Documents/renderer/benchmarks/captures): screenshots, RenderDoc captures, profiler exports

External asset validation:

- [IMPORT_VALIDATION_WORKFLOW.md](/Users/tyler/Documents/renderer/benchmarks/IMPORT_VALIDATION_WORKFLOW.md): real imported-asset workflow using generated manifests and replay scripts

## Rules

- do not mix demo scenes with benchmark scenes
- keep camera paths deterministic
- record config, hardware, and commit for every result
- preserve both cold and warm streaming results where relevant

## Current preflight scene

- `scenes/benchmark_terrace_manifest.txt`: deterministic generated grid scene used to stress hierarchy/Lod linkage before real benchmark capture begins
- `scenes/benchmark_arch_block_manifest.txt`: deterministic generated architectural scene used to validate seam locking and page dependencies
- `scenes/benchmark_gltf_block_manifest.txt`: deterministic glTF scene used to validate the richer import path, node transforms, and runtime replay
- `scenes/benchmark_sparse_gltf_block_manifest.txt`: deterministic sparse glTF scene used to validate sparse accessor import and unnamed-material fallback
- `scenes/benchmark_uv_seam_gltf_manifest.txt`: deterministic glTF scene used to validate UV-seam locking at shared positions
- use `prototype/build/meridian_trace` with this scene to inspect fallback and prefetch behavior under different residency masks
- use `prototype/build/meridian_residency` with this scene to inspect multi-frame request/load/evict behavior before a GPU runtime exists
- use `prototype/build/meridian_replay` with `replays/*.txt` to run the canonical deterministic runtime-contract validation path

## Frozen synthetic baseline set

- `benchmark_terrace_manifest.txt`
- `benchmark_arch_block_manifest.txt`
- `benchmark_gltf_block_manifest.txt`

Current stock Godot baseline workflow runs against these frozen synthetic scenes before real external assets are folded in.

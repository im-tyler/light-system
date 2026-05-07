# Project Meridian

Dense / virtualized geometry renderer for Godot 4 — Nanite as the parity bar. C++ standalone Vulkan prototype plus Godot importer plugin. Builds an offline cluster + page format (`.vgeo`), then streams and renders it with compute culling, hierarchical traversal, visibility-buffer rasterization, and cascaded shadow maps.

**Status: working — Phase 1 complete, Phase 2 in progress.** The offline builder is functional on glTF, OBJ, and synthetic benchmarks (Stanford Dragon, generated 1M-triangle city). The standalone Vulkan renderer runs an interactive present loop on Apple M4 / MoltenVK with measured per-pass GPU timings.

This is a separate Forgejo repository ([Tyler/meridian](http://100.108.123.49:49152/Tyler/meridian.git)) within the [Light System umbrella](https://github.com/) — it is gitignored from the umbrella repo because it has its own commit history, its own build system, and a much faster iteration cadence than the umbrella docs.

## Documentation

The detailed planning and implementation docs live under [`docs/`](./docs/):

| Doc | Purpose |
|-----|---------|
| [PROJECT_PLAN.md](./docs/PROJECT_PLAN.md) | Project roadmap and phase overview. |
| [TECHNICAL_SPEC.md](./docs/TECHNICAL_SPEC.md) | Portable-core renderer specification. |
| [ARCHITECTURE_DECISIONS.md](./docs/ARCHITECTURE_DECISIONS.md) | Architectural decision records (ADRs). |
| [IMPLEMENTATION_STATUS.md](./docs/IMPLEMENTATION_STATUS.md) | Truth-about-now: what is built, what works, known issues. |
| [IMPLEMENTATION_BACKLOG.md](./docs/IMPLEMENTATION_BACKLOG.md) | Prioritized work queue. |
| [TASK_LIST.md](./docs/TASK_LIST.md) | Current execution checklist. |
| [PHASE0_CHECKLIST.md](./docs/PHASE0_CHECKLIST.md) | Feasibility gate checklist. |
| [BENCHMARK_PLAN.md](./docs/BENCHMARK_PLAN.md) | Benchmark methodology. |
| [GODOT_UNREAL_GAP_ANALYSIS.md](./docs/GODOT_UNREAL_GAP_ANALYSIS.md) | What Nanite has that Godot lacks, mapped to deliverables. |
| [COMPETITIVE_PLAN.md](./docs/COMPETITIVE_PLAN.md) | Where Meridian sits relative to Nanite, UE5 Lumen, etc. |
| [FRONTIER_OPPORTUNITIES.md](./docs/FRONTIER_OPPORTUNITIES.md) | Beyond-Nanite research directions. |
| [READING_LIST.md](./docs/READING_LIST.md) | Curated papers and references. |

If you are new, read in this order: **PROJECT_PLAN -> TECHNICAL_SPEC -> IMPLEMENTATION_STATUS**.

## Working Directories

```
meridian/
  prototype/         standalone Vulkan renderer (C++)
  godot-vgeo/        Godot-side importer and runtime integration
  godot/             Godot project shell for runtime testing
  addons/            Godot editor addons (meridian_importer)
  lumen_gdExtension/ exploratory GDExtension shell
  benchmarks/        scenes, scripts, captures, results
  schemas/           data and file-format schemas (VGEO_RESOURCE, PAGE_LAYOUT)
  notes/             status notes (PHASE0_STATUS, RUNTIME_DELIVERY_FEASIBILITY)
  tools/             builder and utility scripts
  assets/            small sample assets (sample_rock.obj)
  docs/              all planning and architecture docs (see above)
  _internal/         internal AI tooling guidance (gitignored)
```

## Quick Status — What Works Today

### Phase 1 — Offline Builder (Complete)
- `meridian_builder` CLI: manifest -> meshlet generation -> hierarchy -> LOD -> page packing -> `.vgeo` serialization.
- `.obj` and `.gltf`/`.glb` import (sparse accessors, meshopt compression, seam locking).
- Meshlet generation, optimization, and bounds via meshoptimizer.
- Partition-based hierarchy tree with clusterlod simplification-backed LOD groups.
- Seam-safe cross-material simplification.
- Dual payload domains (base clusters + LOD clusters) with page table.
- Adjacent-replacement-level page dependency hints for streaming prefetch.
- Validated on synthetic benchmarks, external pirate.glb, Stanford Dragon (871K triangles), generated 1M-triangle city.

### Phase 2 — Standalone Vulkan Renderer (In Progress)
Per-frame GPU pipeline (timed via `VK_EXT_calibrated_timestamps`):

1. Compute instance culling (frustum AABB test, atomic append).
2. Cluster/LOD selection (CPU `simulate_traversal` with normal-cone backface cull).
3. Occlusion refinement (project cluster AABB against previous-frame HZB).
4. Shadow pass (3 cascaded shadow maps, log/uniform split blend lambda=0.7).
5. Main geometry pass (vertex pulling from payload SSBOs, smooth normals + 8-tap Poisson PCF).
6. HZB construction (depth-copy + per-mip max downsample).

Performance (Apple M4, MoltenVK, 1280x720):

- Stanford Dragon (871K tris): median 19.2ms / 52 FPS.
- Massive City (1M tris): median 65.5ms / 15.3 FPS (improving — LOD threshold tuning pending).

Interactive mode: `--interactive` for continuous present loop, WASD + mouse look + Q/E vertical, FPS in window title.

See [`docs/IMPLEMENTATION_STATUS.md`](./docs/IMPLEMENTATION_STATUS.md) for the full pipeline state, known issues, and per-pass timings.

## Not Yet Implemented

- Per-cluster backface culling on LOD clusters (currently base-only).
- Real demand-streaming scheduler (CPU prototype exists; pages start all-resident by default).
- Production async disk I/O for page loading (validation path exists).
- Benchmark automation vs stock Godot.
- Texture / UV support (all shading currently procedural).
- Broader glTF import coverage.
- Compressed geometry payloads.
- Deeper Godot runtime integration.
- Parallel GPU traversal (BFS-per-level or workgroup-DFS).

## Build

See [`prototype/README.md`](./prototype/README.md) for the standalone Vulkan renderer build instructions and [`benchmarks/README.md`](./benchmarks/README.md) for running benchmarks.

Vulkan SDK 1.2+ is required. The renderer is developed on Apple Silicon via MoltenVK; Linux + native Vulkan should work but is less battle-tested. Windows is untested at the time of writing.

## License

See the umbrella [Light System LICENSE](../LICENSE) — MIT for project-authored code.

The `prototype/thirdparty/meshoptimizer/` checkout is vendored at build time and is gitignored. Honor its upstream license.

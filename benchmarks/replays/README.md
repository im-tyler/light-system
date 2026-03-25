# Replay Scripts

These scripts drive deterministic multi-frame validation through `prototype/build/meridian_replay`.

Current scripts:

- `sample_material_replay.txt`: tiny sample validation for fallback and page bring-up
- `terrace_streaming_replay.txt`: larger LOD-heavy streaming validation
- `arch_streaming_replay.txt`: architectural seam/dependency validation with mixed base/LOD selection
- `gltf_streaming_replay.txt`: richer imported-asset validation through the baseline glTF path
- `sparse_gltf_streaming_replay.txt`: sparse-accessor and unnamed-material fallback validation through the glTF path
- `uv_seam_gltf_replay.txt`: UV seam asset smoke replay; primary seam check still comes from summary `seam_locked_vertices`

Current replay model is intentionally simple:

- per-frame error-threshold schedule
- fixed resident budget
- fixed eviction grace
- fixed bootstrap residency mode

This is a CPU-side proxy for camera pressure until the real GPU runtime consumes the same contract directly.

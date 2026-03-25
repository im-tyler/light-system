# Frozen Synthetic Godot Baseline

Captured on this machine with the stock Godot spike project and automation.

Current quick-pass numbers at `1280x720`, `20` measured frames, `5` warmup frames:

- `benchmark_terrace`: `16.3799 ms`, `61.05 FPS`, renderer `forward_plus`
- `benchmark_arch_block`: `0.5383 ms`, `1857.76 FPS`, renderer `forward_plus`
- `benchmark_gltf_block`: `16.4526 ms`, `60.78 FPS`, renderer `forward_plus`

Notes:

- These are early synthetic baselines, not final published benchmark numbers.
- The architectural OBJ scene is far lighter for stock Godot than the larger grid/glTF scenes, so its number is not directly comparable.
- Real external-asset baseline capture should supersede this file once the first imported benchmark asset is validated.

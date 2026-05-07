# Reading List & Reference Materials

## Current Reading Order For This Plan

Read in this order for the current architecture:

1. [PROJECT_PLAN.md](/Users/tyler/Documents/renderer/PROJECT_PLAN.md)
2. [TECHNICAL_SPEC.md](/Users/tyler/Documents/renderer/TECHNICAL_SPEC.md)
3. [ARCHITECTURE_DECISIONS.md](/Users/tyler/Documents/renderer/ARCHITECTURE_DECISIONS.md)
4. `meshoptimizer` `demo/nanite.cpp` and `clusterlod.h`
5. Nanite technical docs and SIGGRAPH material
6. `lighthugger` and Wicked Engine visibility-buffer references
7. [BENCHMARK_PLAN.md](/Users/tyler/Documents/renderer/BENCHMARK_PLAN.md)

Read these later as frontier branches, not portable-core blockers:

- [FRONTIER_OPPORTUNITIES.md](/Users/tyler/Documents/renderer/FRONTIER_OPPORTUNITIES.md)
- AMD Dense Geometry Format
- GPU work graphs / mesh nodes
- procedural resurfacing
- procedural tree generation
- neural-rendering-specific compression paths

## Priority Order: Read These First

### 1. meshoptimizer Nanite-like LOD Demo (CRITICAL — START HERE)
- **What:** Working reference implementation of hierarchical meshlet LOD
- **URL:** https://github.com/zeux/meshoptimizer — see `demo/nanite.cpp`
- **Also:** https://deepwiki.com/zeux/meshoptimizer/3.2-nanite-like-lod-system
- **Also:** `clusterlod.h` single-header library for continuous LOD
- **Why first:** This is the offline processing pipeline we build on. Everything else depends on understanding this.
- **Key functions:** `meshopt_buildMeshlets`, `meshopt_partitionClusters`, `meshopt_simplifyWithUpdate`, `meshopt_SimplifyLockBorder`, `meshopt_SimplifySparse`
- **License:** MIT

### 2. Nanite SIGGRAPH 2021 Deep Dive (CRITICAL)
- **What:** 155-page PDF explaining every aspect of Nanite
- **URL:** http://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- **Video/slides:** https://www.wihlidal.com/projects/nanite-deepdive/
- **Why:** The theoretical foundation. Every architectural decision is explained and justified.
- **Key sections:** Cluster hierarchy (slide 27-46), LOD error function, GPU culling, software rasterizer, streaming

### 3. lighthugger — Vulkan Meshlet + Visibility Buffer Renderer (CRITICAL)
- **What:** Working C++20 Vulkan renderer with meshlets, visibility buffer, compute culling
- **URL:** https://github.com/expenses/lighthugger
- **Why:** This is essentially our Phase 1 prototype already built. Study the architecture.
- **Key features:** Compute-emulated mesh shaders, visibility buffer, single-pass lighting resolve, cascaded shadow maps, DDS/KTX2 texture loading
- **License:** MIT available on request

### 4. Wicked Engine Visibility Buffer & Mesh Shaders
- **What:** Production open-source engine with visibility buffer + optional mesh shaders
- **URL:** https://github.com/turanszkij/WickedEngine
- **Blog (2024 graphics overview):** https://wickedengine.net/2024/12/wicked-engines-graphics-in-2024/
- **Blog (texture derivatives):** Search "Wicked Engine visibility buffer derivatives" — step-by-step guide
- **Why:** Shows how to integrate visibility buffer into a full engine with materials, shadows, effects
- **Key detail:** 25-bit meshlet ID + 7-bit triangle ID encoding, bindless ShaderScene, amplification shader occlusion culling against depth pyramid
- **License:** MIT

### 5. vkguide.dev GPU-Driven Rendering Tutorial
- **What:** Complete tutorial building a GPU-driven Vulkan renderer from scratch
- **URL:** https://vkguide.dev/docs/gpudriven/gpu_driven_engines/
- **Why:** Step-by-step walkthrough of compute culling, indirect draws, bindless textures, SSBO scene data
- **Key result:** 125,000 objects culled and rendered at 290 FPS, 40M+ triangles

## Mesh Shader References

### 6. AMD GPUOpen Mesh Shader Series (ESSENTIAL for cross-vendor)
- **Overview:** https://gpuopen.com/learn/mesh_shaders/mesh_shaders-index/
- **From vertex to mesh shader:** https://gpuopen.com/learn/mesh_shaders/mesh_shaders-from_vertex_shader_to_mesh_shader/
- **Optimization & best practices:** https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
- **Meshlet compression (Best Paper GCPR 2024):** https://gpuopen.com/learn/mesh_shaders/mesh_shaders-meshlet_compression/
- **GDC 2024 RDNA3 mesh shaders:** https://gpuopen.com/gdc-presentations/2024/GDC2024_Mesh_Shaders_in_AMD_RDNA_3_Architecture.pdf
- **Why:** AMD's perspective on optimal meshlet sizes, thread group configurations, compression. Essential for cross-vendor correctness.
- **Key guidance:** 64 vertices / 124 triangles recommended, thread group size = max vertices

### 7. NVIDIA Mesh Shader Resources
- **Introduction to Turing Mesh Shaders:** https://developer.nvidia.com/blog/introduction-turing-mesh-shaders/
- **Professional graphics mesh shaders:** https://developer.nvidia.com/blog/using-mesh-shaders-for-professional-graphics/
- **nvpro-samples CAD scene:** https://github.com/nvpro-samples/gl_vk_meshlet_cadscene
- **Why:** NVIDIA's perspective, different optimization priorities from AMD

### 8. Mesh Shaders Cross-Platform Overview
- **Linebender wiki:** https://linebender.org/wiki/gpu/mesh-shaders/
- **Key info:** VK_EXT_mesh_shader is cross-platform (not NVIDIA-specific). Supported on GTX 1650+, RDNA2+, Intel Arc, Metal 3. NOT yet in WebGPU.
- **DirectX 12 Mesh Shader Spec:** https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html

## Visibility Buffer Theory

### 9. Original Visibility Buffer Paper
- **Burns and Hunt (2013):** http://jcgt.org/published/0002/02/04/
- **Why:** Foundational paper explaining the cache-friendly deferred shading approach

### 10. The Forge / ConfettiFX
- **URL:** https://github.com/ConfettiFX/The-Forge
- **Blog:** http://diaryofagraphicsprogrammer.blogspot.com/2018/03/triangle-visibility-buffer.html
- **Why:** Production-proven visibility buffer (shipped in Star Wars: Bounty Hunter, Call of Duty: Warzone Mobile). Cross-platform including consoles.
- **License:** Apache 2.0

### 11. SIGGRAPH 2024 — Visibility Buffer Advances
- **URL:** https://advances.realtimerendering.com/s2024/index.html
- **Key talk:** "Visibility Buffer rendering" by John Hable — shading rate decoupled from resolution, pixel reduction while maintaining fidelity
- **Key talk:** "Seamless Rendering on Mobile: Adaptive LOD Pipeline" — Nanite-like approach for mobile GPUs (Tencent)

## Godot-Specific References

### 12. Godot RenderingDevice API
- **Docs:** https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html
- **Source:** https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device.h
- **Vulkan progress report:** https://godotengine.org/article/vulkan-progress-report-1/

### 13. Godot Mesh Shader Proposals & PR
- **Proposal #6822 (2023):** https://github.com/godotengine/godot-proposals/issues/6822
- **Proposal #11272 (2024):** https://github.com/godotengine/godot-proposals/issues/11272
- **PR #88934 (implementation):** https://github.com/godotengine/godot/pull/88934/files
- **Rendering hooks PR #80214:** Referenced in proposals
- **Status:** PR written but not merged. The proposal explicitly notes this is "about laying the foundation for future proposals and allowing interested developers to use mesh shaders in their own forks or plugins."

### 14. Godot Virtualized Geometry Proposal
- **Proposal #2793:** https://github.com/godotengine/godot-proposals/issues/2793
- **Why:** Community discussion about what integration would look like. 60+ thumbs up, open since 2021, no implementation.

### 15. Godot Renderer Source Code
- **Path:** `servers/rendering/renderer_rd/` — the RenderingDevice-based Forward+ renderer
- **Key files to study:** forward_clustered/, effects/, storage_rd/, shader_rd/

## 2025-2026 Cutting Edge Research

### 16. Procedural Mesh Resurfacing (Eurographics 2025)
- **Paper:** https://onlinelibrary.wiley.com/doi/10.1111/cgf.70075
- **What:** GPU mesh shaders generate detailed geometry at runtime from simple control meshes
- **Benefit:** Lower VRAM, lower power consumption vs pre-built geometry. Competitive performance with traditional pipelines.
- **Relevance:** Could be integrated as optional path for terrain/rocks/foliage within our mesh shader stage

### 17. NVIDIA Neural Rendering (GDC 2025)
- **URL:** https://developer.nvidia.com/blog/nvidia-rtx-advances-with-neural-rendering-and-digital-human-technologies-at-gdc-2025/
- **RTX Mega Geometry:** Accelerates BVH builds for cluster-based geometry — directly relevant hardware acceleration
- **Neural Texture Compression:** 7x VRAM savings
- **Relevance:** Future optimization opportunity (Phase 5+), currently NVIDIA-specific

### 18. SIGGRAPH 2025 Advances in Real-Time Rendering
- **URL:** https://www.advances.realtimerendering.com/s2025/index.html
- **Key talks:** Assassin's Creed Shadows RT-GI, Unreal MegaLights, strand hair rendering
- **HPG 2026:** Co-located with SIGGRAPH LA, July 2026 — watch for new papers

### 18a. DirectStorage 1.4 + Asset Conditioning (IMPORTANT for streaming architecture)
- **What:** Updated asset streaming and conditioning stack
- **URL:** https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/
- **Why:** Dense geometry is not just culling; it is also chunking, compression, and upload cost

### 18b. AMD Dense Geometry Format (IMPORTANT for future compressed geometry)
- **SDK:** https://gpuopen.com/dgf/
- **Overview:** https://gpuopen.com/learn/problem_increasing_triangle_density/
- **Vulkan extension:** Search GPUOpen for "Dense Geometry Format Vulkan extension"
- **Why:** Strong candidate for a future compressed geometry path that is friendlier to ray tracing

### 18c. GPU Work Graphs / Mesh Nodes (IMPORTANT frontier execution model)
- **DirectX work graphs:** https://devblogs.microsoft.com/directx/d3d12-work-graphs/
- **AMD mesh nodes:** https://gpuopen.com/learn/work_graphs_mesh_nodes/work_graphs_mesh_nodes-intro/
- **Why:** Relevant for future fully GPU-driven scheduling, but not a baseline dependency

### 18d. Real-Time GPU Tree Generation (IMPORTANT for beating Nanite on vegetation)
- **Paper:** https://diglib.eg.org/handle/10.2312/hpg20251168
- **Why:** One of the clearest examples of a hybrid path outperforming explicit stored dense geometry for foliage-like content

### 18e. Real-Time Procedural Resurfacing (IMPORTANT for selected asset classes)
- **Paper:** https://diglib.eg.org/items/0460b6c6-1216-4bf2-b6ac-14db88c65c45
- **Why:** Candidate path for rocks, bark, cliffs, and mesostructures where storing all triangles may be the wrong answer

## Vulkan Learning Resources

### 19. Sascha Willems Vulkan Examples
- **URL:** https://github.com/SaschaWillems/Vulkan
- **Why:** The definitive collection of Vulkan technique implementations. Indirect drawing, compute shaders, deferred rendering, MSAA, etc.

### 20. Vulkan Specification
- **URL:** https://registry.khronos.org/vulkan/specs/1.3-extensions/html/
- **VK_EXT_mesh_shader:** https://www.khronos.org/blog/mesh-shading-for-vulkan

## Additional Open Source Engines to Study

### 21. Bevy Meshlet Implementation
- **Discussion:** https://github.com/bevyengine/bevy/discussions/10433
- **Why:** Developer noted "meshlets + visibility buffer + two-pass occlusion culling + GPU-driven rendering gives you 60-70% of Nanite's benefits"
- **Note:** Bevy uses Rust + wgpu, not directly portable but algorithms are identical

### 22. Evergine Mesh Shader Integration
- **URL:** https://evergine.com/mesh-shaders-and-meshlets-support-on-low-level-api/
- **Why:** Clean example of mesh shader integration with meshoptimizer in a real engine (DX12 + Vulkan)

## Summary: What Already Exists vs What We Build

### Already exists (MIT/Apache licensed, ready to use):
- Meshlet generation → meshoptimizer
- Hierarchical LOD with crack-free transitions → meshoptimizer clusterlod.h
- Meshlet compression → meshoptimizer + AMD research
- Vulkan visibility buffer renderer → lighthugger
- Full engine with vis buffer + mesh shaders → Wicked Engine
- GPU-driven rendering tutorial → vkguide.dev
- Mesh shader Vulkan samples → NVIDIA nvpro-samples
- meshoptimizer already in Godot → no dependency to add

### We build (the novel integration work):
- Runtime GPU pipeline optimized for Godot's architecture
- Godot RenderingDevice integration
- Godot material/shader system bridge
- Import pipeline (glTF → meshlet hierarchy → Godot resource)
- Shadow integration with Godot's shadow maps
- Fallback path management (mesh shader vs compute)
- Debug visualization modes
- Documentation and examples

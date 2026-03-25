# Godot vs Unreal: Closing the Gap

Last updated: 2026-03-24

## Purpose

Define what Godot needs to match or exceed Unreal Engine for AAA-quality 3D game development, prioritized for a Kenshi-style destruction-focused sandbox.

## Where Godot Already Wins

| Area | Godot Advantage |
|------|-----------------|
| AI-assisted development | MCP ecosystem (10+ servers, 149+ tools), Claude can edit scenes, run projects, debug |
| Open source | No royalties, full source access, forkable |
| Lightweight | ~100MB vs Unreal's ~50GB |
| Iteration speed | Fast startup, no shader compilation hell |
| 2D | Native 2D engine, not 3D pretending |
| GDScript + AI codegen | Arguably faster than Blueprint in 2026 |

## The Gaps (What Unreal Has, Godot Doesn't)

### 1. Rendering Stack

| Component | Unreal | Godot | Status |
|-----------|--------|-------|--------|
| Dense geometry (Nanite) | Virtualized geometry, auto LOD, streaming | Basic mesh LOD, HLOD | **Building (Project Meridian)** |
| Global Illumination (Lumen) | Real-time GI, bounce light, sky contribution | SDFGI (experimental), baked lightmaps | Missing |
| Virtual Shadow Maps | Shadows follow Nanite LOD, massive scenes | Basic shadow maps | Missing |
| Ray Tracing | Full RT pipeline, reflections, GI | Limited/experimental | Partial |

**Lumen is the big missing piece.** Nanite solves geometry. Lumen solves lighting. They're separate systems but designed to work together.

### 2. Physics & Simulation (Jolt Extension Strategy)

| Component | Unreal | Godot + Jolt Extension | Est. Time | Difficulty |
|-----------|--------|------------------------|-----------|------------|
| Destruction | Chaos destruction, fracture on impact | Extend Jolt with fracture + structural stress | 2-4 weeks | Medium |
| Cloth | Chaos cloth, real-time simulation | Integrate NvCloth or PBD solver | 1-2 months | Medium-High |
| Fluid | Fluid simulation, Niagara fluids | SPH solver (standalone, talks to Jolt) | 2-4 months | High |
| Particles | Niagara (full VFX system) | GPUParticles3D | — | Acceptable |

**Strategy: Extend Godot's Jolt integration rather than build from scratch.**

#### Destruction Approach

1. Pre-fracture meshes offline (Voronoi decomposition)
2. At runtime: spawn fractured pieces as Jolt bodies
3. Add structural stress layer — track connections between pieces
4. Break connections when force exceeds threshold
5. Debris inherits velocity, interacts with other Jolt bodies

**Why this is fast:** Jolt handles collision, broadphase, rigid body dynamics. We add decomposition + constraints on top.

#### Cloth Approach

**Option A (Recommended):** Wrap NvCloth
- Free, production-tested (used in Unreal, NVIDIA)
- C++ library, GDExtension wrapper
- 2-4 weeks integration

**Option B:** Build PBD solver
- Position-based dynamics
- Particle-constraint system
- Integrates with Jolt's existing solver
- 1-2 months

**Option C:** Use existing open source
- e.g., cloth-cpp, libcloth
- Wrap and expose to Godot

#### Fluid Approach

- SPH (smoothed particle hydrodynamics) — most common real-time approach
- Doesn't fit naturally in Jolt's rigid body architecture
- Standalone solver that shares collision detection
- 2-4 months for basic implementation

### 3. AI Navigation

| Component | Unreal | Godot | Gap |
|-----------|--------|-------|-----|
| Pathfinding | NavigationMesh, AI controllers | NavigationAgent3D, NavigationServer | Adequate |
| Behavior trees | Full BT system, blackboards | Basic, third-party addons | Needs work |
| Crowds | MassAI, entity mass handling | Nothing | Missing |
| Smart objects | Objects advertise interactions to AI | Nothing | Missing |

**For squad-based army combat (50-500 units), crowd AI and behavior trees matter.**

### 4. Animation & Cinematics

| Component | Unreal | Godot | External Fix? |
|-----------|--------|-------|---------------|
| Cinematics | Sequencer (full NLE, camera tracks, shots) | AnimationPlayer (basic) | Blender renders cinematics |
| IK Rig | Full IK retargeting, Control Rig | Basic IK | Blender rigging |
| Animation retargeting | Skeleton sharing, retarget profiles | Basic | Blender export |
| Motion matching | Animation blending by physics | Nothing | Missing |

**Blender covers cinematics adequately.** In-engine is nice but not critical.

### 5. Asset Ecosystem (Unified Asset Browser Strategy)

| Component | Unreal | Godot + Unified Browser | Est. Time |
|-----------|--------|-------------------------|-----------|
| Procedural characters | MetaHuman | Reallusion + Blender | External |
| Scanned assets | Megascans | Quixel API integration | Done via browser |
| Marketplace | Integrated store | Unified browser + converters | 5-9 weeks |
| Unity assets | N/A | FBX conversion pipeline | Done via browser |
| Unreal assets | N/A | UASSET → FBX → GLTF | Done via browser |
| AI generation | Limited | Meshy/Tripo API | Done via browser |

**Strategy: Build a unified browser that aggregates all sources + converts Unity/Unreal assets.**

This effectively closes the asset gap — all Unity/Unreal marketplace assets become usable in Godot.

#### Source Tiers

**Tier 1: Direct API (GLTF output)**
- Sketchfab — 3D models
- Poly Pizza — 3D models (free)
- Godot Asset Library — Plugins/assets
- Kenney Assets — CC0 assets
- Quixel Megascans — Scans/textures (via Bridge)

**Tier 2: Conversion Required**
- Unity Asset Store — FBX/prefab → GLTF
- Unreal Marketplace — UASSET → FBX → GLTF (partial manual)
- CGTrader — FBX/OBJ/Blend → GLTF
- TurboSquid — FBX/OBJ/Blend → GLTF

**Tier 3: AI Generation**
- Meshy — text/image → GLTF
- Tripo — text/image → GLTF
- Rodin — text/image → GLTF

#### Architecture

```
┌─────────────────────────────────────────────────────────┐
│           ASSET BROWSER (Godot Plugin)                  │
├─────────────────────────────────────────────────────────┤
│  UI                                                     │
│  ├── Search bar (unified query)                         │
│  ├── Source filters (checkboxes)                        │
│  ├── Type filters (model, texture, animation)           │
│  ├── AI generation tab (text/image input)               │
│  └── Results grid with preview                          │
├─────────────────────────────────────────────────────────┤
│  Source Adapters                                        │
│  ├── Sketchfab API                                      │
│  ├── Poly Pizza API                                     │
│  ├── Godot Asset Library API                            │
│  ├── Kenney Assets (scraped/indexed)                    │
│  ├── Quixel Bridge API                                  │
│  ├── Meshy API (AI)                                     │
│  └── Tripo API (AI)                                     │
├─────────────────────────────────────────────────────────┤
│  Format Converter (GDExtension)                         │
│  ├── FBX → GLTF (fbx2gltf binary)                       │
│  ├── OBJ → GLTF (assimp)                                │
│  ├── USD → GLTF                                         │
│  ├── Blend → GLTF (Blender headless)                    │
│  ├── UASSET → FBX (umodel/UE Viewer, manual step)       │
│  └── Texture optimizer (basisu/ktx)                     │
├─────────────────────────────────────────────────────────┤
│  Import Pipeline                                        │
│  ├── Download manager (async, resume)                   │
│  ├── Asset organizer (folder structure)                 │
│  ├── Thumbnail generator                                │
│  └── Godot resource importer (.import)                  │
└─────────────────────────────────────────────────────────┘
```

#### Conversion Details

| Format | Tool | Automation |
|--------|------|------------|
| FBX | fbx2gltf (Facebook) | Full auto |
| OBJ | assimp | Full auto |
| USD | USD → GLTF converter | Full auto |
| Blend | Blender --python headless | Full auto |
| UASSET | umodel (UE Viewer) | Partial (manual export from UE) |

**Unity assets:** .unitypackage is a tar archive. Extract → FBX → convert. Full automation possible.

**Unreal assets:** .uasset requires umodel to extract. Marketplace assets must be downloaded manually from Epic Launcher, then exported. Not fully automatable, but browser can guide the workflow.

#### Implementation Phases

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| Phase 1: Core browser | 2-3 weeks | UI + Sketchfab/Poly Pizza/Godot API + GLTF import |
| Phase 2: Conversion | 1-2 weeks | FBX/OBJ/USD/Blend → GLTF pipeline |
| Phase 3: AI generation | 1-2 weeks | Meshy/Tripo integration |
| Phase 4: Polish | 1-2 weeks | Thumbnails, organization, caching |

**Total: 5-9 weeks** (or 2-4 weeks AI-assisted at current pace)

#### What This Closes

| Gap | Before | After |
|-----|--------|-------|
| Free assets | Manual download/import | One-click from browser |
| Unity assets | Manual FBX export/import | Auto-convert in browser |
| Unreal assets | Manual UASSET extraction | Guided workflow + convert |
| AI generation | External site → download → import | Generate directly in Godot |
| Asset discovery | Multiple sites, multiple accounts | Unified search |

**Result: Godot gains access to Unity + Unreal + free + AI asset ecosystems.**

## What Blender Covers

| Need | Blender Covers It? | Limitation |
|------|-------------------|------------|
| Cinematics | Yes | Render to video, not in-engine real-time |
| Animation authoring | Yes | Export via GLTF |
| Rigging/IK | Yes | Export to Godot |
| Cloth simulation | Yes | Bake to vertex cache, not runtime |
| Fluid simulation | Yes | Bake to cache, not runtime |
| Asset creation | Yes | Manual workflow |

**Blender is the authoring solution. Godot is the runtime.**

## What Needs to Be Built

### Tier 1: Critical

1. **Dense geometry renderer** — Project Meridian (in progress)
   - Status: Mid Phase 1, ~60% builder done, ~5% renderer done
   - Est. remaining: 1-2 months at AI-assisted pace
   
2. **Destruction physics** — Jolt extension with structural stress
   - Est. time: 2-4 weeks
   
3. **Real-time GI** — Lumen equivalent
   - Est. time: 2-4 months (DDGI/probe-based approach)

### Tier 2: Important

4. **Squad/army AI** — Behavior trees, crowd handling for 50-500 units
   - Est. time: 1.5-3 months
   
5. **Vehicle physics** — Custom vehicle controller with destruction integration
   - Est. time: 2-4 weeks
   
6. **Mech systems** — Animation blending, IK for mech locomotion
   - Est. time: 2-4 weeks

### Tier 3: Polish

7. **Cloth physics** — Jolt extension (NvCloth or PBD)
   - Est. time: 1-2 months
   
8. **Fluid simulation** — SPH solver
   - Est. time: 2-4 months (optional)
   
9. **In-engine cinematics** — Or continue with Blender pipeline
   - Est. time: 2-4 months (optional)

### Tier 4: Ecosystem

10. **Unified Asset Browser** — Aggregate + convert all sources
    - Est. time: 5-9 weeks
    
11. MetaHuman equivalent — Use Reallusion/Blender (external)
12. Megascans integration — Via unified browser

## Architecture Strategy

```
┌─────────────────────────────────────────────────────────┐
│                    GAME RUNTIME                         │
├─────────────────────────────────────────────────────────┤
│  Godot Engine (core)                                    │
│  ├── GDScript (gameplay, UI, logic)                     │
│  └── Forward+ renderer (base)                           │
├─────────────────────────────────────────────────────────┤
│  GDExtensions (performance-critical)                    │
│  ├── Project Meridian (dense geometry, Nanite-like)     │
│  ├── Jolt Physics Extension                             │
│  │   ├── Destruction (fracture + structural stress)     │
│  │   ├── Cloth (NvCloth wrapper or PBD solver)          │
│  │   └── Fluid (SPH solver, shares collision)           │
│  ├── Army AI (crowd simulation, culling LOD)            │
│  └── Format Converter (FBX/OBJ/USD/Blend → GLTF)        │
├─────────────────────────────────────────────────────────┤
│  Engine Module (if needed)                              │
│  └── Lumen equivalent (GI) — requires renderer access   │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                   ASSET ECOSYSTEM                       │
├─────────────────────────────────────────────────────────┤
│  Unified Asset Browser (Godot Plugin)                   │
│  ├── Source adapters (Sketchfab, Poly Pizza, Quixel)    │
│  ├── AI generation (Meshy, Tripo, Rodin)                │
│  ├── Format conversion (FBX/OBJ/USD/Blend → GLTF)       │
│  └── One-click import to project                        │
├─────────────────────────────────────────────────────────┤
│  External Sources                                       │
│  ├── Unity Asset Store (via conversion)                 │
│  ├── Unreal Marketplace (via conversion)                │
│  └── All free asset sites (unified search)              │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                   AUTHORING PIPELINE                    │
├─────────────────────────────────────────────────────────┤
│  Blender                                                │
│  ├── Cinematics (render to video)                       │
│  ├── Animation/rigging (export GLTF)                    │
│  ├── Cloth/fluid simulation (bake to cache)             │
│  └── Asset creation                                     │
├─────────────────────────────────────────────────────────┤
│  AI Tools                                               │
│  ├── Meshy/Tripo (3D asset generation)                  │
│  ├── Claude + Godot MCP (scene editing, code)           │
│  └── Custom agents (structural decomposition)           │
└─────────────────────────────────────────────────────────┘
```

## Prioritized Build Order

### Phase 1: Foundation (1-2 months)
**Project Meridian**
- Dense geometry renderer
- GPU-driven culling, visibility buffer
- Streaming and residency
- Shadow integration

### Phase 2: Physics (0.5-1.5 months)
**Jolt Extensions**
- Destruction system (2-4 weeks)
  - Voronoi fracture preprocessing
  - Structural stress simulation
  - Runtime break/spawn logic
- Cloth integration (1-2 months, optional)
- Fluid SPH (2-4 months, optional, defer)

### Phase 3: Combat Scale (1.5-3 months)
**AI Systems**
- Squad AI (Kenshi-style job system)
- Army AI (50-500 units with LOD)
- Behavior trees
- Crowd simulation
- Vehicle physics (2-4 weeks)

### Phase 4: Visual Quality (2-4 months)
**GI Solution**
- Real-time GI (Lumen equivalent)
  - DDGI (probe-based) as starting point
  - Integration with Meridian renderer
- Shadow improvements
- Atmospheric effects

### Phase 5: Polish (1-3 months)
- Mech systems
- Cloth (if not done in Phase 2)
- Cinematic tools (or commit to Blender pipeline)

## Time Savings from Jolt Extension

| Component | Original Est. | With Jolt Extension | Savings |
|-----------|---------------|---------------------|---------|
| Destruction | 1-3 months | 2-4 weeks | 50-75% |
| Cloth | 2-4 months | 1-2 months | 50% |
| Fluid | 3-6 months | 2-4 months | 33% |
| **Physics Total** | **3.5-7 months** | **1.5-4 months** | **~50%** |

## Total Timeline Estimates

### Full Stack (Everything)

| Track | Duration | Parallel? |
|-------|----------|-----------|
| Rendering (Meridian + Lumen + shadows) | 4-8 months | Can overlap with physics |
| Physics (destruction + cloth + fluid) | 1.5-4 months | Can overlap with rendering |
| AI/Navigation | 1.5-3 months | Can overlap |
| Asset Browser | 5-9 weeks | Can overlap |
| **Total Sequential** | **8-17 months** | |
| **Total Parallel** | **5-10 months** | With context switching |

### MVP Subset (Recommended)

| Component | Time |
|-----------|------|
| Meridian renderer | 1-2 months |
| Destruction (Jolt) | 2-4 weeks |
| Crowd AI | 1-2 months |
| Unified Asset Browser | 5-9 weeks |
| **MVP Total** | **3-6 months** |

**Skip for MVP:** Lumen, cloth, fluid, MetaHuman equivalent

## What We're Not Building

| Category | Strategy |
|----------|----------|
| Custom marketplace store | Unified browser aggregates existing stores |
| MetaHuman equivalent | Use Reallusion iClone + Blender |
| Blueprint equivalent | GDScript + AI codegen is better in 2026 |
| Full Unreal parity | Not needed |
| Own asset hosting | Link to external sources |

## Success Criteria

Godot + this stack is competitive when:

1. Dense static scenes render at 60fps on target hardware
2. Destruction feels satisfying (Red Faction tier)
3. Army battles (200+ units) run without tanking
4. Lighting doesn't look last-gen (GI solution exists)
5. AI can manage squads and crowds intelligently
6. Cinematics exist (via Blender)
7. Any Unity/Unreal/free asset is one click away (unified browser)

## Open Questions

- Lumen equivalent: build or accept baked lighting for v1?
- Cloth: NvCloth wrapper or custom PBD solver?
- Fluid: defer entirely for MVP?
- How much army AI can be faked with officer-based LOD vs true crowd simulation?
- Jolt extension: GDExtension or contribute upstream to godot-jolt?
- Asset browser: include Unity/Unreal store scraping or require manual download?
- Asset browser: build web frontend or editor-only?

## Related Docs

- [PROJECT_PLAN.md](PROJECT_PLAN.md) — Project Meridian plan
- [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md) — Renderer technical spec
- [FRONTIER_OPPORTUNITIES.md](FRONTIER_OPPORTUNITIES.md) — Ways to beat Nanite
- [COMPETITIVE_PLAN.md](COMPETITIVE_PLAN.md) — Competition analysis
- [lumen_gdExtension/PLAN.md](lumen_gdExtension/PLAN.md) — GI research plan

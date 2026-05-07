# Architecture Decisions

Last updated: 2026-03-23

This file records the current architectural decisions for Project Meridian.

## ADR-001: The project target is competitive dense geometry, not full Nanite parity

Status:

- accepted

Decision:

- target Tier 1 competitiveness first: static opaque high-detail geometry on desktop

Reason:

- this is the portion of the problem that is both strategically valuable and realistically achievable first

Implication:

- do not design v1 around skeletal, translucent, VR, or full material parity requirements

## ADR-002: The portable core is compute-first

Status:

- accepted

Decision:

- the baseline renderer path uses compute culling and indirect execution

Reason:

- mesh shaders are not yet a documented stock Godot runtime capability
- compute fallback is mandatory anyway
- compute-first keeps the architecture portable and debuggable

Implication:

- mesh shaders remain a later acceleration path

## ADR-003: Streaming is a core subsystem

Status:

- accepted

Decision:

- page streaming and residency are part of the first real renderer architecture

Reason:

- dense geometry is not competitive without bounded memory and on-demand residency

Implication:

- page format, scheduler inputs, and async loading must be designed before runtime integration is considered complete

## ADR-004: The system is hybrid by design

Status:

- accepted

Decision:

- the long-term architecture supports more than one geometry representation

Baseline representation:

- clustered explicit geometry

Later representations:

- foliage / aggregate geometry specializations
- procedural resurfacing
- compressed geometry paths

Reason:

- one representation is unlikely to beat Nanite in the places where Nanite is weakest

## ADR-005: Delivery vehicle is hybrid until proven otherwise

Status:

- accepted

Decision:

- use GDExtension for tooling and importer work, but assume the production runtime path may require an engine module or fork

Reason:

- current public renderer hooks do not clearly imply full opaque-pass ownership

Implication:

- architecture and milestones must not depend on a pure extension-only runtime story

## ADR-006: Material scope is intentionally narrow in v1

Status:

- accepted

Decision:

- support a constrained opaque PBR subset first

Reason:

- material parity can consume the entire project if left unconstrained

Implication:

- broad `ShaderMaterial` parity is deferred

## ADR-007: Shadows are part of the minimum viable renderer

Status:

- accepted

Decision:

- directional shadow support is a core milestone, not a polish task

Reason:

- dense geometry without competitive shadow behavior will not feel production-ready

## ADR-008: All progress is measured against benchmark scenes

Status:

- accepted

Decision:

- every major milestone must report results on a fixed benchmark suite

Reason:

- the project exists to outperform a real baseline, not to produce isolated demos

## ADR-009: Godot runtime integration path

Status:

- accepted (with Phase 0 validation gate)

Decision:

- primary path: dual-render via CompositorEffect + GDExtension
- fallback: minimal engine patch to add opaque pass replacement hook

Reason:

- CompositorEffect allows running custom compute and rendering before or after the opaque pass
- dual-render approach: Meridian renders dense geometry via CompositorEffect (before opaque pass), writes to depth buffer; Godot's Forward+ renders standard geometry, skipping occluded pixels via depth test
- this avoids forking Godot entirely — zero rebase cost
- Godot's PR backlog (~5K) makes upstream merge unrealistic; maintaining a deep fork creates permanent maintenance tax
- if CompositorEffect cannot write depth that Forward+ respects, a minimal engine patch (~100-500 lines) adds an opaque pass replacement hook — small, contained, rebeasable

Critical constraint discovered:

- Godot docs state the opaque pass "needs to be left as is" — CompositorEffect runs before/after, not instead of
- dual-render is viable if depth integration works (dense geometry writes depth, Forward+ respects it)
- Phase 0 MUST validate: can a CompositorEffect write to the depth buffer before the opaque pass, and will Forward+ skip occluded standard geometry?

Implication:

- Phase 0 adds a specific test: CompositorEffect depth writing feasibility
- if dual-render works, the entire project ships as GDExtension — no fork
- if it doesn't, the engine patch is small and contained (add one render hook)
- Lighthugger (MIT, Vulkan meshlet + visibility buffer) is the architectural reference for the standalone renderer; the integration question is how to bridge it into Godot's pipeline

## ADR-010: Lighthugger as primary architectural reference

Status:

- accepted

Decision:

- use Lighthugger as the primary reference implementation for Meridian's renderer architecture

Reason:

- MIT license (available on request), C++20, Vulkan-Hpp with GLSL shaders
- implements the exact architecture Meridian needs: meshlet culling via compute (emulating mesh shaders), visibility buffer, single-pass lighting resolve, cascaded shadow maps
- proves the compute-first approach works without mesh shader hardware support
- uses meshoptimizer for meshlet generation (same as Meridian)

Implication:

- study Lighthugger's compute culling, visibility buffer encoding, and material resolve patterns
- Meridian's Phase 2 standalone renderer can be modeled closely on Lighthugger's architecture
- adapt for Godot's material system and RenderingDevice API rather than raw Vulkan-Hpp

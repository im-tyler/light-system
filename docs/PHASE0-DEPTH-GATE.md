# PHASE0-DEPTH-GATE — resolved: dual-render is viable

ADR-009's critical constraint ("can a CompositorEffect write depth that
Forward+ respects?") was resolved by source reading on 2026-09-04
(NVIDIA RTX fork's forward_clustered.cpp — upstream Forward+ logic,
~4.4 era; verify line-for-line on 4.6 when the smoke test runs).

## The proof chain

1. `rendering/driver/depth_prepass/enable` defaults TRUE
   (rendering_server.cpp: `GLOBAL_DEF_RST(...depth_prepass/enable, true)`).
2. The depth prepass runs, THEN pre-opaque CompositorEffects
   (`_process_compositor_effects(PRE_OPAQUE, ...)`, after the prepass
   block, before the opaque pass).
3. The opaque pass issues:
   `(depth_pre_pass ? DRAW_DEFAULT_ALL : DRAW_CLEAR_DEPTH)`
   -> with prepass on, depth is LOADED, not cleared.
4. Therefore effect-written depth survives and Godot's opaque geometry
   depth-tests against it. Occluded fragments are rejected.

## The design that follows (no load_color hack needed)

- PRE-OPAQUE effect: DEPTH-ONLY draw (meridian dense geometry).
  Reversed-Z convention (Godot uses GREATER compare); projection from
  RenderDataRD. Kills Godot's hidden geometry for free.
- POST-OPAQUE effect: COLOR draw, depth-tested against Godot's combined
  depth — meridian renders only where it wins visibility.
- load_color is only set for BG_CANVAS/BG_KEEP; avoid needing it.
- MSAA: write into the same MSAA depth attachment; RESOLVED-depth
  access is not available pre-opaque (engine warns) — not needed here.

## Remaining work (correctness, not feasibility)

- One-line verification of the same draw-flags logic on Godot 4.6.
- Smoke test project: pre-opaque depth-only boxes + Godot meshes behind
  them -> confirm occlusion; exercise reversed-Z math + MSAA on/off.

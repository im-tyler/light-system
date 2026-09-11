# Open audit items

Register LS-01 through LS-14 (scout pass 1, 2026-09-11) is fully fixed and verified - see git history for the `audit LS-NN` commits. What remains open is verification scope, not known defects:

- **Per-frame occlusion paths are compile-verified only.** The drawIndirectCount-path findings (LS-03/05/13 fixes, temporal HZB dispatch) cannot execute on MoltenVK (per-frame refine is epilogue-only there). First run on a non-MoltenVK device should confirm: HZB edge coverage, footprint-max queries, temporal invalidation dispatch, survivor order stability.
- **Pass 1 is one scout pass, not an exhaustive audit.** Coverage: builder (cluster/serialize/traversal), streaming (scheduler/async reader/residency), HZB/occlusion, render/bootstrap, runtime model, shaders. Follow-up passes were planned (cumulative expansion protocol) but blocked by tooling; treat unlisted subsystems (e.g. trickplay/texturing internals beyond what pass 1 reached, tools/) as unaudited.
- **Known accepted behavior, documented in code:** LOD provenance merges (LS-01 fix) trade bounded over-emit for the whole-unit selection invariant; merged-record geometric_error is the max member error.

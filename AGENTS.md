# AGENTS.md

Open audit items, deferred findings and their reasons live in `AUDIT_OPEN.md` at the repo root (2026-09-09→12 audit sweep). Read it before treating related work as done; update it when you close, defer, or upstream-report an item.

This repository is **public** and mirrored to GitHub at https://github.com/im-tyler/light-system.
It is a standalone Vulkan renderer (formerly "Project Meridian," the renderer
subsystem of the retired Godot-parity umbrella; docs/ADRs still use the old name).
Every commit is publicly visible — treat all work as public-facing.

## Do not commit private/transient context
Never create or commit session, handoff, or local-only context as tracked files:
- No `SESSION_NOTES.md`, `*_NOTES.md`, `HANDOFF*.md`, scratch/status docs
- No `.claude/`, `.opencode/` local configs
- No secrets, no internal network addresses (Tailscale IPs, internal hostnames)
- `notes/` is local-only working space: gitignored as a safety net, but do not
  create tracked variants. Design docs that belong in public live in `docs/`.

## Commits
- Conventional style (`feat:`, `fix:`, `chore:`, `docs:`).
- History is public — keep it clean.

## Remotes
`git push origin` fans out to both Forgejo and GitHub (dual push-URL) — a single push mirrors to both.

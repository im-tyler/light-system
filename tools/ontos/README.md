# ontos_stream_dump

Third independent implementation of the ontos stream spec (normative: ontos repo, `docs/STREAM_SPEC.md`) — Rust sim (ontos), Python verifier (simval), this C++17 std-only re-simulator. Verifies both format versions: v1 life (Phase 0) and v2 gravity (Phase 2: leapfrog + Chebyshev ephemeris windows + momentum ledger, re-simulated bit-exactly with bounded drift reported against an all-fine reference). Spike toward the ontos Phase 3 viewer bridge: JoltViewer thin-consumer pattern, zero coupling to the renderer core.

Build: `clang++ -std=c++17 -O2 -ffp-contract=off -o ontos_stream_dump ontos_stream_dump.cpp`

`-ffp-contract=off` is required: the spec bans FMA (determinism section), and clang contracts `a*b+c` into FMA by default at -O2, which breaks bit-exactness on the first tick.

Usage: `./ontos_stream_dump <stream-file> <seed>` (example streams + seeds: simval `examples/ontos*/`)

Exit codes: 0 = all records verified · 1 = verification mismatch (first bad record printed) · 2 = bad stream (magic/version/world size/truncation/unknown tag) · 3 = usage, I/O, or FNV self-check failure.

# ontos_stream_dump

Third independent implementation of the ontos Phase-0 stream spec (normative: ontos repo, `docs/STREAM_SPEC.md`) — Rust sim (ontos), Python verifier (simval), this C++17 std-only re-simulator. Spike toward the ontos Phase 3 viewer bridge: JoltViewer thin-consumer pattern, zero coupling to the renderer core.

Build: `clang++ -std=c++17 -O2 -o ontos_stream_dump ontos_stream_dump.cpp`

Usage: `./ontos_stream_dump <stream-file> <seed>` (example streams + seeds: simval `examples/ontos/*/`)

Exit codes: 0 = all records verified · 1 = verification mismatch (first bad record printed) · 2 = bad stream (magic/version/world size/truncation/unknown tag) · 3 = usage, I/O, or FNV self-check failure.

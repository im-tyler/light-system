#!/usr/bin/env bash
# light-system benchmark harness: standalone Meridian Vulkan renderer vs
# stock Godot Forward+ on shared scenes, shared window size, shared
# bounds-derived camera framing.
#
# Usage:
#   ./run_bench.sh                     # dragon + city, 1 run each
#   SCENES="stanford_dragon" ./run_bench.sh
#   REPEAT=3 FRAMES=237 ./run_bench.sh
#
# Environment overrides:
#   GODOT_BIN     path to stock Godot binary (default /tmp/godot-bench/...)
#   RENDERER_BIN  path to meridian_vk_bootstrap (default repo prototype build)
#   SCENES        space-separated scene ids (default "stanford_dragon massive_city")
#   FRAMES        measured frames per run (default 117, matches renderer 120-3)
#   WARMUP        Godot warmup frames (default 30)
#   REPEAT        runs per engine per scene (default 1; summary uses median)
#   GODOT_WARMUP  alias for WARMUP
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROJECT_DIR="$SCRIPT_DIR/project"

GODOT_BIN="${GODOT_BIN:-/tmp/godot-bench/Godot.app/Contents/MacOS/Godot}"
RENDERER_BIN="${RENDERER_BIN:-$REPO_ROOT/prototype/build/meridian_vk_bootstrap}"
SCENES="${SCENES:-stanford_dragon massive_city}"
FRAMES="${FRAMES:-117}"
WARMUP="${GODOT_WARMUP:-${WARMUP:-30}}"
REPEAT="${REPEAT:-1}"
WIDTH=1280
HEIGHT=720

RESULTS_DIR="$SCRIPT_DIR/results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULTS_DIR"

fail() {
    echo "error: $*" >&2
    exit 1
}

[ -x "$GODOT_BIN" ] || fail "Godot binary not found/executable: $GODOT_BIN (set GODOT_BIN)"
[ -x "$RENDERER_BIN" ] || fail "renderer binary not found/executable: $RENDERER_BIN (set RENDERER_BIN)"

renderer_manifest() {
    case "$1" in
        stanford_dragon) echo "$REPO_ROOT/benchmarks/scenes/external/dragon_manifest.txt" ;;
        massive_city) echo "$REPO_ROOT/benchmarks/scenes/generated/city_manifest.txt" ;;
        *) return 1 ;;
    esac
}

renderer_extra_args() {
    # The renderer's default LOD threshold is scene-scaled (auto) since
    # 2026-09-07; no per-scene override is needed. Set an explicit
    # --error-threshold here to pin a specific detail level instead.
    case "$1" in
        *) echo ""
    esac
}

godot_asset() {
    case "$1" in
        stanford_dragon) echo "res://assets/dragon.obj" ;;
        massive_city) echo "$REPO_ROOT/benchmarks/scenes/generated/massive_city.glb" ;;
        *) return 1 ;;
    esac
}

stage_assets() {
    mkdir -p "$PROJECT_DIR/assets"
    if [ ! -f "$PROJECT_DIR/assets/dragon.obj" ] || ! cmp -s \
        "$REPO_ROOT/benchmarks/assets/dragon/dragon.obj" "$PROJECT_DIR/assets/dragon.obj"; then
        echo "staging dragon.obj into project assets (clone copy)..."
        cp -c "$REPO_ROOT/benchmarks/assets/dragon/dragon.obj" \
            "$PROJECT_DIR/assets/dragon.obj" 2>/dev/null ||
            cp "$REPO_ROOT/benchmarks/assets/dragon/dragon.obj" "$PROJECT_DIR/assets/dragon.obj"
    fi
}

extract_field() {
    # extract_field <key> <line with key=float>
    printf '%s\n' "$2" | sed -n "s/.*[[:space:]]$1=\([0-9.]*\).*/\1/p"
}

median_of() {
    # median_of <space-separated numbers>
    printf '%s\n' $1 | sort -g | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

echo "== light-system benchmark harness =="
echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "host:        $(scutil --get ComputerName 2>/dev/null || hostname) ($(sysctl -n machdep.cpu.brand_string 2>/dev/null))"
echo "os:          $(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null)"
echo "godot:       $("$GODOT_BIN" --version)"
echo "renderer:    $RENDERER_BIN"
echo "resolution:  ${WIDTH}x${HEIGHT}, frames=$FRAMES, warmup(godot)=$WARMUP, repeat=$REPEAT"
echo "results dir: $RESULTS_DIR"
echo

for scene in $SCENES; do
    manifest="$(renderer_manifest "$scene")" || fail "unknown scene: $scene"
    asset="$(godot_asset "$scene")"
    [ -f "$manifest" ] || fail "manifest missing: $manifest"
    [ -f "$asset" ] || [ "${asset#res://}" != "$asset" ] || fail "asset missing: $asset"
done

stage_assets

# One-time CLI import so res://assets/dragon.obj resolves at runtime.
echo "importing godot project assets (headless, cached)..."
"$GODOT_BIN" --headless --import --path "$PROJECT_DIR" >"$RESULTS_DIR/import.log" 2>&1 ||
    fail "godot --import failed, see $RESULTS_DIR/import.log"

ROWS_FILE="$RESULTS_DIR/rows.txt"
: >"$ROWS_FILE"

for scene in $SCENES; do
    manifest="$(renderer_manifest "$scene")"
    asset="$(godot_asset "$scene")"
    extra="$(renderer_extra_args "$scene")"

    for run in $(seq 1 "$REPEAT"); do
        echo "-- [$scene] renderer run $run/$REPEAT"
        # shellcheck disable=SC2086
        "$RENDERER_BIN" --manifest "$manifest" $extra \
            >"$RESULTS_DIR/renderer_${scene}_r${run}.stdout" 2>"$RESULTS_DIR/renderer_${scene}_r${run}.log"
        mer_line="$(grep -h "MERIDIAN_BENCHMARK:" "$RESULTS_DIR/renderer_${scene}_r${run}.log" | tail -1)"
        [ -n "$mer_line" ] || fail "no MERIDIAN_BENCHMARK line, see $RESULTS_DIR/renderer_${scene}_r${run}.log"
        echo "   $mer_line"
        r_med="$(extract_field median_ms "$mer_line")"
        r_avg="$(extract_field avg_ms "$mer_line")"
        r_p99="$(extract_field p99_ms "$mer_line")"
        r_fps="$(extract_field avg_fps "$mer_line")"
        printf 'renderer %s %s %s %s %s %s %s\n' "$scene" "$run" "$r_med" "$r_avg" "$r_p99" "$r_fps" "$FRAMES" >>"$ROWS_FILE"

        echo "-- [$scene] godot run $run/$REPEAT (brief window)"
        "$GODOT_BIN" --path "$PROJECT_DIR" \
            --resolution "${WIDTH}x${HEIGHT}" --rendering-driver vulkan --rendering-method forward_plus \
            -- --scene="$asset" --scene_id="$scene" --frames="$FRAMES" --warmup="$WARMUP" \
            --out="$RESULTS_DIR/godot_${scene}_r${run}.json" \
            >"$RESULTS_DIR/godot_${scene}_r${run}.log" 2>&1
        god_line="$(grep -h "GODOT_BENCHMARK:" "$RESULTS_DIR/godot_${scene}_r${run}.log" | tail -1)"
        [ -n "$god_line" ] || fail "no GODOT_BENCHMARK line, see $RESULTS_DIR/godot_${scene}_r${run}.log"
        echo "   $god_line"
        g_med="$(extract_field median_ms "$god_line")"
        g_avg="$(extract_field avg_ms "$god_line")"
        g_p99="$(extract_field p99_ms "$god_line")"
        g_fps="$(extract_field avg_fps "$god_line")"
        printf 'godot %s %s %s %s %s %s %s\n' "$scene" "$run" "$g_med" "$g_avg" "$g_p99" "$g_fps" "$FRAMES" >>"$ROWS_FILE"
    done
done

moltenvk="$(grep -h "Using Device" "$RESULTS_DIR"/godot_*.log | head -1 | sed 's/^.*Using/using/' | tr -s ' ' | cut -c1-90)"
echo
echo "== environment =="
if [ -n "$moltenvk" ]; then
    echo "godot gpu:   $moltenvk"
fi

echo
echo "== per-run results (ms per frame; lower is better) =="
printf '%-9s %-16s %4s %10s %10s %10s %9s\n' "engine" "scene" "run" "median_ms" "avg_ms" "p99_ms" "avg_fps"
while read -r engine scene run med avg p99 fps frames; do
    printf '%-9s %-16s %4s %10s %10s %10s %9s\n' "$engine" "$scene" "$run" "$med" "$avg" "$p99" "$fps"
done <"$ROWS_FILE"

echo
echo "== comparison (median of runs) =="
printf '%-16s %12s %12s %10s %14s\n' "scene" "renderer_ms" "godot_ms" "ratio" "fps_rr_over_g"
for scene in $SCENES; do
    r_meds=""
    g_meds=""
    r_fpsvals=""
    g_fpsvals=""
    while read -r engine s run med avg p99 fps frames; do
        [ "$s" = "$scene" ] || continue
        if [ "$engine" = "renderer" ]; then
            r_meds="$r_meds $med"
            r_fpsvals="$r_fpsvals $fps"
        else
            g_meds="$g_meds $med"
            g_fpsvals="$g_fpsvals $fps"
        fi
    done <"$ROWS_FILE"
    r_median="$(median_of "$r_meds")"
    g_median="$(median_of "$g_meds")"
    r_fps="$(median_of "$r_fpsvals")"
    g_fps="$(median_of "$g_fpsvals")"
    ratio="$(awk -v a="$g_median" -v b="$r_median" 'BEGIN {if (b > 0) printf "%.2f", a / b; else printf "n/a"}')"
    speedup="$(awk -v a="$r_fps" -v b="$g_fps" 'BEGIN {if (b > 0) printf "%.2fx", a / b; else printf "n/a"}')"
    printf '%-16s %12s %12s %10s %14s\n' "$scene" "$r_median" "$g_median" "$ratio" "$speedup"
done
echo
echo "ratio = godot_ms / renderer_ms (>1 means the renderer is faster)"
echo "fps_rr_over_g = renderer avg fps / godot avg fps (<1 means the renderer is slower)"
echo "note: godot on macOS/MoltenVK is floored at the display refresh (60 Hz)"
echo "unless its frame time exceeds 16.7 ms; capped runs read as <=16.7 ms."
echo "raw logs and json in: $RESULTS_DIR"

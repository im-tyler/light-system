#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def parse_key_values(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    return result


def parse_material_slots(value: str) -> list[str]:
    slots = [item.strip() for item in value.split(",") if item.strip()]
    if not slots:
        raise SystemExit("at least one material slot is required")
    return slots


def write_manifest(
    manifest_path: Path, asset_id: str, asset_path: Path, material_slots: list[str]
) -> None:
    lines = [
        f"asset_id = {asset_id}",
        f"source_asset = {asset_path}",
        f"output_path = ../../../build/{asset_id}.vgeo",
        "emit_fallback = true",
        f"material_slots = {', '.join(material_slots)}",
        "cluster_vertex_limit = 24",
        "cluster_triangle_limit = 24",
        "page_cluster_limit = 6",
        "hierarchy_partition_size = 4",
        "bounds_padding = 0.25",
    ]
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_replay_script(
    replay_path: Path,
    asset_id: str,
    error_thresholds: list[str],
    resident_budget: int,
    eviction_grace: int,
    bootstrap_resident: str,
) -> None:
    lines = [
        f"name = {asset_id}_import_validation",
        f"frame_count = {len(error_thresholds)}",
        f"resident_budget = {resident_budget}",
        f"eviction_grace_frames = {eviction_grace}",
        f"bootstrap_resident = {bootstrap_resident}",
        f"error_thresholds = {', '.join(error_thresholds)}",
    ]
    replay_path.parent.mkdir(parents=True, exist_ok=True)
    replay_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_report(
    report_path: Path,
    asset_id: str,
    asset_path: Path,
    manifest_path: Path,
    replay_path: Path,
    summary: dict[str, str],
    replay: dict[str, str],
) -> None:
    lines = [
        "# Imported Asset Validation",
        "",
        f"- Asset ID: {asset_id}",
        f"- Source asset: {asset_path}",
        f"- Manifest: {manifest_path}",
        f"- Replay script: {replay_path}",
        "",
        "## Builder Summary",
        "",
        f"- Source vertices: {summary.get('source_vertices', 'unknown')}",
        f"- Source triangles: {summary.get('source_triangles', 'unknown')}",
        f"- Seam locked vertices: {summary.get('seam_locked_vertices', 'unknown')}",
        f"- Clusters: {summary.get('clusters', 'unknown')}",
        f"- Pages: {summary.get('pages', 'unknown')}",
        f"- LOD groups: {summary.get('lod_groups', 'unknown')}",
        f"- LOD clusters: {summary.get('lod_clusters', 'unknown')}",
        f"- Page dependencies: {summary.get('page_dependencies', 'unknown')}",
        "",
        "## Replay Final State",
        "",
        f"- Selected nodes: {replay.get('selected_nodes', 'unknown')}",
        f"- Selected pages: {replay.get('selected_pages', 'unknown')}",
        f"- Selected clusters: {replay.get('selected_clusters', 'unknown')}",
        f"- Selected LOD groups: {replay.get('selected_lod_groups', 'unknown')}",
        f"- Selected LOD clusters: {replay.get('selected_lod_clusters', 'unknown')}",
        f"- Missing pages: {replay.get('missing_pages', 'unknown')}",
        f"- Prefetch pages: {replay.get('prefetch_pages', 'unknown')}",
        "",
        "## Notes",
        "",
        "- This report is produced by the deterministic import-validation workflow.",
        "- Review the generated `.summary.txt` and replay output if any count looks suspicious.",
    ]
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create and run a deterministic imported-asset validation workflow"
    )
    parser.add_argument(
        "--asset", required=True, help="Path to source asset (.gltf/.glb/.obj)"
    )
    parser.add_argument(
        "--asset-id", required=True, help="Stable asset id for generated outputs"
    )
    parser.add_argument(
        "--material-slots", required=True, help="Comma-separated material slots"
    )
    parser.add_argument(
        "--error-thresholds",
        default="2.5,2.5,2.5,2.5",
        help="Comma-separated replay thresholds",
    )
    parser.add_argument("--resident-budget", type=int, default=12)
    parser.add_argument("--eviction-grace", type=int, default=1)
    parser.add_argument("--bootstrap-resident", default="none")
    parser.add_argument("--no-run", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    asset_path = Path(args.asset).expanduser().resolve()
    if not asset_path.exists():
        raise SystemExit(f"asset does not exist: {asset_path}")

    material_slots = parse_material_slots(args.material_slots)
    error_thresholds = [
        item.strip() for item in args.error_thresholds.split(",") if item.strip()
    ]
    if not error_thresholds:
        raise SystemExit("at least one error threshold is required")

    manifest_path = (
        repo_root
        / "benchmarks"
        / "scenes"
        / "external"
        / f"{args.asset_id}_manifest.txt"
    )
    replay_path = (
        repo_root
        / "benchmarks"
        / "replays"
        / "external"
        / f"{args.asset_id}_replay.txt"
    )
    report_path = (
        repo_root / "benchmarks" / "results" / f"{args.asset_id}_import_validation.md"
    )
    write_manifest(manifest_path, args.asset_id, asset_path, material_slots)
    write_replay_script(
        replay_path,
        args.asset_id,
        error_thresholds,
        args.resident_budget,
        args.eviction_grace,
        args.bootstrap_resident,
    )

    print(f"manifest={manifest_path}")
    print(f"replay_script={replay_path}")

    if args.no_run:
        return 0

    prototype_dir = repo_root / "prototype"
    builder = prototype_dir / "build" / "meridian_builder"
    replay = prototype_dir / "build" / "meridian_replay"
    if not builder.exists() or not replay.exists():
        raise SystemExit(
            "expected built tools at prototype/build/meridian_builder and prototype/build/meridian_replay"
        )

    subprocess.run(
        [str(builder), "--manifest", str(manifest_path)], cwd=prototype_dir, check=True
    )
    replay_process = subprocess.run(
        [str(replay), "--manifest", str(manifest_path), "--script", str(replay_path)],
        cwd=prototype_dir,
        check=True,
        text=True,
        capture_output=True,
    )
    print(replay_process.stdout, end="")

    summary_path = repo_root / "build" / f"{args.asset_id}.vgeo.summary.txt"
    if not summary_path.exists():
        raise SystemExit(f"expected summary file at {summary_path}")

    summary = parse_key_values(summary_path.read_text(encoding="utf-8"))
    replay_frames = replay_process.stdout.split("frame=")
    replay_final = (
        parse_key_values("frame=" + replay_frames[-1]) if len(replay_frames) > 1 else {}
    )
    write_report(
        report_path,
        args.asset_id,
        asset_path,
        manifest_path,
        replay_path,
        summary,
        replay_final,
    )
    print(f"report={report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


def run(cmd: list[str], cwd: Path) -> str:
    result = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True, check=True)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    return result.stdout


def write_markdown(
    report_path: Path, result: dict[str, object], scene_id: str, asset_path: str
) -> None:
    lines = [
        "# Godot Baseline Result",
        "",
        f"- Scene ID: {scene_id}",
        f"- Asset Path: {asset_path}",
        f"- Average Frame Time: {result.get('avg_frame_ms', 'unknown')}",
        f"- Average FPS: {result.get('avg_fps', 'unknown')}",
        f"- Mesh Instances: {result.get('mesh_instance_count', 'unknown')}",
        f"- Renderer: {result.get('renderer', 'unknown')}",
    ]
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def append_csv(
    csv_path: Path, scene_id: str, result: dict[str, object], resolution: str
) -> None:
    row = [
        "",
        "godot_spike",
        "",
        scene_id,
        "stock_godot",
        "local_macos",
        str(result.get("renderer", "unknown")),
        resolution,
        "cold",
        str(result.get("avg_frame_ms", "")),
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "synthetic spike",
    ]
    with csv_path.open("a", encoding="utf-8") as handle:
        handle.write(",".join(value.replace(",", ";") for value in row) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run stock Godot baseline on a benchmark asset"
    )
    parser.add_argument("--scene-id", required=True)
    parser.add_argument(
        "--asset",
        required=True,
        help="Project-relative asset path, e.g. res://benchmarks/scenes/generated/gltf_block_scene.gltf",
    )
    parser.add_argument("--frames", type=int, default=240)
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--resolution", default="1280x720")
    parser.add_argument("--output-prefix", default="godot_baseline")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    godot = subprocess.run(
        ["godot", "--version"], cwd=repo_root, text=True, capture_output=True
    )
    if godot.returncode != 0:
        raise SystemExit("Godot executable not available")

    output_json = (
        repo_root
        / "benchmarks"
        / "results"
        / f"{args.output_prefix}_{args.scene_id}.json"
    )
    output_md = (
        repo_root
        / "benchmarks"
        / "results"
        / f"{args.output_prefix}_{args.scene_id}.md"
    )
    output_csv = repo_root / "benchmarks" / "results" / "godot_baselines.csv"

    run(["godot", "--path", str(repo_root), "--import", "--quit"], repo_root)
    run(
        [
            "godot",
            "--path",
            str(repo_root),
            "--disable-vsync",
            "--resolution",
            args.resolution,
            "--scene",
            "res://godot/scenes/benchmark_runner.tscn",
            "--",
            f"--asset={args.asset}",
            f"--scene_id={args.scene_id}",
            f"--frames={args.frames}",
            f"--warmup={args.warmup}",
            f"--output={output_json}",
            "--mode=stock_godot",
        ],
        repo_root,
    )

    result = json.loads(output_json.read_text(encoding="utf-8"))
    write_markdown(output_md, result, args.scene_id, args.asset)
    if not output_csv.exists():
        output_csv.write_text(
            (repo_root / "benchmarks" / "results" / "RESULT_TEMPLATE.csv").read_text(
                encoding="utf-8"
            ),
            encoding="utf-8",
        )
    append_csv(output_csv, args.scene_id, result, args.resolution)
    print(f"json={output_json}")
    print(f"markdown={output_md}")
    print(f"csv={output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

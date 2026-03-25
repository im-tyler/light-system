#!/usr/bin/env python3

from __future__ import annotations

import argparse
import math
from pathlib import Path


def vertex_index(x: int, z: int, columns: int) -> int:
    return z * (columns + 1) + x + 1


def height_at(x: int, z: int, columns: int, rows: int) -> float:
    xf = x / max(columns, 1)
    zf = z / max(rows, 1)
    ridge = math.sin(xf * math.pi * 2.3) * 0.45
    swell = math.cos(zf * math.pi * 3.1) * 0.3
    terrace = 0.55 if x > columns // 2 else -0.25
    tilt = (zf - 0.5) * 0.4
    return ridge + swell + terrace + tilt


def emit_obj(
    output_path: Path, columns: int, rows: int, left_material: str, right_material: str
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("o terrace_grid\n")

        for z in range(rows + 1):
            for x in range(columns + 1):
                xpos = float(x) - columns * 0.5
                zpos = float(z) - rows * 0.5
                ypos = height_at(x, z, columns, rows)
                handle.write(f"v {xpos:.6f} {ypos:.6f} {zpos:.6f}\n")

        left_faces = []
        right_faces = []
        split_column = columns // 2

        for z in range(rows):
            for x in range(columns):
                a = vertex_index(x, z, columns)
                b = vertex_index(x + 1, z, columns)
                c = vertex_index(x + 1, z + 1, columns)
                d = vertex_index(x, z + 1, columns)
                target = left_faces if x < split_column else right_faces
                target.append((a, b, c))
                target.append((a, c, d))

        handle.write(f"usemtl {left_material}\n")
        for tri in left_faces:
            handle.write(f"f {tri[0]} {tri[1]} {tri[2]}\n")

        handle.write(f"usemtl {right_material}\n")
        for tri in right_faces:
            handle.write(f"f {tri[0]} {tri[1]} {tri[2]}\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic terrace-grid OBJ benchmark asset"
    )
    parser.add_argument("--output", required=True, help="Output OBJ path")
    parser.add_argument("--columns", type=int, default=36, help="Grid columns")
    parser.add_argument("--rows", type=int, default=28, help="Grid rows")
    parser.add_argument(
        "--left-material", default="rock_master", help="Left-half material name"
    )
    parser.add_argument(
        "--right-material", default="moss_overlay", help="Right-half material name"
    )
    args = parser.parse_args()

    if args.columns < 2 or args.rows < 2:
        raise SystemExit("columns and rows must both be at least 2")
    if args.left_material == args.right_material:
        raise SystemExit("left and right material names must differ")

    emit_obj(
        Path(args.output),
        args.columns,
        args.rows,
        args.left_material,
        args.right_material,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

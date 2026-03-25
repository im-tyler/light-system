#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path


@dataclass
class MeshBuilder:
    vertices: list[tuple[float, float, float]]
    faces_by_material: dict[str, list[tuple[int, int, int]]]

    def add_vertex(self, x: float, y: float, z: float) -> int:
        self.vertices.append((x, y, z))
        return len(self.vertices)

    def add_quad(self, material: str, a: int, b: int, c: int, d: int) -> None:
        self.faces_by_material.setdefault(material, []).append((a, b, c))
        self.faces_by_material.setdefault(material, []).append((a, c, d))


def add_box(
    builder: MeshBuilder,
    material_walls: str,
    material_roof: str,
    x0: float,
    y0: float,
    z0: float,
    x1: float,
    y1: float,
    z1: float,
) -> None:
    v000 = builder.add_vertex(x0, y0, z0)
    v100 = builder.add_vertex(x1, y0, z0)
    v110 = builder.add_vertex(x1, y1, z0)
    v010 = builder.add_vertex(x0, y1, z0)
    v001 = builder.add_vertex(x0, y0, z1)
    v101 = builder.add_vertex(x1, y0, z1)
    v111 = builder.add_vertex(x1, y1, z1)
    v011 = builder.add_vertex(x0, y1, z1)

    builder.add_quad(material_walls, v000, v100, v110, v010)
    builder.add_quad(material_walls, v101, v001, v011, v111)
    builder.add_quad(material_walls, v001, v000, v010, v011)
    builder.add_quad(material_walls, v100, v101, v111, v110)
    builder.add_quad(material_roof, v010, v110, v111, v011)


def emit_arch_block(
    output_path: Path, columns: int, rows: int, wall_material: str, roof_material: str
) -> None:
    builder = MeshBuilder(vertices=[], faces_by_material={})

    for row in range(rows):
        for column in range(columns):
            base_x = column * 2.5
            base_z = row * 2.5
            width = 2.0
            depth = 2.0
            height = 2.5 + ((row + column) % 3) * 1.15
            add_box(
                builder,
                wall_material,
                roof_material,
                base_x,
                0.0,
                base_z,
                base_x + width,
                height,
                base_z + depth,
            )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("o arch_block\n")
        for x, y, z in builder.vertices:
            handle.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        for material_name, faces in builder.faces_by_material.items():
            handle.write(f"usemtl {material_name}\n")
            for a, b, c in faces:
                handle.write(f"f {a} {b} {c}\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic architectural block OBJ benchmark"
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--columns", type=int, default=8)
    parser.add_argument("--rows", type=int, default=6)
    parser.add_argument("--wall-material", default="stone_wall")
    parser.add_argument("--roof-material", default="roof_cap")
    args = parser.parse_args()

    if args.columns < 1 or args.rows < 1:
        raise SystemExit("columns and rows must be positive")
    if args.wall_material == args.roof_material:
        raise SystemExit("wall and roof materials must differ")

    emit_arch_block(
        Path(args.output),
        args.columns,
        args.rows,
        args.wall_material,
        args.roof_material,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

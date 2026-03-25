#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def build_building_mesh() -> tuple[list[float], list[int], list[int]]:
    vertices = [
        0.0,
        0.0,
        0.0,
        2.0,
        0.0,
        0.0,
        2.0,
        2.5,
        0.0,
        0.0,
        2.5,
        0.0,
        0.0,
        0.0,
        2.0,
        2.0,
        0.0,
        2.0,
        2.0,
        2.5,
        2.0,
        0.0,
        2.5,
        2.0,
    ]

    wall_indices = [
        0,
        1,
        2,
        0,
        2,
        3,
        5,
        4,
        7,
        5,
        7,
        6,
        4,
        0,
        3,
        4,
        3,
        7,
        1,
        5,
        6,
        1,
        6,
        2,
    ]
    roof_indices = [3, 2, 6, 3, 6, 7]
    return vertices, wall_indices, roof_indices


def write_buffer(
    output_dir: Path,
    vertices: list[float],
    wall_indices: list[int],
    roof_indices: list[int],
) -> tuple[str, list[dict], list[dict]]:
    buffer_path = output_dir / "building_block.bin"
    vertex_bytes = struct.pack(f"<{len(vertices)}f", *vertices)
    wall_bytes = struct.pack(f"<{len(wall_indices)}I", *wall_indices)
    roof_bytes = struct.pack(f"<{len(roof_indices)}I", *roof_indices)
    blob = vertex_bytes + wall_bytes + roof_bytes
    buffer_path.write_bytes(blob)

    wall_offset = len(vertex_bytes)
    roof_offset = wall_offset + len(wall_bytes)

    buffer_views = [
        {
            "buffer": 0,
            "byteOffset": 0,
            "byteLength": len(vertex_bytes),
            "target": 34962,
        },
        {
            "buffer": 0,
            "byteOffset": wall_offset,
            "byteLength": len(wall_bytes),
            "target": 34963,
        },
        {
            "buffer": 0,
            "byteOffset": roof_offset,
            "byteLength": len(roof_bytes),
            "target": 34963,
        },
    ]
    accessors = [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": len(vertices) // 3,
            "type": "VEC3",
            "min": [0.0, 0.0, 0.0],
            "max": [2.0, 2.5, 2.0],
        },
        {
            "bufferView": 1,
            "componentType": 5125,
            "count": len(wall_indices),
            "type": "SCALAR",
        },
        {
            "bufferView": 2,
            "componentType": 5125,
            "count": len(roof_indices),
            "type": "SCALAR",
        },
    ]
    return buffer_path.name, buffer_views, accessors


def write_gltf(output_path: Path, columns: int, rows: int) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    vertices, wall_indices, roof_indices = build_building_mesh()
    buffer_uri, buffer_views, accessors = write_buffer(
        output_path.parent, vertices, wall_indices, roof_indices
    )

    nodes = []
    scene_nodes = []
    for row in range(rows):
        for column in range(columns):
            nodes.append(
                {
                    "mesh": 0,
                    "translation": [column * 2.5, 0.0, row * 2.5],
                }
            )
            scene_nodes.append(len(nodes) - 1)

    gltf = {
        "asset": {"version": "2.0", "generator": "Meridian generate_gltf_block.py"},
        "scene": 0,
        "scenes": [{"nodes": scene_nodes}],
        "nodes": nodes,
        "materials": [
            {"name": "stone_wall"},
            {"name": "roof_cap"},
        ],
        "meshes": [
            {
                "name": "building_block",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0},
                        "indices": 1,
                        "material": 0,
                    },
                    {
                        "attributes": {"POSITION": 0},
                        "indices": 2,
                        "material": 1,
                    },
                ],
            }
        ],
        "buffers": [
            {
                "uri": buffer_uri,
                "byteLength": (output_path.parent / buffer_uri).stat().st_size,
            }
        ],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }

    output_path.write_text(json.dumps(gltf, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic glTF architectural benchmark"
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--columns", type=int, default=5)
    parser.add_argument("--rows", type=int, default=4)
    args = parser.parse_args()

    if args.columns < 1 or args.rows < 1:
        raise SystemExit("columns and rows must be positive")

    write_gltf(Path(args.output), args.columns, args.rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

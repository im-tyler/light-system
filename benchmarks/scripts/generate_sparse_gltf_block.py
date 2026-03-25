#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def write_sparse_gltf(output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    positions = [
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
    sparse_indices = [2, 6]
    sparse_values = [
        2.0,
        3.1,
        0.0,
        2.0,
        3.1,
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

    position_bytes = struct.pack(f"<{len(positions)}f", *positions)
    sparse_index_bytes = struct.pack(f"<{len(sparse_indices)}H", *sparse_indices)
    sparse_value_bytes = struct.pack(f"<{len(sparse_values)}f", *sparse_values)
    wall_index_bytes = struct.pack(f"<{len(wall_indices)}I", *wall_indices)
    roof_index_bytes = struct.pack(f"<{len(roof_indices)}I", *roof_indices)

    blob = (
        position_bytes
        + sparse_index_bytes
        + sparse_value_bytes
        + wall_index_bytes
        + roof_index_bytes
    )
    buffer_path = output_path.parent / "sparse_building_block.bin"
    buffer_path.write_bytes(blob)

    sparse_index_offset = len(position_bytes)
    sparse_value_offset = sparse_index_offset + len(sparse_index_bytes)
    wall_index_offset = sparse_value_offset + len(sparse_value_bytes)
    roof_index_offset = wall_index_offset + len(wall_index_bytes)

    gltf = {
        "asset": {
            "version": "2.0",
            "generator": "Meridian generate_sparse_gltf_block.py",
        },
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"mesh": 0, "translation": [0.0, 0.0, 0.0]},
            {"mesh": 0, "translation": [3.0, 0.0, 0.0]},
        ],
        "materials": [{}, {}],
        "meshes": [
            {
                "name": "sparse_building_block",
                "primitives": [
                    {"attributes": {"POSITION": 0}, "indices": 3, "material": 0},
                    {"attributes": {"POSITION": 0}, "indices": 4, "material": 1},
                ],
            }
        ],
        "buffers": [{"uri": buffer_path.name, "byteLength": len(blob)}],
        "bufferViews": [
            {
                "buffer": 0,
                "byteOffset": 0,
                "byteLength": len(position_bytes),
                "target": 34962,
            },
            {
                "buffer": 0,
                "byteOffset": sparse_index_offset,
                "byteLength": len(sparse_index_bytes),
            },
            {
                "buffer": 0,
                "byteOffset": sparse_value_offset,
                "byteLength": len(sparse_value_bytes),
            },
            {
                "buffer": 0,
                "byteOffset": wall_index_offset,
                "byteLength": len(wall_index_bytes),
                "target": 34963,
            },
            {
                "buffer": 0,
                "byteOffset": roof_index_offset,
                "byteLength": len(roof_index_bytes),
                "target": 34963,
            },
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": 8,
                "type": "VEC3",
                "min": [0.0, 0.0, 0.0],
                "max": [2.0, 3.1, 2.0],
                "sparse": {
                    "count": 2,
                    "indices": {"bufferView": 1, "componentType": 5123},
                    "values": {"bufferView": 2},
                },
            },
            {"bufferView": 1, "componentType": 5123, "count": 2, "type": "SCALAR"},
            {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC3"},
            {
                "bufferView": 3,
                "componentType": 5125,
                "count": len(wall_indices),
                "type": "SCALAR",
            },
            {
                "bufferView": 4,
                "componentType": 5125,
                "count": len(roof_indices),
                "type": "SCALAR",
            },
        ],
    }

    output_path.write_text(json.dumps(gltf, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic sparse glTF benchmark asset"
    )
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    write_sparse_gltf(Path(args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

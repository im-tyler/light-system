#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def write_uv_seam_gltf(output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    positions = [
        -1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        -1.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        1.0,
        1.0,
        0.0,
        0.0,
        1.0,
        0.0,
    ]
    normals = [0.0, 0.0, 1.0] * 8
    texcoords = [
        0.0,
        0.0,
        1.0,
        0.0,
        1.0,
        1.0,
        0.0,
        1.0,
        0.0,
        0.0,
        1.0,
        0.0,
        1.0,
        1.0,
        0.0,
        1.0,
    ]
    indices = [0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7]

    position_bytes = struct.pack(f"<{len(positions)}f", *positions)
    normal_bytes = struct.pack(f"<{len(normals)}f", *normals)
    texcoord_bytes = struct.pack(f"<{len(texcoords)}f", *texcoords)
    index_bytes = struct.pack(f"<{len(indices)}I", *indices)
    blob = position_bytes + normal_bytes + texcoord_bytes + index_bytes

    buffer_path = output_path.parent / "uv_seam_plane.bin"
    buffer_path.write_bytes(blob)

    normal_offset = len(position_bytes)
    texcoord_offset = normal_offset + len(normal_bytes)
    index_offset = texcoord_offset + len(texcoord_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "Meridian generate_uv_seam_gltf.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "materials": [{"name": "stone_wall"}],
        "meshes": [
            {
                "name": "uv_seam_plane",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                        "indices": 3,
                        "material": 0,
                    }
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
                "byteOffset": normal_offset,
                "byteLength": len(normal_bytes),
                "target": 34962,
            },
            {
                "buffer": 0,
                "byteOffset": texcoord_offset,
                "byteLength": len(texcoord_bytes),
                "target": 34962,
            },
            {
                "buffer": 0,
                "byteOffset": index_offset,
                "byteLength": len(index_bytes),
                "target": 34963,
            },
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": 8,
                "type": "VEC3",
                "min": [-1.0, 0.0, 0.0],
                "max": [1.0, 1.0, 0.0],
            },
            {"bufferView": 1, "componentType": 5126, "count": 8, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 8, "type": "VEC2"},
            {
                "bufferView": 3,
                "componentType": 5125,
                "count": len(indices),
                "type": "SCALAR",
            },
        ],
    }

    output_path.write_text(json.dumps(gltf, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic UV-seam glTF asset"
    )
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    write_uv_seam_gltf(Path(args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
